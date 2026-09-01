// SPDX-License-Identifier: LGPL-3.0-or-later
#include "hidmaestro_adapter.h"

#include "pointer_inject.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace {

namespace hm = satellite::hidmaestro;

inline bool isValidSerial(uint32_t serial) {
    return serial >= 1 && serial <= MAX_BACKEND_CONTROLLERS;
}

inline HANDLE toHandle(uint64_t v) { return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(v)); }

} // namespace

HidMaestroAdapter::HidMaestroAdapter(hm::IHidMaestroProvisioner& provisioner,
                                     AudioEnabledFn audioEnabled)
    : provisioner_(provisioner), audioEnabled_(std::move(audioEnabled)) {}

HidMaestroAdapter::~HidMaestroAdapter() {
    closeBus();
    provisioner_.shutdown();
}

// "Bus open" = believed usable. Deliberately prompt-free: the elevated helper
// (and its UAC prompt) is deferred to the first actual plug so a ViGEm-only
// session never sees it.
bool HidMaestroAdapter::ensureBusOpen() {
    std::lock_guard<std::mutex> lk(busMtx_);
    if (busOpen_) return true;
    busOpen_ = provisioner_.isReady() || provisioner_.installed();
    return busOpen_;
}

void HidMaestroAdapter::closeBus() {
    std::vector<uint32_t> serials;
    {
        std::lock_guard<std::mutex> lk(busMtx_);
        serials.reserve(outputWorkers_.size() + audioWorkers_.size());
        for (auto& [serial, _] : outputWorkers_) serials.push_back(serial);
        for (auto& [serial, _] : audioWorkers_) serials.push_back(serial);
    }
    for (uint32_t serial : serials) {
        std::lock_guard<std::mutex> lk(busMtx_);
        stopOutputWorker(serial);
        stopAudioWorker(serial);
    }

    std::lock_guard<std::mutex> lk(busMtx_);
    for (uint32_t s = 1; s <= MAX_BACKEND_CONTROLLERS; s++) {
        IoSlot& slot = io_[s];
        if (slot.plugged.load(std::memory_order_acquire)) provisioner_.deprovision(s);
        releaseSlotLocked(slot);
    }
    // The helper stays resident: closeBus fires every time the session goes
    // idle, and respawning the elevated helper would mean a fresh UAC prompt
    // on every reconnect. It exits with satellite (parent watch) or in the
    // destructor.
    busOpen_ = false;
}

bool HidMaestroAdapter::isBusOpen() const {
    std::lock_guard<std::mutex> lk(busMtx_);
    return busOpen_;
}

bool HidMaestroAdapter::supportsIdentity(GamepadIdentity identity) const {
    return hm::profileForIdentity(identity) != nullptr;
}

// Caller holds busMtx_. Unmaps + closes everything; plugged must already be
// false (no submit can be mid-write because submits hold busMtx_ too).
void HidMaestroAdapter::releaseSlotLocked(IoSlot& slot) {
    slot.plugged.store(false, std::memory_order_release);
    if (slot.inputView) UnmapViewOfFile(slot.inputView);
    if (slot.outputView) UnmapViewOfFile(const_cast<uint8_t*>(slot.outputView));
    if (slot.speakerView) UnmapViewOfFile(const_cast<uint8_t*>(slot.speakerView));
    if (slot.micView) UnmapViewOfFile(slot.micView);
    for (HANDLE h : {slot.inputSection, slot.inputEvent, slot.companionEvent, slot.outputSection,
                     slot.outputEvent, slot.speakerSection, slot.speakerEvent, slot.micSection,
                     slot.micEvent}) {
        if (h) CloseHandle(h);
    }
    slot.inputView = nullptr;
    slot.outputView = nullptr;
    slot.speakerView = nullptr;
    slot.micView = nullptr;
    slot.inputSection = slot.inputEvent = slot.companionEvent = nullptr;
    slot.outputSection = slot.outputEvent = nullptr;
    slot.speakerSection = slot.speakerEvent = slot.micSection = slot.micEvent = nullptr;
    slot.speakerRateHz = slot.micRateHz = 0;
    slot.micResampler = {};
    slot.micScratch.clear();
    slot.micSeq = 0;
    slot.ds4 = {};
    slot.ds5 = {};
    slot.switchPad = {};
    slot.switchMotion = {};
    slot.fingerDown0 = slot.fingerDown1 = false;
    slot.lastSonySubmit = {};
}

bool HidMaestroAdapter::pluginDevice(uint32_t serial, GamepadIdentity identity) {
    if (!isValidSerial(serial) || !supportsIdentity(identity)) return false;
    std::lock_guard<std::mutex> lk(busMtx_);
    if (!busOpen_) return false;
    if (!provisioner_.isReady() && !provisioner_.ensureReady()) return false;

    // Ask for the audio-carrying persona when the setting allows it and the
    // identity has one. It can fail on its own (the composite rides a kernel
    // USB transport that self-installs on first use, and that install can be
    // declined or blocked), so a failure falls back to the plain persona: a
    // pad without audio beats no pad at all.
    const bool wantAudio =
        hm::identityHasAudioPersona(identity) && audioEnabled_ && audioEnabled_();
    hm::ProvisionResult r;
    if (!provisioner_.provision(serial, identity, wantAudio, r)) {
        if (!wantAudio || !provisioner_.provision(serial, identity, false, r)) return false;
    }

    IoSlot& slot = io_[serial];
    slot.identity = identity;
    slot.inputSection = toHandle(r.inputSection);
    slot.inputEvent = toHandle(r.inputEvent);
    slot.companionEvent = toHandle(r.companionEvent);
    slot.outputSection = toHandle(r.outputSection);
    slot.outputEvent = toHandle(r.outputEvent);

    slot.inputView = static_cast<uint8_t*>(MapViewOfFile(
        slot.inputSection, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, hm::INPUT_SECTION_SIZE));
    if (slot.outputSection) {
        slot.outputView = static_cast<const uint8_t*>(
            MapViewOfFile(slot.outputSection, FILE_MAP_READ, 0, 0, hm::OUTPUT_SECTION_SIZE));
    }
    attachAudioLocked(slot, r);
    if (slot.inputView == nullptr) {
        releaseSlotLocked(slot);
        provisioner_.deprovision(serial);
        return false;
    }

    slot.plugged.store(true, std::memory_order_release);

    // Neutral first frame so the pad isn't a stuck corner pre-first-report
    // (GamepadReport{} is centred on the signed XUSB scale; the Sony packers
    // centre their byte sticks from it).
    packAndWriteLocked(slot);

    if (slot.outputView != nullptr) startOutputWorker(serial);
    if (slot.speakerView != nullptr) startAudioWorker(serial);
    return true;
}

// Caller holds busMtx_. Every failure here degrades to "no audio on this pad"
// rather than failing the plug: the input path is the thing the session
// actually needs. Mapping asks for exactly AUDIO_SECTION_SIZE, so a helper
// built against a different ring layout fails the map instead of misreading
// PCM (the section SIZE is the layout check, same rule as the driver's).
void HidMaestroAdapter::attachAudioLocked(IoSlot& slot, const hm::ProvisionResult& r) {
    slot.speakerSection = toHandle(r.speakerSection);
    slot.speakerEvent = toHandle(r.speakerEvent);
    slot.micSection = toHandle(r.micSection);
    slot.micEvent = toHandle(r.micEvent);
    slot.speakerRateHz = r.speakerRateHz;
    slot.micRateHz = r.micRateHz;

    if (slot.speakerSection) {
        slot.speakerView = static_cast<const uint8_t*>(
            MapViewOfFile(slot.speakerSection, FILE_MAP_READ, 0, 0, hm::AUDIO_SECTION_SIZE));
    }
    if (slot.micSection) {
        slot.micView = static_cast<uint8_t*>(MapViewOfFile(
            slot.micSection, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, hm::AUDIO_SECTION_SIZE));
    }
    if (slot.micView != nullptr) {
        // A persona that reported no rate is taken at the wire rate, which
        // configures the identity: the alternative (guessing) would silently
        // pitch-shift the stream.
        slot.micResampler.configure(AUDIO_SAMPLE_RATE_HZ,
                                    slot.micRateHz > 0 ? slot.micRateHz : AUDIO_SAMPLE_RATE_HZ,
                                    AUDIO_MIC_CHANNELS);
    }
}

bool HidMaestroAdapter::unplugDevice(uint32_t serial) {
    if (!isValidSerial(serial)) return true; // nothing to remove

    {
        std::lock_guard<std::mutex> lk(busMtx_);
        stopOutputWorker(serial);
        stopAudioWorker(serial);
    }

    std::lock_guard<std::mutex> lk(busMtx_);
    IoSlot& slot = io_[serial];
    const bool wasPlugged = slot.plugged.load(std::memory_order_acquire);
    releaseSlotLocked(slot);
    if (!wasPlugged) return true;
    // False (helper gone / SDK teardown failed) means target state unknown:
    // the caller quarantines the serial, exactly like an unconfirmed ViGEm
    // unplug.
    return provisioner_.deprovision(serial);
}

bool HidMaestroAdapter::isDevicePlugged(uint32_t serial) const {
    if (!isValidSerial(serial)) return false;
    return io_[serial].plugged.load(std::memory_order_acquire);
}

// Caller holds busMtx_.
bool HidMaestroAdapter::packAndWriteLocked(IoSlot& slot) {
    if (slot.inputView == nullptr) return false;
    bool withGip = false;
    uint16_t len = 0;
    switch (slot.identity) {
    case GamepadIdentity::Xbox:
        hm::packX360Report(slot.ds4.pad, slot.payload);
        hm::packGip(slot.ds4.pad, slot.gip);
        len = hm::X360_REPORT_BYTES;
        withGip = true;
        break;
    case GamepadIdentity::DS4: {
        const auto now = std::chrono::steady_clock::now();
        if (slot.lastSonySubmit.time_since_epoch().count() != 0) {
            const auto us =
                std::chrono::duration_cast<std::chrono::microseconds>(now - slot.lastSonySubmit)
                    .count();
            slot.ds4.timestamp = static_cast<uint16_t>(slot.ds4.timestamp + (us * 3) / 16);
        }
        slot.lastSonySubmit = now;
        slot.ds4.frameCounter = static_cast<uint8_t>((slot.ds4.frameCounter + 1) & 0x3F);
        hm::packDs4Payload(slot.ds4, slot.payload);
        len = hm::DS4_PAYLOAD_BYTES;
        break;
    }
    case GamepadIdentity::DualSense: {
        const auto now = std::chrono::steady_clock::now();
        if (slot.lastSonySubmit.time_since_epoch().count() != 0) {
            const auto us =
                std::chrono::duration_cast<std::chrono::microseconds>(now - slot.lastSonySubmit)
                    .count();
            slot.ds5.sensorTimestamp += static_cast<uint32_t>(us * 3);
        }
        slot.lastSonySubmit = now;
        slot.ds5.seq++;
        hm::packDs5Payload(slot.ds5, slot.payload);
        len = hm::DS5_PAYLOAD_BYTES;
        break;
    }
    case GamepadIdentity::SwitchPro:
        hm::packSwitchBody(slot.switchPad, slot.switchMotion, slot.payload);
        len = hm::SWITCH_BODY_BYTES;
        break;
    }

    if (!hm::writeInputFrame(slot.inputView, slot.payload, len, withGip ? slot.gip : nullptr))
        return false;
    if (slot.inputEvent) SetEvent(slot.inputEvent);
    if (withGip && slot.companionEvent) SetEvent(slot.companionEvent);
    return true;
}

bool HidMaestroAdapter::submitReport(uint32_t serial, const GamepadReport& report) {
    if (!isValidSerial(serial)) return false;
    std::lock_guard<std::mutex> lk(busMtx_);
    IoSlot& slot = io_[serial];
    if (!slot.plugged.load(std::memory_order_relaxed)) return false;

    switch (slot.identity) {
    case GamepadIdentity::Xbox:
    case GamepadIdentity::DS4:
        slot.ds4.pad = report;
        break;
    case GamepadIdentity::DualSense:
        slot.ds5.pad = report;
        break;
    case GamepadIdentity::SwitchPro:
        slot.switchPad = report;
        break;
    }
    return packAndWriteLocked(slot);
}

bool HidMaestroAdapter::submitMotion(uint32_t serial, const MotionReport& report) {
    if (!isValidSerial(serial)) return false;
    std::lock_guard<std::mutex> lk(busMtx_);
    IoSlot& slot = io_[serial];
    if (!slot.plugged.load(std::memory_order_relaxed)) return false;

    switch (slot.identity) {
    case GamepadIdentity::DS4:
        slot.ds4.motion = report;
        break;
    case GamepadIdentity::DualSense:
        slot.ds5.motion = report;
        break;
    case GamepadIdentity::SwitchPro:
        slot.switchMotion = report;
        break;
    default:
        return false;
    }
    return packAndWriteLocked(slot);
}

bool HidMaestroAdapter::submitBattery(uint32_t serial, const BatteryReport& report) {
    if (!isValidSerial(serial)) return false;
    std::lock_guard<std::mutex> lk(busMtx_);
    IoSlot& slot = io_[serial];
    if (!slot.plugged.load(std::memory_order_relaxed)) return false;

    switch (slot.identity) {
    case GamepadIdentity::DS4:
        slot.ds4.batteryByte = ds4BatteryByte(report);
        break;
    case GamepadIdentity::DualSense:
        slot.ds5.batteryByte = hm::ds5BatteryByte(report);
        break;
    default:
        // Xbox has no battery surface on the wired profile; Switch battery
        // bytes are stamped by the driver's streamer.
        return false;
    }
    return packAndWriteLocked(slot);
}

bool HidMaestroAdapter::submitTouchpad(uint32_t serial, const TouchpadReport& report) {
    if (!isValidSerial(serial)) return false;
    std::lock_guard<std::mutex> lk(busMtx_);
    IoSlot& slot = io_[serial];
    if (!slot.plugged.load(std::memory_order_relaxed)) return false;
    if (slot.identity != GamepadIdentity::DS4 && slot.identity != GamepadIdentity::DualSense)
        return false;

    // Bump tracking id on up->down so a consumer reads a new contact, not a
    // teleporting drag.
    if (report.finger0.active && !slot.fingerDown0) {
        slot.ds4.touchTrackingId0 = static_cast<uint8_t>((slot.ds4.touchTrackingId0 + 1) & 0x7F);
        slot.ds5.touchTrackingId0 = static_cast<uint8_t>((slot.ds5.touchTrackingId0 + 1) & 0x7F);
    }
    if (report.finger1.active && !slot.fingerDown1) {
        slot.ds4.touchTrackingId1 = static_cast<uint8_t>((slot.ds4.touchTrackingId1 + 1) & 0x7F);
        slot.ds5.touchTrackingId1 = static_cast<uint8_t>((slot.ds5.touchTrackingId1 + 1) & 0x7F);
    }
    slot.fingerDown0 = report.finger0.active;
    slot.fingerDown1 = report.finger1.active;

    if (slot.identity == GamepadIdentity::DS4) {
        slot.ds4.finger0 = report.finger0;
        slot.ds4.finger1 = report.finger1;
        slot.ds4.touchpadButtonPressed = report.buttonPressed;
        slot.ds4.touchPacketCounter++;
    } else {
        slot.ds5.finger0 = report.finger0;
        slot.ds5.finger1 = report.finger1;
        slot.ds5.touchpadButtonPressed = report.buttonPressed;
    }
    return packAndWriteLocked(slot);
}

bool HidMaestroAdapter::submitRelativeMouse(int dx, int dy, const MouseButtons& buttons,
                                            int wheelV) {
    return injectRelativeMouse(relMouseBtns_, dx, dy, buttons, wheelV);
}

bool HidMaestroAdapter::supportsMotionForType(uint8_t controllerType) const {
    return controllerTypeHasMotion(controllerType);
}

void HidMaestroAdapter::setRumbleCallback(RumbleCallback cb) {
    std::lock_guard<std::mutex> lk(busMtx_);
    rumbleCb_ = std::move(cb);
}

void HidMaestroAdapter::setLightbarCallback(LightbarCallback cb) {
    std::lock_guard<std::mutex> lk(busMtx_);
    lightbarCb_ = std::move(cb);
}

void HidMaestroAdapter::setTriggerEffectsCallback(TriggerEffectsCallback cb) {
    std::lock_guard<std::mutex> lk(busMtx_);
    triggerEffectsCb_ = std::move(cb);
}

void HidMaestroAdapter::setPlayerLedsCallback(PlayerLedsCallback cb) {
    std::lock_guard<std::mutex> lk(busMtx_);
    playerLedsCb_ = std::move(cb);
}

void HidMaestroAdapter::setSpeakerAudioCallback(SpeakerAudioCallback cb) {
    std::lock_guard<std::mutex> lk(busMtx_);
    speakerAudioCb_ = std::move(cb);
}

void HidMaestroAdapter::setMicLedCallback(MicLedCallback cb) {
    std::lock_guard<std::mutex> lk(busMtx_);
    micLedCb_ = std::move(cb);
}

bool HidMaestroAdapter::hasSpeakerEndpoint(uint32_t serial) const {
    if (!isValidSerial(serial)) return false;
    std::lock_guard<std::mutex> lk(busMtx_);
    return io_[serial].speakerView != nullptr;
}

bool HidMaestroAdapter::hasMicEndpoint(uint32_t serial) const {
    if (!isValidSerial(serial)) return false;
    std::lock_guard<std::mutex> lk(busMtx_);
    return io_[serial].micView != nullptr;
}

// One 20 ms window of the client's microphone, on its way to the pad's mic
// endpoint. Rate-converted here (the endpoint is 16 kHz on the DualShock 4 v2
// persona) and handed to the helper as endpoint-rate mono; the helper only has
// to spread it across the endpoint's channels. False = this pad has no mic
// endpoint, which is a normal state and not an error the sender should act on.
bool HidMaestroAdapter::submitMicAudioPcm(uint32_t serial, const int16_t* mono48k, size_t samples) {
    if (!isValidSerial(serial) || mono48k == nullptr || samples == 0) return false;
    std::lock_guard<std::mutex> lk(busMtx_);
    IoSlot& slot = io_[serial];
    if (!slot.plugged.load(std::memory_order_relaxed) || slot.micView == nullptr) return false;

    slot.micScratch.clear();
    slot.micResampler.process(mono48k, samples, slot.micScratch);
    // A downsampler can legitimately produce nothing from a short first chunk
    // while its filter fills; that is delivered, not dropped.
    if (slot.micScratch.empty()) return true;

    // One window never fills a slot, but chunking rather than truncating means
    // an over-long batch degrades to extra slots instead of clipped audio.
    size_t offset = 0;
    while (offset < slot.micScratch.size()) {
        const size_t chunk =
            std::min(slot.micScratch.size() - offset, hm::AUDIO_SLOT_SAMPLE_CAPACITY);
        if (!hm::writeAudioSlot(slot.micView, serial, slot.micSeq, slot.micScratch.data() + offset,
                                static_cast<uint16_t>(chunk))) {
            return false;
        }
        slot.micSeq++;
        offset += chunk;
    }
    if (slot.micEvent) SetEvent(slot.micEvent);
    return true;
}

// Caller holds busMtx_. The ring baseline is snapshotted HERE, under the plug
// lock, so a packet published the instant the plug returns is never skipped —
// the worker starting from "whatever the head is once my thread runs" would
// race exactly that window (while still skipping any pre-plug stale packets).
void HidMaestroAdapter::startOutputWorker(uint32_t serial) {
    uint32_t baseline = 0;
    std::memcpy(&baseline, io_[serial].outputView, sizeof(baseline));
    auto& w = outputWorkers_[serial];
    w.cancel = CreateEventW(nullptr, TRUE /* manual reset */, FALSE, nullptr);
    HANDLE cancelHandle = w.cancel;
    w.th = std::thread(
        [this, serial, cancelHandle, baseline] { outputLoop(serial, cancelHandle, baseline); });
}

// Caller holds busMtx_.
void HidMaestroAdapter::stopOutputWorker(uint32_t serial) {
    auto it = outputWorkers_.find(serial);
    if (it == outputWorkers_.end()) return;
    OutputWorker w = std::move(it->second);
    outputWorkers_.erase(it);
    if (w.cancel) SetEvent(w.cancel);
    // Drop + reacquire busMtx_ around the join so the worker's own lock
    // acquisition (for the callback copy) doesn't deadlock.
    busMtx_.unlock();
    if (w.th.joinable()) w.th.join();
    if (w.cancel) CloseHandle(w.cancel);
    busMtx_.lock();
}

void HidMaestroAdapter::outputLoop(uint32_t serial, HANDLE cancel, uint32_t lastSeq) {
    const uint8_t* view;
    HANDLE doorbell;
    GamepadIdentity identity;
    {
        std::lock_guard<std::mutex> lk(busMtx_);
        IoSlot& slot = io_[serial];
        view = slot.outputView;
        doorbell = slot.outputEvent;
        identity = slot.identity;
    }
    if (view == nullptr) return;

    // The driver signals the doorbell once per published packet; the timeout
    // is a safety net (and the whole cadence for pre-doorbell drivers).
    HANDLE waits[2] = {cancel, doorbell};
    const DWORD waitCount = doorbell ? 2 : 1;
    const DWORD timeoutMs = doorbell ? 500 : 8;

    TriggerEffectsReport triggerEffects{};

    while (true) {
        const DWORD rc = WaitForMultipleObjects(waitCount, waits, FALSE, timeoutMs);
        if (rc == WAIT_OBJECT_0) return; // cancelled
        if (rc == WAIT_FAILED) return;

        hm::OutputPacket pkt;
        while (hm::readNextOutputPacket(view, lastSeq, pkt)) {
            const hm::DecodedOutput decoded = hm::decodeOutputPacket(identity, pkt);
            if (!decoded.hasRumble && !decoded.hasLightbar && !decoded.hasLeftTriggerEffect &&
                !decoded.hasRightTriggerEffect && !decoded.hasPlayerLeds && !decoded.hasMicLed) {
                continue;
            }
            RumbleCallback rcb;
            LightbarCallback lcb;
            TriggerEffectsCallback tcb;
            PlayerLedsCallback pcb;
            MicLedCallback mcb;
            {
                std::lock_guard<std::mutex> lk(busMtx_);
                rcb = rumbleCb_;
                lcb = lightbarCb_;
                tcb = triggerEffectsCb_;
                pcb = playerLedsCb_;
                mcb = micLedCb_;
            }
            if (decoded.hasRumble && rcb) rcb(serial, decoded.rumble);
            if (decoded.hasLightbar && lcb) lcb(serial, decoded.r, decoded.g, decoded.b);
            if (decoded.hasLeftTriggerEffect || decoded.hasRightTriggerEffect) {
                // Merge per-trigger valid flags into this worker's cache: the
                // report always carries BOTH blocks downstream. The cache lives
                // on the loop stack, so a replug starts from neutral effects,
                // matching the fresh-actuator reset in SessionService.
                if (decoded.hasLeftTriggerEffect) {
                    std::memcpy(triggerEffects.left, decoded.leftTriggerEffect,
                                TRIGGER_EFFECT_BLOCK_BYTES);
                }
                if (decoded.hasRightTriggerEffect) {
                    std::memcpy(triggerEffects.right, decoded.rightTriggerEffect,
                                TRIGGER_EFFECT_BLOCK_BYTES);
                }
                if (tcb) tcb(serial, triggerEffects);
            }
            if (decoded.hasPlayerLeds && pcb) pcb(serial, decoded.playerLeds);
            if (decoded.hasMicLed && mcb) mcb(serial, decoded.micLed);
        }
    }
}

// Caller holds busMtx_. Same baseline-under-the-plug-lock rule as the output
// worker: the helper can publish a batch the instant the plug returns.
void HidMaestroAdapter::startAudioWorker(uint32_t serial) {
    const uint32_t baseline = hm::audioRingHead(io_[serial].speakerView);
    auto& w = audioWorkers_[serial];
    w.cancel = CreateEventW(nullptr, TRUE /* manual reset */, FALSE, nullptr);
    HANDLE cancelHandle = w.cancel;
    w.th = std::thread(
        [this, serial, cancelHandle, baseline] { audioLoop(serial, cancelHandle, baseline); });
}

// Caller holds busMtx_.
void HidMaestroAdapter::stopAudioWorker(uint32_t serial) {
    auto it = audioWorkers_.find(serial);
    if (it == audioWorkers_.end()) return;
    OutputWorker w = std::move(it->second);
    audioWorkers_.erase(it);
    if (w.cancel) SetEvent(w.cancel);
    // Drop + reacquire busMtx_ around the join for the same reason the output
    // worker does: the worker takes the lock to snapshot the callback.
    busMtx_.unlock();
    if (w.th.joinable()) w.th.join();
    if (w.cancel) CloseHandle(w.cancel);
    busMtx_.lock();
}

// Speaker ring drain: endpoint-rate stereo in, wire-rate stereo out to the
// SAT-2 backend callback, which re-windows whatever batch size arrives into
// 20 ms Opus frames. The resampler is loop-local, so a replug starts from a
// clean filter rather than the tail of the previous pad's audio.
void HidMaestroAdapter::audioLoop(uint32_t serial, HANDLE cancel, uint32_t lastSeq) {
    const uint8_t* view;
    HANDLE doorbell;
    int rateHz;
    {
        std::lock_guard<std::mutex> lk(busMtx_);
        IoSlot& slot = io_[serial];
        view = slot.speakerView;
        doorbell = slot.speakerEvent;
        rateHz = slot.speakerRateHz;
    }
    if (view == nullptr) return;

    satellite::audio::RationalResampler resampler;
    resampler.configure(rateHz > 0 ? rateHz : AUDIO_SAMPLE_RATE_HZ, AUDIO_SAMPLE_RATE_HZ,
                        AUDIO_SPEAKER_CHANNELS);

    // The helper signals once per published batch; the timeout is the same
    // safety net the output worker uses for a doorbell that never rings.
    HANDLE waits[2] = {cancel, doorbell};
    const DWORD waitCount = doorbell ? 2 : 1;
    const DWORD timeoutMs = 500;

    hm::AudioPacket pkt;
    std::vector<int16_t> pcm;

    while (true) {
        const DWORD rc = WaitForMultipleObjects(waitCount, waits, FALSE, timeoutMs);
        if (rc == WAIT_OBJECT_0) return; // cancelled
        if (rc == WAIT_FAILED) return;

        while (hm::readNextAudioSlot(view, lastSeq, pkt)) {
            // A batch stamped with someone else's serial means the section was
            // aliased or reused; dropping it is safer than playing it out on
            // the wrong controller's stream.
            if (pkt.serial != serial) continue;
            const size_t frames = pkt.sampleCount / AUDIO_SPEAKER_CHANNELS;
            if (frames == 0) continue;
            pcm.clear();
            resampler.process(pkt.data, frames, pcm);
            if (pcm.empty()) continue;

            SpeakerAudioCallback cb;
            {
                std::lock_guard<std::mutex> lk(busMtx_);
                cb = speakerAudioCb_;
            }
            if (cb) cb(serial, pcm.data(), pcm.size() / AUDIO_SPEAKER_CHANNELS);
        }
    }
}
