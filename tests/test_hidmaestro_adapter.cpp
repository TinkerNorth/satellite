// SPDX-License-Identifier: LGPL-3.0-or-later

// Drives the real hidmaestro_adapter.cpp against a fake provisioner whose
// "sections" are in-process anonymous file mappings, so the seqlock submit
// path, per-profile packing, merged motion/touch/battery state, plug/unplug
// bookkeeping, the output-ring rumble/lightbar/mic-LED worker and the
// controller-audio pump in both directions run without the driver, the helper
// process, or elevation.
//
// The fake NEVER creates a real composite persona: doing so would install
// HIDMaestro's bundled kernel USB transport. What it fakes is the shape of the
// helper's answer (audio sections + endpoint formats), which is all the adapter
// consumes.
#include "test_util.h"

#include "../src/platform/windows/hidmaestro_adapter.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace satellite::hidmaestro;

namespace {

uint32_t u32at(const uint8_t* p, size_t off) {
    uint32_t v;
    std::memcpy(&v, p + off, sizeof(v));
    return v;
}

void storeU32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, sizeof(v)); }
void storeU16(uint8_t* p, uint16_t v) { std::memcpy(p, &v, sizeof(v)); }

struct FakeProvisioner : IHidMaestroProvisioner {
    struct Fake {
        HANDLE inputSection = nullptr;
        HANDLE outputSection = nullptr;
        uint8_t* inputView = nullptr;
        uint8_t* outputView = nullptr;
        HANDLE inputEvent = nullptr;
        HANDLE companionEvent = nullptr;
        HANDLE outputEvent = nullptr;
        HANDLE speakerSection = nullptr;
        HANDLE micSection = nullptr;
        uint8_t* speakerView = nullptr;
        uint8_t* micView = nullptr;
        HANDLE speakerEvent = nullptr;
        HANDLE micEvent = nullptr;
        uint32_t micLastSeq = 0; // this side's drain cursor over the mic ring
        uint16_t speakerSeq = 0;
    };

    bool installedVal = true;
    bool ready = false;
    bool ensureReadyResult = true;
    bool provisionResult = true;
    bool deprovisionResult = true;
    bool withOutputSection = true;
    // What a composite persona reports back. 48/48 is the DualSense; the
    // DualShock 4 v2 is 32 kHz out / 16 kHz in, which is what forces the
    // adapter's resamplers to exist.
    bool audioAvailable = true;
    bool failAudioProvision = false;
    int speakerRateHz = 48000;
    int micRateHz = 48000;
    int micChannels = 1;
    int provisionCalls = 0;
    int audioProvisionCalls = 0;
    int deprovisionCalls = 0;
    int shutdownCalls = 0;
    GamepadIdentity lastIdentity = GamepadIdentity::Xbox;
    bool lastAudioRequested = false;
    std::map<uint32_t, Fake> live;

    ~FakeProvisioner() override {
        for (auto& [serial, f] : live) destroy(f);
        live.clear();
    }

    static HANDLE dup(HANDLE h) {
        HANDLE out = nullptr;
        DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &out, 0, FALSE,
                        DUPLICATE_SAME_ACCESS);
        return out;
    }

    static void destroy(Fake& f) {
        if (f.inputView) UnmapViewOfFile(f.inputView);
        if (f.outputView) UnmapViewOfFile(f.outputView);
        if (f.speakerView) UnmapViewOfFile(f.speakerView);
        if (f.micView) UnmapViewOfFile(f.micView);
        for (HANDLE h :
             {f.inputSection, f.outputSection, f.inputEvent, f.companionEvent, f.outputEvent,
              f.speakerSection, f.micSection, f.speakerEvent, f.micEvent}) {
            if (h) CloseHandle(h);
        }
        f = Fake{};
    }

    bool installed() const override { return installedVal; }
    bool isReady() const override { return ready; }
    bool ensureReady() override {
        if (!ensureReadyResult) return false;
        ready = true;
        return true;
    }
    void shutdown() override {
        shutdownCalls++;
        ready = false;
    }

    bool provision(uint32_t serial, GamepadIdentity identity, bool audio,
                   ProvisionResult& out) override {
        provisionCalls++;
        lastIdentity = identity;
        lastAudioRequested = audio;
        if (audio) audioProvisionCalls++;
        if (!provisionResult) return false;
        // A composite persona can fail on its own (the kernel transport it
        // rides self-installs and that install can be declined); the adapter
        // must then retry without audio.
        if (audio && failAudioProvision) return false;

        Fake f;
        f.inputSection = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                            INPUT_SECTION_SIZE, nullptr);
        f.inputView = static_cast<uint8_t*>(
            MapViewOfFile(f.inputSection, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0));
        f.inputEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        f.companionEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (withOutputSection) {
            f.outputSection = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                                 OUTPUT_SECTION_SIZE, nullptr);
            f.outputView = static_cast<uint8_t*>(
                MapViewOfFile(f.outputSection, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0));
            f.outputEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        }

        if (audio && audioAvailable) {
            f.speakerSection = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                                  AUDIO_SECTION_SIZE, nullptr);
            f.speakerView = static_cast<uint8_t*>(
                MapViewOfFile(f.speakerSection, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0));
            f.micSection = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                              AUDIO_SECTION_SIZE, nullptr);
            f.micView = static_cast<uint8_t*>(
                MapViewOfFile(f.micSection, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0));
            f.speakerEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            f.micEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        }

        out.controllerIndex = serial;
        out.inputSection = reinterpret_cast<uint64_t>(dup(f.inputSection));
        out.inputEvent = reinterpret_cast<uint64_t>(dup(f.inputEvent));
        out.companionEvent = identity == GamepadIdentity::Xbox
                                 ? reinterpret_cast<uint64_t>(dup(f.companionEvent))
                                 : 0;
        out.outputSection = f.outputSection ? reinterpret_cast<uint64_t>(dup(f.outputSection)) : 0;
        out.outputEvent = f.outputEvent ? reinterpret_cast<uint64_t>(dup(f.outputEvent)) : 0;
        if (f.speakerSection) {
            out.speakerSection = reinterpret_cast<uint64_t>(dup(f.speakerSection));
            out.speakerEvent = reinterpret_cast<uint64_t>(dup(f.speakerEvent));
            out.micSection = reinterpret_cast<uint64_t>(dup(f.micSection));
            out.micEvent = reinterpret_cast<uint64_t>(dup(f.micEvent));
            out.speakerChannels = AUDIO_SPEAKER_CHANNELS;
            out.speakerRateHz = speakerRateHz;
            out.micChannels = micChannels;
            out.micRateHz = micRateHz;
        }

        auto existing = live.find(serial);
        if (existing != live.end()) destroy(existing->second);
        live[serial] = f;
        return true;
    }

    bool deprovision(uint32_t serial) override {
        deprovisionCalls++;
        auto it = live.find(serial);
        if (it != live.end()) {
            destroy(it->second);
            live.erase(it);
        }
        return deprovisionResult;
    }

    // Publish an output packet the way the driver does, then ring the doorbell.
    void publishOutput(uint32_t serial, uint8_t source, uint8_t reportId,
                       const std::vector<uint8_t>& data) {
        Fake& f = live.at(serial);
        const uint32_t newSeq = u32at(f.outputView, 0) + 1;
        storeU32(f.outputView, newSeq);
        uint8_t* slot = f.outputView + OUTPUT_HEADER_SIZE +
                        ((newSeq - 1) % OUTPUT_RING_SLOTS) * OUTPUT_SLOT_SIZE;
        slot[OUTPUT_SLOT_SOURCE_OFFSET] = source;
        slot[OUTPUT_SLOT_REPORT_ID_OFFSET] = reportId;
        storeU16(slot + OUTPUT_SLOT_SIZE_OFFSET, static_cast<uint16_t>(data.size()));
        if (!data.empty()) std::memcpy(slot + OUTPUT_SLOT_DATA_OFFSET, data.data(), data.size());
        storeU32(slot + OUTPUT_SLOT_SEQNO_OFFSET, newSeq);
        SetEvent(f.outputEvent);
    }

    // Publish speaker PCM the way the helper does: interleaved stereo at the
    // persona's own rate, then the doorbell.
    void publishSpeaker(uint32_t serial, const std::vector<int16_t>& interleaved) {
        Fake& f = live.at(serial);
        writeAudioSlot(f.speakerView, serial, f.speakerSeq++, interleaved.data(),
                       static_cast<uint16_t>(interleaved.size()));
        SetEvent(f.speakerEvent);
    }

    // Drain everything satellite has written to the mic ring, the way the
    // helper's drain thread does.
    std::vector<int16_t> drainMic(uint32_t serial) {
        Fake& f = live.at(serial);
        std::vector<int16_t> out;
        AudioPacket pkt;
        while (readNextAudioSlot(f.micView, f.micLastSeq, pkt)) {
            if (pkt.serial != serial) continue;
            out.insert(out.end(), pkt.data, pkt.data + pkt.sampleCount);
        }
        return out;
    }

    bool hasAudioSections(uint32_t serial) const { return live.at(serial).speakerView != nullptr; }

    const uint8_t* inputSection(uint32_t serial) const { return live.at(serial).inputView; }
    uint32_t inputSeq(uint32_t serial) const { return u32at(live.at(serial).inputView, 0); }
    const uint8_t* inputData(uint32_t serial) const {
        return live.at(serial).inputView + INPUT_DATA_OFFSET;
    }
};

// Latching sink for rumble/lightbar callbacks with a condition variable so
// the tests wait deterministically instead of sleeping.
struct CallbackSink {
    std::mutex m;
    std::condition_variable cv;
    int rumbleCount = 0;
    int lightbarCount = 0;
    uint32_t lastSerial = 0;
    RumbleReport lastRumble{};
    uint8_t r = 0, g = 0, b = 0;

    void install(HidMaestroAdapter& adapter) {
        adapter.setRumbleCallback([this](uint32_t serial, const RumbleReport& rr) {
            std::lock_guard<std::mutex> lk(m);
            rumbleCount++;
            lastSerial = serial;
            lastRumble = rr;
            cv.notify_all();
        });
        adapter.setLightbarCallback([this](uint32_t serial, uint8_t rr, uint8_t gg, uint8_t bb) {
            std::lock_guard<std::mutex> lk(m);
            lightbarCount++;
            lastSerial = serial;
            r = rr;
            g = gg;
            b = bb;
            cv.notify_all();
        });
    }

    bool waitRumble(int atLeast, int timeoutMs = 3000) {
        std::unique_lock<std::mutex> lk(m);
        return cv.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                           [&] { return rumbleCount >= atLeast; });
    }

    bool waitLightbar(int atLeast, int timeoutMs = 3000) {
        std::unique_lock<std::mutex> lk(m);
        return cv.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                           [&] { return lightbarCount >= atLeast; });
    }
};

// Speaker PCM and mute-lamp states arriving from the adapter's workers. Kept
// apart from CallbackSink so the audio tests can install only what they need.
struct AudioSink {
    std::mutex m;
    std::condition_variable cv;
    int speakerCalls = 0;
    int micLedCalls = 0;
    uint32_t lastSerial = 0;
    uint8_t lastMicLed = 0xFF;
    size_t totalFrames = 0;
    std::vector<int16_t> pcm; // accumulated interleaved stereo

    void install(HidMaestroAdapter& adapter) {
        adapter.setSpeakerAudioCallback(
            [this](uint32_t serial, const int16_t* stereo48k, size_t frames) {
                std::lock_guard<std::mutex> lk(m);
                speakerCalls++;
                lastSerial = serial;
                totalFrames += frames;
                pcm.insert(pcm.end(), stereo48k, stereo48k + frames * AUDIO_SPEAKER_CHANNELS);
                cv.notify_all();
            });
        adapter.setMicLedCallback([this](uint32_t serial, uint8_t state) {
            std::lock_guard<std::mutex> lk(m);
            micLedCalls++;
            lastSerial = serial;
            lastMicLed = state;
            cv.notify_all();
        });
    }

    bool waitFrames(size_t atLeast, int timeoutMs = 3000) {
        std::unique_lock<std::mutex> lk(m);
        return cv.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                           [&] { return totalFrames >= atLeast; });
    }

    bool waitMicLed(int atLeast, int timeoutMs = 3000) {
        std::unique_lock<std::mutex> lk(m);
        return cv.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                           [&] { return micLedCalls >= atLeast; });
    }
};

// Interleaved stereo tone, the shape the helper writes into the speaker ring.
std::vector<int16_t> stereoTone(int rateHz, double freqHz, size_t frames) {
    std::vector<int16_t> out(frames * 2);
    for (size_t f = 0; f < frames; ++f) {
        const double s =
            std::sin(2.0 * 3.14159265358979323846 * freqHz * (static_cast<double>(f) / rateHz));
        out[f * 2] = static_cast<int16_t>(s * 20000.0);
        out[f * 2 + 1] = static_cast<int16_t>(s * 10000.0);
    }
    return out;
}

double rmsOf(const std::vector<int16_t>& pcm, size_t channels, size_t channel, size_t skipFrames) {
    const size_t frames = pcm.size() / channels;
    if (frames <= skipFrames) return 0.0;
    double acc = 0.0;
    for (size_t f = skipFrames; f < frames; ++f) {
        const double v = pcm[f * channels + channel];
        acc += v * v;
    }
    return std::sqrt(acc / static_cast<double>(frames - skipFrames));
}

// Audio always on, for the tests that want a composite persona.
HidMaestroAdapter::AudioEnabledFn audioOn() {
    return [] { return true; };
}

} // namespace

static void test_bus_open_probe_only() {
    TEST("ensureBusOpen — passive: installed-ness only, never spawns the helper");
    FakeProvisioner prov;
    prov.installedVal = false;
    HidMaestroAdapter adapter(prov);
    EXPECT(!adapter.ensureBusOpen());
    EXPECT(!adapter.isBusOpen());

    prov.installedVal = true;
    EXPECT(adapter.ensureBusOpen());
    EXPECT(adapter.isBusOpen());
    EXPECT(!prov.ready); // ensureReady untouched
}

static void test_identity_support() {
    TEST("supportsIdentity — all four identities materialize");
    FakeProvisioner prov;
    HidMaestroAdapter adapter(prov);
    EXPECT(adapter.supportsIdentity(GamepadIdentity::Xbox));
    EXPECT(adapter.supportsIdentity(GamepadIdentity::DS4));
    EXPECT(adapter.supportsIdentity(GamepadIdentity::DualSense));
    EXPECT(adapter.supportsIdentity(GamepadIdentity::SwitchPro));
}

static void test_plugin_lifecycle() {
    TEST("pluginDevice — helper spun up lazily, neutral frame written");
    FakeProvisioner prov;
    HidMaestroAdapter adapter(prov);
    EXPECT(adapter.ensureBusOpen());
    EXPECT(adapter.pluginDevice(1, GamepadIdentity::Xbox));
    EXPECT(prov.ready);
    EXPECT_EQ(prov.provisionCalls, 1);
    EXPECT(adapter.isDevicePlugged(1));
    EXPECT(!adapter.isDevicePlugged(2));
    // Neutral frame published at plug: seq even and nonzero, 18-byte report.
    EXPECT_EQ(prov.inputSeq(1), (uint32_t)2);
    EXPECT_EQ(u32at(prov.inputSection(1), INPUT_DATASIZE_OFFSET), (uint32_t)X360_REPORT_BYTES);

    EXPECT(adapter.unplugDevice(1));
    EXPECT_EQ(prov.deprovisionCalls, 1);
    EXPECT(!adapter.isDevicePlugged(1));
}

static void test_plugin_denied_elevation() {
    TEST("pluginDevice — a declined helper start fails the plug cleanly");
    FakeProvisioner prov;
    prov.ensureReadyResult = false;
    HidMaestroAdapter adapter(prov);
    EXPECT(adapter.ensureBusOpen());
    EXPECT(!adapter.pluginDevice(1, GamepadIdentity::Xbox));
    EXPECT_EQ(prov.provisionCalls, 0);
    EXPECT(!adapter.isDevicePlugged(1));
}

static void test_plugin_provision_failure() {
    TEST("pluginDevice — provision failure leaves no slot state behind");
    FakeProvisioner prov;
    prov.provisionResult = false;
    HidMaestroAdapter adapter(prov);
    EXPECT(adapter.ensureBusOpen());
    EXPECT(!adapter.pluginDevice(1, GamepadIdentity::DS4));
    EXPECT(!adapter.isDevicePlugged(1));
    GamepadReport rpt{};
    EXPECT(!adapter.submitReport(1, rpt));
}

static void test_unplug_unconfirmed() {
    TEST("unplugDevice — an unconfirmed teardown reports false (quarantine)");
    FakeProvisioner prov;
    prov.deprovisionResult = false;
    HidMaestroAdapter adapter(prov);
    EXPECT(adapter.ensureBusOpen());
    EXPECT(adapter.pluginDevice(3, GamepadIdentity::Xbox));
    EXPECT(!adapter.unplugDevice(3));
    // Unplugging a never-plugged serial is trivially gone.
    EXPECT(adapter.unplugDevice(9));
}

static void test_xbox_submit_bytes() {
    TEST("submitReport(Xbox) — section carries the packed report + GIP slice");
    FakeProvisioner prov;
    HidMaestroAdapter adapter(prov);
    EXPECT(adapter.ensureBusOpen());
    EXPECT(adapter.pluginDevice(1, GamepadIdentity::Xbox));

    GamepadReport rpt{};
    rpt.sThumbLX = 1000;
    rpt.bLeftTrigger = 255;
    rpt.wButtons = 0x1000 | 0x0400; // A + Guide
    EXPECT(adapter.submitReport(1, rpt));

    const uint8_t* sec = prov.inputSection(1);
    EXPECT_EQ(u32at(sec, INPUT_SEQNO_OFFSET), (uint32_t)4); // neutral + this frame
    EXPECT_EQ(u32at(sec, INPUT_DATASIZE_OFFSET), (uint32_t)X360_REPORT_BYTES);
    uint8_t expected[X360_REPORT_BYTES];
    packX360Report(rpt, expected);
    EXPECT_EQ(std::memcmp(sec + INPUT_DATA_OFFSET, expected, X360_REPORT_BYTES), 0);
    uint8_t expectedGip[INPUT_GIP_LENGTH];
    packGip(rpt, expectedGip);
    EXPECT_EQ(std::memcmp(sec + INPUT_GIP_OFFSET, expectedGip, INPUT_GIP_LENGTH), 0);
    EXPECT_EQ(u32at(sec, INPUT_EXT_SIZE_OFFSET), (uint32_t)0);
}

static void test_ds4_submit_merges_motion_touch_battery() {
    TEST("DS4 — motion/touch/battery merge into every subsequent frame");
    FakeProvisioner prov;
    HidMaestroAdapter adapter(prov);
    EXPECT(adapter.ensureBusOpen());
    EXPECT(adapter.pluginDevice(2, GamepadIdentity::DS4));

    MotionReport motion{};
    motion.gyroX = 100;
    EXPECT(adapter.submitMotion(2, motion));
    const uint8_t* data = prov.inputData(2);
    EXPECT_EQ(u32at(prov.inputSection(2), INPUT_DATASIZE_OFFSET), (uint32_t)DS4_PAYLOAD_BYTES);
    const int16_t gyro = static_cast<int16_t>(data[12] | (data[13] << 8));
    EXPECT_EQ(gyro, sonyImuFromWire(100));

    TouchpadReport touch{};
    touch.finger0.active = true;
    touch.buttonPressed = true;
    EXPECT(adapter.submitTouchpad(2, touch));
    EXPECT_EQ(data[34] & 0x80, 0x00); // finger 0 down
    EXPECT_EQ(data[34] & 0x7F, 1);    // first contact bumped the tracking id

    BatteryReport batt;
    batt.level = 30;
    batt.status = BATTERY_STATUS_DISCHARGING;
    EXPECT(adapter.submitBattery(2, batt));
    EXPECT_EQ(data[11], (uint8_t)3);

    // The gamepad frame keeps all of the merged state riding along.
    GamepadReport rpt{};
    rpt.wButtons = 0x1000;
    EXPECT(adapter.submitReport(2, rpt));
    EXPECT_EQ(static_cast<int16_t>(data[12] | (data[13] << 8)), sonyImuFromWire(100));
    EXPECT_EQ(data[11], (uint8_t)3);
    EXPECT_EQ(data[4] & 0x20, 0x20); // Cross
}

static void test_ds5_submit() {
    TEST("DualSense — seq counter advances, motion + touch at DS5 offsets");
    FakeProvisioner prov;
    HidMaestroAdapter adapter(prov);
    EXPECT(adapter.ensureBusOpen());
    EXPECT(adapter.pluginDevice(4, GamepadIdentity::DualSense));

    const uint8_t* data = prov.inputData(4);
    GamepadReport rpt{};
    EXPECT(adapter.submitReport(4, rpt));
    const uint8_t seqA = data[6];
    EXPECT(adapter.submitReport(4, rpt));
    EXPECT_EQ(static_cast<uint8_t>(data[6] - seqA), (uint8_t)1);
    EXPECT_EQ(u32at(prov.inputSection(4), INPUT_DATASIZE_OFFSET), (uint32_t)DS5_PAYLOAD_BYTES);

    MotionReport motion{};
    motion.accelZ = 500;
    EXPECT(adapter.submitMotion(4, motion));
    EXPECT_EQ(static_cast<int16_t>(data[25] | (data[26] << 8)), sonyImuFromWire(500));

    TouchpadReport touch{};
    touch.finger0.active = true;
    EXPECT(adapter.submitTouchpad(4, touch));
    EXPECT_EQ(data[32] & 0x80, 0x00);

    BatteryReport batt;
    batt.level = 70;
    batt.status = BATTERY_STATUS_CHARGING;
    EXPECT(adapter.submitBattery(4, batt));
    EXPECT_EQ(data[52], (uint8_t)(0x10 | 7));
}

static void test_switch_submit() {
    TEST("SwitchPro — pad and motion merge into the 48-byte body");
    FakeProvisioner prov;
    HidMaestroAdapter adapter(prov);
    EXPECT(adapter.ensureBusOpen());
    EXPECT(adapter.pluginDevice(5, GamepadIdentity::SwitchPro));

    const uint8_t* data = prov.inputData(5);
    GamepadReport rpt{};
    rpt.wButtons = 0x1000; // A (south) -> wire B
    EXPECT(adapter.submitReport(5, rpt));
    EXPECT_EQ(u32at(prov.inputSection(5), INPUT_DATASIZE_OFFSET), (uint32_t)SWITCH_BODY_BYTES);
    EXPECT_EQ(data[2], (uint8_t)0x04);

    MotionReport motion{};
    motion.accelY = 32767;
    EXPECT(adapter.submitMotion(5, motion));
    EXPECT_EQ(data[2], (uint8_t)0x04); // pad state still merged in
    const int16_t az = static_cast<int16_t>(data[12 + 4] | (data[12 + 5] << 8));
    EXPECT_EQ(az, (int16_t)16384);

    // No touchpad/battery surface on this profile.
    TouchpadReport touch{};
    EXPECT(!adapter.submitTouchpad(5, touch));
    BatteryReport batt;
    EXPECT(!adapter.submitBattery(5, batt));
}

static void test_motion_support_matrix() {
    TEST("supportsMotionForType — everything but Xbox");
    FakeProvisioner prov;
    HidMaestroAdapter adapter(prov);
    EXPECT(!adapter.supportsMotionForType(CONTROLLER_TYPE_XBOX));
    EXPECT(adapter.supportsMotionForType(CONTROLLER_TYPE_PLAYSTATION));
    EXPECT(adapter.supportsMotionForType(CONTROLLER_TYPE_DUALSENSE));
    EXPECT(adapter.supportsMotionForType(CONTROLLER_TYPE_SWITCHPRO));
    GamepadReport rpt{};
    (void)rpt;
    MotionReport motion{};
    FakeProvisioner prov2;
    HidMaestroAdapter adapter2(prov2);
    EXPECT(adapter2.ensureBusOpen());
    EXPECT(adapter2.pluginDevice(1, GamepadIdentity::Xbox));
    EXPECT(!adapter2.submitMotion(1, motion));
}

static void test_rumble_worker_xbox() {
    TEST("output worker — XUSB SET_STATE packets surface as rumble callbacks");
    FakeProvisioner prov;
    HidMaestroAdapter adapter(prov);
    CallbackSink sink;
    sink.install(adapter);
    EXPECT(adapter.ensureBusOpen());
    EXPECT(adapter.pluginDevice(1, GamepadIdentity::Xbox));

    prov.publishOutput(1, OUTPUT_SOURCE_XINPUT, 0, {0x00, 0x08, 0x40, 0x80, 0x00});
    EXPECT(sink.waitRumble(1));
    EXPECT_EQ(sink.lastSerial, (uint32_t)1);
    EXPECT_EQ(sink.lastRumble.strongMagnitude, (uint16_t)(0x40 * 257));
    EXPECT_EQ(sink.lastRumble.weakMagnitude, (uint16_t)(0x80 * 257));

    // A burst is drained in order, not coalesced.
    prov.publishOutput(1, OUTPUT_SOURCE_XINPUT, 0, {0x00, 0x08, 0x01, 0x01, 0x00});
    prov.publishOutput(1, OUTPUT_SOURCE_XINPUT, 0, {0x00, 0x08, 0x02, 0x02, 0x00});
    EXPECT(sink.waitRumble(3));
    EXPECT_EQ(sink.lastRumble.strongMagnitude, (uint16_t)(0x02 * 257));
}

static void test_rumble_and_lightbar_worker_ds4() {
    TEST("output worker — DS4 0x05 packets surface rumble + lightbar");
    FakeProvisioner prov;
    HidMaestroAdapter adapter(prov);
    CallbackSink sink;
    sink.install(adapter);
    EXPECT(adapter.ensureBusOpen());
    EXPECT(adapter.pluginDevice(2, GamepadIdentity::DS4));

    std::vector<uint8_t> report(31, 0);
    report[0] = 0x03;
    report[3] = 10;
    report[4] = 20;
    report[5] = 7;
    report[6] = 8;
    report[7] = 9;
    prov.publishOutput(2, OUTPUT_SOURCE_HID_OUTPUT, 0x05, report);
    EXPECT(sink.waitRumble(1));
    EXPECT(sink.waitLightbar(1));
    EXPECT_EQ(sink.lastSerial, (uint32_t)2);
    EXPECT_EQ(sink.lastRumble.strongMagnitude, (uint16_t)(20 * 257));
    EXPECT_EQ(sink.r, (uint8_t)7);
    EXPECT_EQ(sink.g, (uint8_t)8);
    EXPECT_EQ(sink.b, (uint8_t)9);

    // Feature-read echoes (source 3) are informational, never rumble.
    prov.publishOutput(2, OUTPUT_SOURCE_HID_FEATURE_READ, 0x05, {});
    prov.publishOutput(2, OUTPUT_SOURCE_HID_OUTPUT, 0x05, report);
    EXPECT(sink.waitRumble(2));
    EXPECT_EQ(sink.rumbleCount, 2);
}

// The setting reaches the plug as a provisioner argument, never as an
// assumption. Getting it wrong upward installs a kernel USB transport nobody
// asked for, so the whole matrix is pinned.
static void test_audio_request_matrix() {
    TEST("audio setting on: the Sony identities ask for a composite persona");
    {
        FakeProvisioner prov;
        HidMaestroAdapter adapter(prov, audioOn());
        EXPECT(adapter.ensureBusOpen());
        EXPECT(adapter.pluginDevice(1, GamepadIdentity::DualSense));
        EXPECT(prov.lastAudioRequested);
        EXPECT(adapter.hasSpeakerEndpoint(1));
        EXPECT(adapter.hasMicEndpoint(1));

        EXPECT(adapter.pluginDevice(2, GamepadIdentity::DS4));
        EXPECT(prov.lastAudioRequested);
        EXPECT(adapter.hasSpeakerEndpoint(2));
    }

    TEST("audio setting on: identities with no audio function never ask");
    {
        FakeProvisioner prov;
        HidMaestroAdapter adapter(prov, audioOn());
        EXPECT(adapter.ensureBusOpen());
        EXPECT(adapter.pluginDevice(1, GamepadIdentity::Xbox));
        EXPECT(!prov.lastAudioRequested);
        EXPECT(adapter.pluginDevice(2, GamepadIdentity::SwitchPro));
        EXPECT(!prov.lastAudioRequested);
        EXPECT_EQ(prov.audioProvisionCalls, 0);
        EXPECT(!adapter.hasSpeakerEndpoint(1));
        EXPECT(!adapter.hasMicEndpoint(2));
    }

    TEST("audio setting off: not even a DualSense asks");
    {
        FakeProvisioner prov;
        HidMaestroAdapter adapter(prov, [] { return false; });
        EXPECT(adapter.ensureBusOpen());
        EXPECT(adapter.pluginDevice(1, GamepadIdentity::DualSense));
        EXPECT(!prov.lastAudioRequested);
        EXPECT_EQ(prov.audioProvisionCalls, 0);
        EXPECT(!adapter.hasSpeakerEndpoint(1));
    }

    TEST("no setting provider at all: audio stays off (the safe default)");
    {
        FakeProvisioner prov;
        HidMaestroAdapter adapter(prov);
        EXPECT(adapter.ensureBusOpen());
        EXPECT(adapter.pluginDevice(1, GamepadIdentity::DualSense));
        EXPECT(!prov.lastAudioRequested);
        EXPECT(!adapter.hasSpeakerEndpoint(1));
    }
}

static void test_audio_provision_failure_falls_back() {
    TEST("a refused composite falls back to the plain persona, pad still works");
    FakeProvisioner prov;
    prov.failAudioProvision = true;
    HidMaestroAdapter adapter(prov, audioOn());
    EXPECT(adapter.ensureBusOpen());
    EXPECT(adapter.pluginDevice(4, GamepadIdentity::DualSense));
    EXPECT_EQ(prov.provisionCalls, 2); // one with audio, one without
    EXPECT(!prov.lastAudioRequested);
    EXPECT(adapter.isDevicePlugged(4));
    EXPECT(!adapter.hasSpeakerEndpoint(4));
    EXPECT(!adapter.hasMicEndpoint(4));

    // Input still works on the fallback pad.
    GamepadReport rpt{};
    rpt.wButtons = 0x1000;
    EXPECT(adapter.submitReport(4, rpt));

    TEST("a plug that fails outright is not retried into an audio loop");
    FakeProvisioner dead;
    dead.provisionResult = false;
    HidMaestroAdapter adapter2(dead, audioOn());
    EXPECT(adapter2.ensureBusOpen());
    EXPECT(!adapter2.pluginDevice(1, GamepadIdentity::DualSense));
    EXPECT_EQ(dead.provisionCalls, 2); // audio attempt, then the plain retry
    EXPECT(!adapter2.isDevicePlugged(1));
}

static void test_speaker_ring_to_callback() {
    TEST("speaker ring -> backend callback: PCM arrives with the frames intact");
    FakeProvisioner prov;
    HidMaestroAdapter adapter(prov, audioOn());
    AudioSink sink;
    sink.install(adapter);
    EXPECT(adapter.ensureBusOpen());
    EXPECT(adapter.pluginDevice(1, GamepadIdentity::DualSense));
    EXPECT(prov.hasAudioSections(1));

    // 48 kHz persona: the resampler is the identity, so the samples the helper
    // published must arrive byte-for-byte.
    const std::vector<int16_t> batch = stereoTone(48000, 1000.0, 240);
    prov.publishSpeaker(1, batch);
    EXPECT(sink.waitFrames(240));
    {
        std::lock_guard<std::mutex> lk(sink.m);
        EXPECT_EQ(sink.lastSerial, (uint32_t)1);
        EXPECT_EQ(sink.totalFrames, (size_t)240);
        EXPECT_EQ(sink.pcm.size(), batch.size());
        EXPECT(sink.pcm == batch);
    }

    TEST("a burst of batches drains in order without coalescing");
    prov.publishSpeaker(1, stereoTone(48000, 500.0, 240));
    prov.publishSpeaker(1, stereoTone(48000, 500.0, 240));
    EXPECT(sink.waitFrames(720));
    {
        std::lock_guard<std::mutex> lk(sink.m);
        EXPECT_EQ(sink.totalFrames, (size_t)720);
        EXPECT_EQ(sink.speakerCalls, 3);
    }
}

// The DualShock 4 v2 persona's speaker endpoint is 32 kHz, so the adapter must
// rate-convert before the SAT-2 callback, which is pinned at the wire rate.
// Half again as many frames come out, and the tone keeps its level.
static void test_speaker_rate_conversion() {
    TEST("a 32 kHz persona is resampled to the wire rate before the callback");
    FakeProvisioner prov;
    prov.speakerRateHz = 32000;
    HidMaestroAdapter adapter(prov, audioOn());
    AudioSink sink;
    sink.install(adapter);
    EXPECT(adapter.ensureBusOpen());
    EXPECT(adapter.pluginDevice(1, GamepadIdentity::DS4));

    // 3200 frames at 32 kHz = 100 ms, in ring-sized batches.
    const std::vector<int16_t> batch = stereoTone(32000, 1000.0, 320);
    for (int i = 0; i < 10; ++i) prov.publishSpeaker(1, batch);
    EXPECT(sink.waitFrames(4700));
    {
        std::lock_guard<std::mutex> lk(sink.m);
        EXPECT(sink.totalFrames >= 4780 && sink.totalFrames <= 4810);
        // Left channel was written at twice the right channel's amplitude;
        // both must survive the conversion at their own level.
        const double left = rmsOf(sink.pcm, 2, 0, 100);
        const double right = rmsOf(sink.pcm, 2, 1, 100);
        EXPECT(left > 12000.0 && left < 16000.0);
        EXPECT(right > 6000.0 && right < 8000.0);
    }
}

static void test_mic_submit_to_ring() {
    TEST("submitMicAudioPcm -> mic ring: a 48 kHz window crosses unchanged");
    FakeProvisioner prov;
    HidMaestroAdapter adapter(prov, audioOn());
    EXPECT(adapter.ensureBusOpen());
    EXPECT(adapter.pluginDevice(1, GamepadIdentity::DualSense));

    std::vector<int16_t> window(AUDIO_FRAME_SAMPLES);
    for (size_t i = 0; i < window.size(); ++i) window[i] = static_cast<int16_t>(i % 1000);
    EXPECT(adapter.submitMicAudioPcm(1, window.data(), window.size()));

    const std::vector<int16_t> got = prov.drainMic(1);
    EXPECT_EQ(got.size(), window.size());
    EXPECT(got == window);

    TEST("a second window follows the first, in order");
    EXPECT(adapter.submitMicAudioPcm(1, window.data(), window.size()));
    EXPECT_EQ(prov.drainMic(1).size(), window.size());

    TEST("submitting to an unplugged serial is refused, not silently dropped");
    EXPECT(!adapter.submitMicAudioPcm(2, window.data(), window.size()));
    EXPECT(!adapter.submitMicAudioPcm(0, window.data(), window.size()));
    EXPECT(!adapter.submitMicAudioPcm(1, nullptr, 10));
    EXPECT(!adapter.submitMicAudioPcm(1, window.data(), 0));
}

static void test_mic_submit_without_endpoint() {
    TEST("a pad with no mic endpoint refuses mic PCM (not an error, just absent)");
    FakeProvisioner prov;
    HidMaestroAdapter adapter(prov, [] { return false; });
    EXPECT(adapter.ensureBusOpen());
    EXPECT(adapter.pluginDevice(1, GamepadIdentity::DualSense));
    EXPECT(!adapter.hasMicEndpoint(1));

    std::vector<int16_t> window(AUDIO_FRAME_SAMPLES, 1234);
    EXPECT(!adapter.submitMicAudioPcm(1, window.data(), window.size()));

    TEST("an Xbox pad likewise has nowhere to put mic PCM");
    EXPECT(adapter.pluginDevice(2, GamepadIdentity::Xbox));
    EXPECT(!adapter.submitMicAudioPcm(2, window.data(), window.size()));
}

// The DualShock 4 v2 persona records at 16 kHz, so a 960-sample 48 kHz window
// must reach the ring as ~320 samples, lowpassed rather than strided.
static void test_mic_rate_conversion() {
    TEST("a 16 kHz mic endpoint gets a downsampled window, one third the length");
    FakeProvisioner prov;
    prov.micRateHz = 16000;
    prov.speakerRateHz = 32000;
    HidMaestroAdapter adapter(prov, audioOn());
    EXPECT(adapter.ensureBusOpen());
    EXPECT(adapter.pluginDevice(1, GamepadIdentity::DS4));

    // Ten 20 ms windows of a 1 kHz tone: in band at 16 kHz, so it must survive.
    std::vector<int16_t> total;
    for (int w = 0; w < 10; ++w) {
        std::vector<int16_t> window(AUDIO_FRAME_SAMPLES);
        for (size_t i = 0; i < window.size(); ++i) {
            const double t = (w * AUDIO_FRAME_SAMPLES + i) / 48000.0;
            window[i] =
                static_cast<int16_t>(std::sin(2.0 * 3.14159265358979323846 * 1000.0 * t) * 20000.0);
        }
        EXPECT(adapter.submitMicAudioPcm(1, window.data(), window.size()));
        const std::vector<int16_t> got = prov.drainMic(1);
        total.insert(total.end(), got.begin(), got.end());
    }
    EXPECT(total.size() >= 3180 && total.size() <= 3210);

    TEST("the downsampled tone keeps its level (a filter, not an attenuator)");
    const double level = rmsOf(total, 1, 0, 100);
    EXPECT(level > 12000.0 && level < 16000.0);
}

static void test_mic_led_from_output_ring() {
    TEST("output worker — a DS5 mute-lamp write surfaces as a mic-LED callback");
    FakeProvisioner prov;
    HidMaestroAdapter adapter(prov, audioOn());
    AudioSink sink;
    sink.install(adapter);
    EXPECT(adapter.ensureBusOpen());
    EXPECT(adapter.pluginDevice(3, GamepadIdentity::DualSense));

    std::vector<uint8_t> report(47, 0);
    report[1] = 0x01; // MIC_MUTE_LED_CONTROL_ENABLE
    report[8] = MIC_LED_STATE_ON;
    prov.publishOutput(3, OUTPUT_SOURCE_HID_OUTPUT, 0x02, report);
    EXPECT(sink.waitMicLed(1));
    {
        std::lock_guard<std::mutex> lk(sink.m);
        EXPECT_EQ(sink.lastSerial, (uint32_t)3);
        EXPECT_EQ(sink.lastMicLed, MIC_LED_STATE_ON);
    }

    TEST("pulse follows, and an out-of-range state produces no callback at all");
    report[8] = MIC_LED_STATE_PULSE;
    prov.publishOutput(3, OUTPUT_SOURCE_HID_OUTPUT, 0x02, report);
    EXPECT(sink.waitMicLed(2));
    {
        std::lock_guard<std::mutex> lk(sink.m);
        EXPECT_EQ(sink.lastMicLed, MIC_LED_STATE_PULSE);
    }
    report[8] = 0x7F;
    prov.publishOutput(3, OUTPUT_SOURCE_HID_OUTPUT, 0x02, report);
    // Follow it with a real one so the wait has something to land on.
    report[8] = MIC_LED_STATE_OFF;
    prov.publishOutput(3, OUTPUT_SOURCE_HID_OUTPUT, 0x02, report);
    EXPECT(sink.waitMicLed(3));
    {
        std::lock_guard<std::mutex> lk(sink.m);
        EXPECT_EQ(sink.micLedCalls, 3); // the 0x7F write produced nothing
        EXPECT_EQ(sink.lastMicLed, MIC_LED_STATE_OFF);
    }
}

// The real producer and consumer run in different processes on different
// threads. This is the multi-threaded half of the seqlock contract the pure
// suite cannot force: hammer the ring from another thread and assert every
// batch that arrives is internally consistent, never half of two batches.
static void test_speaker_ring_concurrent_producer() {
    TEST("a concurrent producer never yields a torn batch to the drain worker");
    FakeProvisioner prov;
    HidMaestroAdapter adapter(prov, audioOn());

    std::atomic<int> torn{0};
    std::atomic<int> seen{0};
    adapter.setSpeakerAudioCallback([&](uint32_t, const int16_t* pcm, size_t frames) {
        // Every batch is written as a constant value repeated, so a torn read
        // shows up as two different values inside one callback.
        for (size_t i = 1; i < frames * AUDIO_SPEAKER_CHANNELS; ++i) {
            if (pcm[i] != pcm[0]) {
                torn.fetch_add(1);
                break;
            }
        }
        seen.fetch_add(1);
    });

    EXPECT(adapter.ensureBusOpen());
    EXPECT(adapter.pluginDevice(1, GamepadIdentity::DualSense));

    std::thread producer([&] {
        for (int i = 1; i <= 400; ++i) {
            const std::vector<int16_t> batch(480, static_cast<int16_t>((i % 300) + 1));
            prov.publishSpeaker(1, batch);
            std::this_thread::yield();
        }
    });
    producer.join();

    // Give the worker a moment to finish draining the tail.
    for (int i = 0; i < 100 && seen.load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_EQ(torn.load(), 0);
    EXPECT(seen.load() > 0); // the drain actually ran
    EXPECT(adapter.unplugDevice(1));
}

static void test_audio_workers_stop_with_the_pad() {
    TEST("unplug tears down the audio worker and forgets the endpoints");
    FakeProvisioner prov;
    HidMaestroAdapter adapter(prov, audioOn());
    AudioSink sink;
    sink.install(adapter);
    EXPECT(adapter.ensureBusOpen());
    EXPECT(adapter.pluginDevice(1, GamepadIdentity::DualSense));
    EXPECT(adapter.hasSpeakerEndpoint(1));

    prov.publishSpeaker(1, stereoTone(48000, 440.0, 240));
    EXPECT(sink.waitFrames(240));

    EXPECT(adapter.unplugDevice(1));
    EXPECT(!adapter.hasSpeakerEndpoint(1));
    EXPECT(!adapter.hasMicEndpoint(1));
    std::vector<int16_t> window(AUDIO_FRAME_SAMPLES, 7);
    EXPECT(!adapter.submitMicAudioPcm(1, window.data(), window.size()));

    TEST("a replug gets fresh rings and a fresh resampler");
    EXPECT(adapter.pluginDevice(1, GamepadIdentity::DualSense));
    EXPECT(adapter.hasSpeakerEndpoint(1));
    EXPECT(adapter.submitMicAudioPcm(1, window.data(), window.size()));
    EXPECT_EQ(prov.drainMic(1).size(), window.size());
}

static void test_close_bus() {
    TEST("closeBus — tears down slots but keeps the helper resident");
    FakeProvisioner prov;
    {
        HidMaestroAdapter adapter(prov);
        EXPECT(adapter.ensureBusOpen());
        EXPECT(adapter.pluginDevice(1, GamepadIdentity::Xbox));
        EXPECT(adapter.pluginDevice(2, GamepadIdentity::DualSense));
        adapter.closeBus();
        EXPECT(!adapter.isBusOpen());
        EXPECT_EQ(prov.deprovisionCalls, 2);
        // Idle-close must NOT stop the helper: respawning it would mean a
        // fresh UAC prompt on every reconnect.
        EXPECT_EQ(prov.shutdownCalls, 0);
        EXPECT(!adapter.isDevicePlugged(1));
        EXPECT(!adapter.isDevicePlugged(2));
    }
    EXPECT(prov.shutdownCalls >= 1); // destructor ends the helper session
}

int main() {
    test_bus_open_probe_only();
    test_identity_support();
    test_plugin_lifecycle();
    test_plugin_denied_elevation();
    test_plugin_provision_failure();
    test_unplug_unconfirmed();
    test_xbox_submit_bytes();
    test_ds4_submit_merges_motion_touch_battery();
    test_ds5_submit();
    test_switch_submit();
    test_motion_support_matrix();
    test_rumble_worker_xbox();
    test_rumble_and_lightbar_worker_ds4();
    test_audio_request_matrix();
    test_audio_provision_failure_falls_back();
    test_speaker_ring_to_callback();
    test_speaker_rate_conversion();
    test_mic_submit_to_ring();
    test_mic_submit_without_endpoint();
    test_mic_rate_conversion();
    test_mic_led_from_output_ring();
    test_speaker_ring_concurrent_producer();
    test_audio_workers_stop_with_the_pad();
    test_close_bus();

    std::cout << "hidmaestro_adapter: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
