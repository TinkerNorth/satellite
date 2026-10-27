// SPDX-License-Identifier: LGPL-3.0-or-later

#include "session_service.h"

#include "core/audio/haptic_envelope.h"
#include "core/hex.h"
#include "ipv4_util.h"

#include <algorithm>
#include <cstring>

// std::random not libsodium, to keep the core libsodium-free. These values need
// uniqueness, not secrecy: session security rests on the pairing key feeding the
// injected KeyDeriver.
#include <random>

using satellite::formatIPv4Nbo;
using satellite::parseIPv4Nbo;

static constexpr const char* CONNECTION_ID_PREFIX = "conn_";

static uint32_t makeRandomToken() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist(1, UINT32_MAX);
    return dist(gen);
}

namespace {

uint32_t padInfoWord(const Controller& ctrl) {
    return ctrl.active ? hotPadInfo(ctrl.caps, ctrl.controllerType, ctrl.touchpadMode) : 0u;
}

uint32_t padGenWord(const Controller& ctrl) { return ctrl.active ? ctrl.plugGen : 0u; }

uint32_t sessionWord(const Connection& conn) {
    return hotSessionWord(conn.epoch, conn.mouseControlGranted);
}

void publishControllers(const Connection& conn) {
    HotSlot& hot = *conn.hot;
    for (size_t i = 0; i < MAX_CONTROLLERS_PER_CONN; i++) {
        const Controller& ctrl = conn.controllers[i];
        hot.ctrl[i].store(hotCtrlWord(ctrl.active, ctrl.serialNo), std::memory_order_relaxed);
        hot.padInfo[i].store(padInfoWord(ctrl), std::memory_order_relaxed);
        hot.padGen[i].store(padGenWord(ctrl), std::memory_order_relaxed);
    }
    hot.session.store(sessionWord(conn), std::memory_order_relaxed);
}

bool controllersPublished(const Connection& conn) {
    const HotSlot& hot = *conn.hot;
    for (size_t i = 0; i < MAX_CONTROLLERS_PER_CONN; i++) {
        const Controller& ctrl = conn.controllers[i];
        const uint32_t word = hotCtrlWord(ctrl.active, ctrl.serialNo);
        if (hot.ctrl[i].load(std::memory_order_relaxed) != word) return false;
        if (hot.padInfo[i].load(std::memory_order_relaxed) != padInfoWord(ctrl)) return false;
        if (hot.padGen[i].load(std::memory_order_relaxed) != padGenWord(ctrl)) return false;
    }
    return hot.session.load(std::memory_order_relaxed) == sessionWord(conn);
}

uint8_t safeTouchpadMode(uint8_t mode) {
    return mode < TOUCHPAD_MODE_COUNT ? mode : TOUCHPAD_MODE_OFF;
}

class ConnectionWrite {
  public:
    ConnectionWrite(Connection& conn, const RxEpoch& rx) : conn_(conn), write_(*conn.hot, rx) {}
    ~ConnectionWrite() { close(); }
    ConnectionWrite(const ConnectionWrite&) = delete;
    ConnectionWrite& operator=(const ConnectionWrite&) = delete;

    void close() {
        if (!open_) return;
        open_ = false;
        publishControllers(conn_);
        write_.close();
    }

  private:
    Connection& conn_;
    HotWrite write_;
    bool open_ = true;
};

void recordDatagram(HotSlot& slot, uint32_t keyEpoch, uint32_t counter, uint64_t clientAddr) {
    if (slot.clientAddr.load(std::memory_order_relaxed) != clientAddr) {
        slot.clientAddr.store(clientAddr, std::memory_order_release);
    }
    slot.lastCounter.store(counter, std::memory_order_relaxed);
    slot.lastPacketTicks.store(hotTicks(std::chrono::steady_clock::now()),
                               std::memory_order_relaxed);
    if (slot.replayKeyEpoch.load(std::memory_order_relaxed) == keyEpoch) return;
    slot.replayKeyEpoch.store(keyEpoch, std::memory_order_release);
    slot.feedbackPending.store(true, std::memory_order_release);
}

std::chrono::steady_clock::time_point lastPacketAt(const Connection& conn) {
    return hotTime(conn.hot->lastPacketTicks.load(std::memory_order_acquire));
}

constexpr int RX_READ_ATTEMPTS = 64;

void publishRxMotion(RxPad& rx, const MotionReport& report, bool delivered) {
    rx.ownFlags |= RX_PAD_MOTION;
    rx.ownFlags =
        delivered ? (rx.ownFlags | RX_PAD_MOTION_SINK) : (rx.ownFlags & ~RX_PAD_MOTION_SINK);
    rx.beginWrite();
    storeWords(rx.motion, report);
    rx.flags.store(rx.ownFlags, std::memory_order_relaxed);
    rx.endWrite();
}

void publishRxBattery(RxPad& rx, const BatteryReport& report) {
    rx.ownFlags |= RX_PAD_BATTERY;
    rx.beginWrite();
    rx.battery.store(static_cast<uint32_t>(report.level) |
                         (static_cast<uint32_t>(report.status) << 8),
                     std::memory_order_relaxed);
    rx.flags.store(rx.ownFlags, std::memory_order_relaxed);
    rx.endWrite();
}

void publishRxTouchpad(RxPad& rx, const TouchpadReport& report, uint8_t mode, uint32_t keyEpoch) {
    rx.ownFlags |= RX_PAD_TOUCHPAD;
    rx.beginWrite();
    storeWords(rx.touchpad, report);
    rx.touchpadMode.store(mode, std::memory_order_relaxed);
    rx.touchpadKeyEpoch.store(keyEpoch, std::memory_order_relaxed);
    rx.flags.store(rx.ownFlags, std::memory_order_relaxed);
    rx.endWrite();
}

bool readRxPad(const RxPad& rx, RxPadView& out) {
    for (int attempt = 0; attempt < RX_READ_ATTEMPTS; attempt++) {
        if (rx.read(out)) return true;
        std::this_thread::yield();
    }
    return false;
}

} // namespace

SessionService::SessionService(IGamepadPort& backend, IClientPort& client, ILogPort& log,
                               KeyDeriver keyDeriver, IAudioCodecFactory* audioCodecs,
                               ProcessBarrierFn processBarrier)
    : backend_(backend), client_(client), log_(log), keyDeriver_(std::move(keyDeriver)),
      audioCodecs_(audioCodecs), rx_(processBarrier) {
    // The `this`-capturing lambdas are safe: SessionService outlives the adapter,
    // and the adapter joins its notification workers before the callbacks fire.
    backend_.setRumbleCallback(
        [this](uint32_t serial, const RumbleReport& r) { handleRumbleFromBackend(serial, r); });
    backend_.setLightbarCallback([this](uint32_t serial, uint8_t r, uint8_t g, uint8_t b) {
        handleLightbarFromBackend(serial, r, g, b);
    });
    backend_.setTriggerEffectsCallback([this](uint32_t serial, const TriggerEffectsReport& r) {
        handleTriggerEffectsFromBackend(serial, r);
    });
    backend_.setPlayerLedsCallback(
        [this](uint32_t serial, uint8_t ledMask) { handlePlayerLedsFromBackend(serial, ledMask); });
    // The two audio handlers report whether a frame went out, which the tests
    // pin; the sinks have nobody to tell. Every reason for a false is either a
    // gate that is meant to be silent (policy off, no capable sender, a stray
    // serial) or already counted in the audio stats (contention, encode).
    backend_.setSpeakerAudioCallback([this](uint32_t serial, const int16_t* pcm, size_t frames) {
        handleSpeakerAudioFromBackend(serial, pcm, frames);
    });
    backend_.setHapticAudioCallback([this](uint32_t serial, const int16_t* pcm, size_t frames) {
        handleHapticAudioFromBackend(serial, pcm, frames);
    });
    backend_.setMicLedCallback(
        [this](uint32_t serial, uint8_t state) { handleMicLedFromBackend(serial, state); });
}

void SessionService::setAudioPolicy(AudioPolicyFn fn) { audioPolicy_ = std::move(fn); }

ControllerAudioPolicy SessionService::audioPolicy() const {
    // Deliberately unlocked, like keyDeriver_ and audioCodecs_.
    return audioPolicy_ ? audioPolicy_() : ControllerAudioPolicy{};
}

// Internal helpers below assume the caller holds mtx_.

bool SessionService::findBySerialLocked(uint32_t serial, Connection*& outConn,
                                        Controller*& outCtrl) {
    // Linear scan, bounded by the 16-controller cap; not worth a reverse index
    // that must track allocateSerial/releaseSerial.
    for (auto& [tok, conn] : connections_) {
        for (auto& ctrl : conn.controllers) {
            if (ctrl.active && ctrl.serialNo == serial) {
                outConn = &conn;
                outCtrl = &ctrl;
                return true;
            }
        }
    }
    outConn = nullptr;
    outCtrl = nullptr;
    return false;
}

Connection* SessionService::findByDeviceId(const std::string& deviceId) {
    for (auto& [tok, conn] : connections_) {
        if (conn.deviceId == deviceId) return &conn;
    }
    return nullptr;
}

Connection* SessionService::findByConnectionId(const std::string& connectionId) {
    for (auto& [tok, conn] : connections_) {
        if (conn.connectionId == connectionId) return &conn;
    }
    return nullptr;
}

const Connection* SessionService::findByConnectionId(const std::string& connectionId) const {
    for (auto& [tok, conn] : connections_) {
        if (conn.connectionId == connectionId) return &conn;
    }
    return nullptr;
}

void SessionService::retireConnectionLocked(Connection& conn, std::vector<RetiredSerial>& retired) {
    for (auto& ctrl : conn.controllers) {
        if (ctrl.active && ctrl.serialNo != 0) {
            releasePointerLocked(ctrl.serialNo);
            retired.push_back(RetiredSerial{ctrl.serialNo, false});
            ctrl.active = false;
            ctrl.serialNo = 0;
        }
    }
    adjustActivePadsLocked(conn, -conn.activeControllerCount);
    hot_.retire(conn.slot);
    ConnectionWrite write(conn, rx_);
    conn.hot->token.store(0, std::memory_order_relaxed);
    const uint8_t cleared[CRYPTO_KEY_SIZE] = {};
    conn.hot->storeKey(cleared);
}

void SessionService::unplugRetired(std::vector<RetiredSerial>& retired) {
    for (RetiredSerial& r : retired) {
        blockingRegion("backend.lifecycle");
        r.unplugged = backend_.unplugDevice(r.serial);
    }
}

void SessionService::settleRetiredLocked(std::vector<RetiredSerial>& retired) {
    for (const RetiredSerial& r : retired) {
        if (r.unplugged) {
            releaseSerial(r.serial);
            continue;
        }
        quarantineSerial(r.serial);
        log_.logMsg(LogLevel::WARN, "service",
                    "Unplug of serial " + std::to_string(r.serial) +
                        " unconfirmed; serial quarantined until the bus closes");
    }
    retired.clear();
}

void SessionService::finishTeardown(std::vector<RetiredSerial>& retired) {
    unplugRetired(retired);
    bool idle = false;
    {
        std::lock_guard<TrackedMutex> lk(mtx_);
        settleRetiredLocked(retired);
        idle = busIdleLocked();
    }
    closeBusIfIdle(idle);
}

uint32_t SessionService::allocateSerial() {
    // Round-robin scan start so a just-freed serial isn't re-plugged while its
    // async PnP removal may still be in flight.
    for (size_t step = 0; step < MAX_BACKEND_CONTROLLERS; step++) {
        size_t i = (serialScanStart_ + step) % MAX_BACKEND_CONTROLLERS;
        if (!serialInUse_[i] && !serialQuarantined_[i]) {
            pendingRumble_[i + 1].store(0, std::memory_order_release);
            serialInUse_[i] = true;
            serialScanStart_ = (i + 1) % MAX_BACKEND_CONTROLLERS;
            return static_cast<uint32_t>(i + 1); // 1-based
        }
    }
    return 0;
}

void SessionService::releaseSerial(uint32_t serial) {
    if (serial == 0 || serial > (uint32_t)MAX_BACKEND_CONTROLLERS) return;
    pendingRumble_[serial].store(0, std::memory_order_release);
    serialInUse_[serial - 1] = false;
}

void SessionService::quarantineSerial(uint32_t serial) {
    if (serial == 0 || serial > (uint32_t)MAX_BACKEND_CONTROLLERS) return;
    pendingRumble_[serial].store(0, std::memory_order_release);
    serialInUse_[serial - 1] = false;
    serialQuarantined_[serial - 1] = true;
}

bool SessionService::claimSerialLocked(uint32_t serial) {
    if (serial == 0 || serial > (uint32_t)MAX_BACKEND_CONTROLLERS) return false;
    if (serialInUse_[serial - 1] || serialQuarantined_[serial - 1]) return false;
    pendingRumble_[serial].store(0, std::memory_order_release);
    serialInUse_[serial - 1] = true;
    return true;
}

int SessionService::countGlobalActiveControllers() const {
    int total = 0;
    for (auto& [tok, c] : connections_) { total += c.activeControllerCount; }
    return total;
}

void SessionService::adjustActivePadsLocked(Connection& conn, int delta) {
    conn.activeControllerCount += delta;
    activePads_.fetch_add(delta, std::memory_order_relaxed);
}

bool SessionService::busIdleLocked() const {
    return backend_.isBusOpen() && countGlobalActiveControllers() == 0;
}

void SessionService::closeBusIfIdle(bool idle) {
    if (!idle) return;
    blockingRegion("backend.lifecycle");
    backend_.closeBus();
    std::lock_guard<TrackedMutex> lk(mtx_);
    std::memset(serialQuarantined_, 0, sizeof(serialQuarantined_));
    log_.logMsg(LogLevel::INFO, "service", "Backend bus closed (no active controllers)");
}

uint32_t SessionService::generateUniqueToken() {
    uint32_t token;
    do { token = makeRandomToken(); } while (connections_.count(token));
    return token;
}

bool SessionService::reserveSessionLocked(const std::string& deviceId,
                                          const std::string& deviceName, SessionMint& mint) {
    const int slot = hot_.findFree();
    if (slot < 0) {
        log_.logMsg(LogLevel::WARN, "service",
                    "Session refused for " + deviceName + ": all " + std::to_string(MAX_SESSIONS) +
                        " session slots are in use");
        return false;
    }
    Connection& row = mint.row;
    do {
        row.connectionId = CONNECTION_ID_PREFIX + hexEncodeBE32(makeRandomToken());
    } while (findByConnectionId(row.connectionId) != nullptr);
    row.deviceId = deviceId;
    row.hot = &hot_.slots[static_cast<size_t>(slot)];
    row.slot = slot;
    row.row = ++nextRow_;
    row.connectedAt = std::chrono::steady_clock::now();
    return true;
}

void SessionService::mintSessionKeyLocked(const uint8_t pairingKey[CRYPTO_KEY_SIZE],
                                          SessionMint& mint) {
    mint.token = generateUniqueToken();
    for (size_t i = 0; i < SESSION_SALT_SIZE; i += 4) {
        uint32_t r = makeRandomToken();
        std::memcpy(mint.salt + i, &r, 4);
    }
    if (keyDeriver_) {
        keyDeriver_(pairingKey, mint.salt, mint.token, mint.key);
    } else {
        std::memcpy(mint.key, pairingKey, CRYPTO_KEY_SIZE);
    }
}

Connection& SessionService::adoptSessionLocked(Connection* existing, const std::string& deviceName,
                                               SessionMint& mint) {
    if (existing == nullptr) {
        mint.row.token = mint.token;
        Connection& created = connections_.emplace(mint.token, std::move(mint.row)).first->second;
        log_.logMsg(LogLevel::INFO, "service",
                    "Session created for " + deviceName + " (" + created.connectionId + ")");
        return created;
    }
    client_.sendSessionClose(*existing, CLOSE_REASON_REPLACED);
    releaseConnectionPointerLocked(*existing);
    auto node = connections_.extract(existing->token);
    node.key() = mint.token;
    node.mapped().token = mint.token;
    Connection& rotated = connections_.insert(std::move(node)).position->second;
    log_.logMsg(LogLevel::INFO, "service",
                "Session rotated for " + deviceName + " (" + rotated.connectionId + ")");
    return rotated;
}

void SessionService::publishSessionKeyLocked(Connection& conn,
                                             std::chrono::steady_clock::time_point now) {
    HotSlot& hot = *conn.hot;
    hot.token.store(conn.token, std::memory_order_relaxed);
    hot.keyEpoch.store(hot.keyEpoch.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    hot.storeKey(conn.sessionKey);
    hot.txCounter.store(0, std::memory_order_relaxed);
    hot.clientAddr.store(0, std::memory_order_relaxed);
    hot.feedbackPending.store(false, std::memory_order_relaxed);
    hot.lastPacketTicks.store(hotTicks(now), std::memory_order_relaxed);
    hot.row.store(conn.row, std::memory_order_relaxed);
    hot.storeName(conn.deviceName);
}

void SessionService::publishSessionLocked(Connection& conn,
                                          std::chrono::steady_clock::time_point now) {
    ConnectionWrite write(conn, rx_);
    publishSessionKeyLocked(conn, now);
    write.close();
    hot_.publish(conn.slot, conn.token);
}

void SessionService::publishControllersLocked(Connection& conn) {
    if (controllersPublished(conn)) return;
    ConnectionWrite write(conn, rx_);
}

void SessionService::resetControllerStreamState(Controller& ctrl) {
    // A (re)plugged controller is a fresh actuator, so the next rumble/colour
    // must not be suppressed as "same as last".
    ctrl.lastRumble = RumbleReport{};
    ctrl.lastRumbleValid = false;
    ctrl.lastRumbleSentAt = {};
    ctrl.lastRumbleExpiresAt = {};
    ctrl.hidRumble = RumbleReport{};
    ctrl.hapticRumble = RumbleReport{};
    ctrl.hapticRumbleSentAt = {};
    ctrl.hapticRumbleExpiresAt = {};
    ctrl.lightbarR = 0;
    ctrl.lightbarG = 0;
    ctrl.lightbarB = 0;
    ctrl.lastLightbarValid = false;
    ctrl.lastTriggerEffects = TriggerEffectsReport{};
    ctrl.lastTriggerEffectsValid = false;
    ctrl.playerLeds = 0;
    ctrl.lastPlayerLedsValid = false;
    ctrl.micLedState = MIC_LED_STATE_OFF;
    ctrl.lastMicLedValid = false;
    // The codecs and the reorder window belong to the pad that WAS here. A
    // decoder carries the previous pad's filter state and concealment history,
    // and a sequence number carried across would make the new stream look like
    // a 40 ms reorder of the old one. Dropping the whole thing also returns the
    // memory (see ControllerAudio) rather than holding it for a pad that may
    // never speak.
    ctrl.audio.reset();
    ctrl.plugGen = ++nextPlugGen_;
    if (ctrl.plugGen == 0) ctrl.plugGen = ++nextPlugGen_;
}

void SessionService::removePadLocked(Connection& conn, Controller& ctrl,
                                     std::vector<RetiredSerial>& retired) {
    log_.logMsg(LogLevel::INFO, "service",
                "Controller #" + std::to_string(ctrl.index) + " removed from " + conn.deviceName);
    vacatePadLocked(conn, ctrl, retired);
    ctrl.audio.reset();
}

void SessionService::vacatePadLocked(Connection& conn, Controller& ctrl,
                                     std::vector<RetiredSerial>& retired) {
    if (ctrl.serialNo != 0) {
        releasePointerLocked(ctrl.serialNo);
        retired.push_back(RetiredSerial{ctrl.serialNo, false});
    }
    ctrl.active = false;
    ctrl.serialNo = 0;
    adjustActivePadsLocked(conn, -1);
    conn.epoch++;
}

uint16_t SessionService::activeBitmapLocked(const Connection& conn) const {
    uint16_t bitmap = 0;
    for (size_t i = 0; i < MAX_CONTROLLERS_PER_CONN; i++) {
        if (conn.controllers[i].active) bitmap |= static_cast<uint16_t>(1u << i);
    }
    return bitmap;
}

static void fillAppliedState(IGamepadPort& backend, const Controller& ctrl,
                             ControllerApplyResult& out) {
    if (!ctrl.active) {
        out.motionSinkSupportedForType = false;
        out.motionBackendOk = false;
        out.backendId.clear();
        return;
    }
    out.motionSinkSupportedForType = backend.supportsMotionForType(ctrl.controllerType);
    out.motionBackendOk = backend.motionBackendOk(ctrl.serialNo);
    out.backendId = backend.backendIdForSerial(ctrl.serialNo);
}

void SessionService::planPadLocked(const Connection& conn, const ControllerDescriptor& desc,
                                   PadChange& pad) {
    pad.desc = desc;
    if (desc.ctrlIdx >= MAX_CONTROLLERS_PER_CONN) {
        pad.result = APPLY_ERR_INVALID_INDEX;
        return;
    }
    const Controller& ctrl = conn.controllers[desc.ctrlIdx];
    if (desc.type >= CONTROLLER_TYPE_COUNT ||
        !backend_.supportsIdentity(controllerIdentity(desc.type))) {
        pad.result = APPLY_ERR_INVALID_TYPE;
        pad.appliedType = ctrl.active ? ctrl.controllerType : CONTROLLER_TYPE_XBOX;
        return;
    }
    pad.appliedType = desc.type;
    if (!ctrl.active) {
        pad.kind = PadChange::Kind::Add;
        pad.serial = allocateSerial();
        return;
    }
    if (controllerIdentity(desc.type) == ctrl.identity) {
        pad.kind = PadChange::Kind::Update;
        return;
    }
    pad.kind = PadChange::Kind::Replug;
    if (!backend_.isBusOpen()) return;
    pad.serial = allocateSerial();
    if (pad.serial != 0) return;
    pad.kind = PadChange::Kind::ReplugInPlace;
    pad.retiring = ctrl.serialNo;
    pad.waits = true;
}

SessionService::PadChange SessionService::planRemoval(uint8_t ctrlIdx) {
    PadChange pad;
    pad.kind = PadChange::Kind::Remove;
    pad.desc.ctrlIdx = ctrlIdx;
    return pad;
}

void SessionService::deferUnservedAdds(TopologyChange& change) {
    bool retires = false;
    for (const PadChange& pad : change.pads) {
        const bool swaps = pad.kind == PadChange::Kind::Replug && pad.serial != 0;
        retires = retires || swaps || pad.kind == PadChange::Kind::ReplugInPlace ||
                  pad.kind == PadChange::Kind::Remove;
    }
    for (PadChange& pad : change.pads) {
        if (pad.kind == PadChange::Kind::Add && pad.serial == 0) pad.waits = retires;
    }
}

std::vector<ControllerDescriptor>
SessionService::dedupDescriptors(const std::vector<ControllerDescriptor>& descriptors) {
    std::vector<ControllerDescriptor> deduped;
    for (const auto& d : descriptors) {
        bool replaced = false;
        for (auto& existing : deduped) {
            if (existing.ctrlIdx == d.ctrlIdx) {
                existing = d;
                replaced = true;
                break;
            }
        }
        if (!replaced) deduped.push_back(d);
    }
    return deduped;
}

void SessionService::planSessionPadsLocked(const Connection& conn,
                                           const std::vector<ControllerDescriptor>& desired,
                                           TopologyChange& change) {
    bool kept[MAX_CONTROLLERS_PER_CONN] = {};
    for (const auto& d : desired) {
        if (d.ctrlIdx < MAX_CONTROLLERS_PER_CONN) kept[d.ctrlIdx] = true;
    }
    for (size_t i = 0; i < MAX_CONTROLLERS_PER_CONN; i++) {
        if (conn.controllers[i].active && !kept[i]) {
            change.pads.push_back(planRemoval(static_cast<uint8_t>(i)));
        }
    }
    for (const auto& d : desired) {
        change.pads.emplace_back();
        planPadLocked(conn, d, change.pads.back());
    }
    deferUnservedAdds(change);
}

bool SessionService::plugsInRound(const PadChange& pad, bool secondRound) {
    const bool plugs = pad.kind == PadChange::Kind::Add || pad.kind == PadChange::Kind::Replug ||
                       pad.kind == PadChange::Kind::ReplugInPlace;
    return plugs && pad.serial != 0 && pad.waits == secondRound;
}

void SessionService::plugPads(TopologyChange& change, bool secondRound) {
    bool wanted = false;
    for (const PadChange& pad : change.pads) wanted = wanted || plugsInRound(pad, secondRound);
    if (!wanted) return;
    if (!backend_.isBusOpen()) {
        blockingRegion("backend.lifecycle");
        if (backend_.ensureBusOpen()) change.busReopened = true;
    }
    const bool busOpen = backend_.isBusOpen();
    for (PadChange& pad : change.pads) {
        if (!plugsInRound(pad, secondRound)) continue;
        pad.busDown = !busOpen;
        if (!busOpen) continue;
        blockingRegion("backend.lifecycle");
        pad.plugged = backend_.pluginDevicePreferring(pad.serial, controllerIdentity(pad.desc.type),
                                                      pad.desc.preferredBackend);
    }
}

void SessionService::commitPadsLocked(Connection& conn, TopologyChange& change, bool secondRound) {
    if (change.busReopened) {
        change.busReopened = false;
        std::memset(serialQuarantined_, 0, sizeof(serialQuarantined_));
        log_.logMsg(LogLevel::INFO, "service", "Backend bus opened on demand");
    }
    for (PadChange& pad : change.pads) {
        if (secondRound && !pad.waits) continue;
        commitPadLocked(conn, pad, change.retired, secondRound);
    }
}

void SessionService::commitPadLocked(Connection& conn, PadChange& pad,
                                     std::vector<RetiredSerial>& retired, bool secondRound) {
    if (pad.kind == PadChange::Kind::Refuse) return;
    Controller& ctrl = conn.controllers[pad.desc.ctrlIdx];
    if (pad.kind == PadChange::Kind::Update) {
        updatePadLocked(ctrl, pad.desc);
    } else if (pad.kind == PadChange::Kind::Remove) {
        removePadLocked(conn, ctrl, retired);
    } else if (pad.kind == PadChange::Kind::Replug) {
        replugPadLocked(conn, ctrl, pad, retired);
    } else if (pad.kind == PadChange::Kind::ReplugInPlace && !secondRound) {
        vacatePadLocked(conn, ctrl, retired);
    } else if (pad.kind == PadChange::Kind::ReplugInPlace) {
        refitPadLocked(conn, ctrl, pad);
    } else if (secondRound || !pad.waits) {
        addPadLocked(conn, ctrl, pad);
    }
}

void SessionService::updatePadLocked(Controller& ctrl, const ControllerDescriptor& desc) {
    const uint8_t mode = safeTouchpadMode(desc.touchpadMode);
    if (mode != ctrl.touchpadMode) releasePointerLocked(ctrl.serialNo);
    ctrl.controllerType = desc.type;
    ctrl.caps = desc.caps;
    ctrl.touchpadMode = mode;
    ctrl.preferredBackend = desc.preferredBackend;
}

void SessionService::takeDescriptorLocked(Controller& ctrl, const ControllerDescriptor& desc) {
    ctrl.controllerType = desc.type;
    ctrl.identity = controllerIdentity(desc.type);
    ctrl.caps = desc.caps;
    ctrl.touchpadMode = safeTouchpadMode(desc.touchpadMode);
    ctrl.preferredBackend = desc.preferredBackend;
    resetControllerStreamState(ctrl);
}

void SessionService::bindPadLocked(Connection& conn, Controller& ctrl, const PadChange& pad) {
    ctrl.index = pad.desc.ctrlIdx;
    ctrl.serialNo = pad.serial;
    ctrl.active = true;
    takeDescriptorLocked(ctrl, pad.desc);
    adjustActivePadsLocked(conn, 1);
    conn.epoch++;
}

void SessionService::addPadLocked(Connection& conn, Controller& ctrl, PadChange& pad) {
    if (pad.plugged) {
        bindPadLocked(conn, ctrl, pad);
        log_.logMsg(LogLevel::INFO, "service",
                    "Controller #" + std::to_string(pad.desc.ctrlIdx) + " plugged as " +
                        controllerTypeLabel(pad.desc.type) + " (serial " +
                        std::to_string(pad.serial) + ") for " + conn.deviceName);
        return;
    }
    releaseSerial(pad.serial);
    if (pad.serial == 0) {
        pad.result = APPLY_ERR_NO_SLOTS;
    } else if (pad.busDown) {
        log_.logMsg(LogLevel::ERR, "service", "Controller apply failed: backend bus unavailable");
        pad.result = APPLY_ERR_BACKEND_UNAVAIL;
    } else {
        pad.result = APPLY_ERR_PLUGIN_FAIL;
    }
}

void SessionService::replugPadLocked(Connection& conn, Controller& ctrl, PadChange& pad,
                                     std::vector<RetiredSerial>& retired) {
    conn.epoch++;
    if (!pad.plugged) {
        pad.result = APPLY_ERR_REPLUG_FAIL;
        pad.appliedType = ctrl.controllerType;
        if (pad.serial == 0) return;
        releaseSerial(pad.serial);
        log_.logMsg(LogLevel::ERR, "service",
                    "Failed to replug controller #" + std::to_string(pad.desc.ctrlIdx) + " as " +
                        controllerTypeLabel(pad.desc.type) + "; keeping existing " +
                        controllerTypeLabel(ctrl.controllerType));
        return;
    }
    releasePointerLocked(ctrl.serialNo);
    retired.push_back(RetiredSerial{ctrl.serialNo, false});
    ctrl.serialNo = pad.serial;
    takeDescriptorLocked(ctrl, pad.desc);
    log_.logMsg(LogLevel::INFO, "service",
                "Replugged controller #" + std::to_string(pad.desc.ctrlIdx) + " as " +
                    controllerTypeLabel(pad.desc.type) + " (serial " +
                    std::to_string(ctrl.serialNo) + ")");
}

void SessionService::refitPadLocked(Connection& conn, Controller& ctrl, PadChange& pad) {
    const std::string index = std::to_string(pad.desc.ctrlIdx);
    if (pad.plugged) {
        bindPadLocked(conn, ctrl, pad);
        log_.logMsg(LogLevel::INFO, "service",
                    "Replugged controller #" + index + " as " + controllerTypeLabel(pad.desc.type) +
                        " (serial " + std::to_string(pad.serial) + ")");
        return;
    }
    releaseSerial(pad.serial);
    pad.result = APPLY_ERR_PLUGIN_FAIL;
    log_.logMsg(LogLevel::ERR, "service",
                pad.serial == 0
                    ? "Replug fallback failed to unplug controller #" + index + "; slot lost"
                    : "Replug fallback plug failed for controller #" + index + "; slot lost");
}

bool SessionService::allocateWaitingLocked(TopologyChange& change) {
    bool waiting = false;
    for (PadChange& pad : change.pads) {
        if (pad.kind != PadChange::Kind::ReplugInPlace) continue;
        waiting = true;
        pad.serial = claimSerialLocked(pad.retiring) ? pad.retiring : allocateSerial();
    }
    for (PadChange& pad : change.pads) {
        if (pad.kind != PadChange::Kind::Add || !pad.waits) continue;
        waiting = true;
        pad.serial = allocateSerial();
    }
    return waiting;
}

void SessionService::convergePads(Connection& conn, TopologyChange& change) {
    plugPads(change, false);
    {
        std::lock_guard<TrackedMutex> lk(mtx_);
        commitPadsLocked(conn, change, false);
        publishControllersLocked(conn);
    }
    settlePadChange(conn, change);
}

void SessionService::settlePadChange(Connection& conn, TopologyChange& change) {
    unplugRetired(change.retired);
    {
        std::lock_guard<TrackedMutex> lk(mtx_);
        settleRetiredLocked(change.retired);
        if (!allocateWaitingLocked(change)) return;
    }
    plugPads(change, true);
    std::lock_guard<TrackedMutex> lk(mtx_);
    commitPadsLocked(conn, change, true);
    publishControllersLocked(conn);
}

ControllerApplyResult SessionService::applyResultLocked(const Connection& conn,
                                                        const PadChange& pad) const {
    ControllerApplyResult out;
    out.ctrlIdx = pad.desc.ctrlIdx;
    out.result = pad.result;
    out.appliedType = pad.appliedType;
    if (pad.desc.ctrlIdx < MAX_CONTROLLERS_PER_CONN) {
        fillAppliedState(backend_, conn.controllers[pad.desc.ctrlIdx], out);
    }
    return out;
}

SessionUpsertResult SessionService::upsertSession(
    const std::string& deviceId, const std::string& deviceName, const std::string& clientIP,
    const uint8_t pairingKey[CRYPTO_KEY_SIZE], const std::vector<ControllerDescriptor>& descriptors,
    bool requestMouseControl, int protocolVersion) {
    std::lock_guard<TrackedMutex> topo(topoMtx_);
    SessionUpsertResult res;
    const std::vector<ControllerDescriptor> desired = dedupDescriptors(descriptors);
    SessionMint mint;
    TopologyChange change;
    Connection* conn = nullptr;
    {
        std::lock_guard<TrackedMutex> lk(mtx_);
        conn = findByDeviceId(deviceId);
        if (conn == nullptr && !reserveSessionLocked(deviceId, deviceName, mint)) {
            res.error = UPSERT_ERROR_TOO_MANY_SESSIONS;
            return res;
        }
        mintSessionKeyLocked(pairingKey, mint);
        planSessionPadsLocked(conn != nullptr ? *conn : mint.row, desired, change);
    }
    plugPads(change, false);
    {
        std::lock_guard<TrackedMutex> lk(mtx_);
        const auto now = std::chrono::steady_clock::now();
        conn = &adoptSessionLocked(conn, deviceName, mint);
        conn->deviceName = deviceName;
        conn->clientIP = clientIP;
        std::memcpy(conn->sessionSalt, mint.salt, SESSION_SALT_SIZE);
        std::memcpy(conn->sessionKey, mint.key, CRYPTO_KEY_SIZE);
        conn->graceUntil = now + std::chrono::seconds(REST_LIVENESS_GRACE_SEC);
        conn->protocolVersion = protocolVersion;
        conn->mouseControlGranted = requestMouseControl && backend_.supportsRelativeMouse();
        commitPadsLocked(*conn, change, false);
        publishSessionLocked(*conn, now);
    }
    settlePadChange(*conn, change);
    bool idle = false;
    {
        std::lock_guard<TrackedMutex> lk(mtx_);
        res.ok = true;
        res.connectionId = conn->connectionId;
        res.token = conn->token;
        std::memcpy(res.sessionSalt, conn->sessionSalt, SESSION_SALT_SIZE);
        res.epoch = conn->epoch;
        res.maxControllers = MAX_BACKEND_CONTROLLERS;
        res.protocolVersion = conn->protocolVersion;
        res.mouseControlGranted = conn->mouseControlGranted;
        if (requestMouseControl && !conn->mouseControlGranted) {
            res.mouseControlDenyReason = HOST_FEATURE_DENY_NOT_SUPPORTED;
        }
        for (const PadChange& pad : change.pads) {
            if (pad.kind == PadChange::Kind::Remove) continue;
            res.controllers.push_back(applyResultLocked(*conn, pad));
        }
        idle = busIdleLocked();
    }
    closeBusIfIdle(idle);
    return res;
}

bool SessionService::applyController(const std::string& connectionId, const std::string& deviceId,
                                     const ControllerDescriptor& desc,
                                     ControllerApplyResult& outResult, uint16_t& outEpoch) {
    std::lock_guard<TrackedMutex> topo(topoMtx_);
    TopologyChange change;
    Connection* conn = nullptr;
    {
        std::lock_guard<TrackedMutex> lk(mtx_);
        conn = findByConnectionId(connectionId);
        if (conn == nullptr || conn->deviceId != deviceId) return false;
        change.pads.emplace_back();
        planPadLocked(*conn, desc, change.pads.back());
        deferUnservedAdds(change);
    }
    convergePads(*conn, change);
    bool idle = false;
    {
        std::lock_guard<TrackedMutex> lk(mtx_);
        outResult = applyResultLocked(*conn, change.pads.front());
        outEpoch = conn->epoch;
        idle = busIdleLocked();
    }
    closeBusIfIdle(idle);
    return true;
}

bool SessionService::removeController(const std::string& connectionId, const std::string& deviceId,
                                      uint8_t ctrlIdx, uint16_t& outEpoch) {
    std::lock_guard<TrackedMutex> topo(topoMtx_);
    TopologyChange change;
    Connection* conn = nullptr;
    {
        std::lock_guard<TrackedMutex> lk(mtx_);
        conn = findByConnectionId(connectionId);
        if (conn == nullptr || conn->deviceId != deviceId) return false;
        const bool present =
            ctrlIdx < MAX_CONTROLLERS_PER_CONN && conn->controllers[ctrlIdx].active;
        if (present) change.pads.push_back(planRemoval(ctrlIdx));
    }
    convergePads(*conn, change);
    bool idle = false;
    {
        std::lock_guard<TrackedMutex> lk(mtx_);
        outEpoch = conn->epoch;
        idle = busIdleLocked();
    }
    closeBusIfIdle(idle);
    return true;
}

SessionService::SessionView SessionService::getSessionView(const std::string& connectionId,
                                                           const std::string& deviceId) const {
    std::lock_guard<TrackedMutex> lk(mtx_);
    SessionView view;
    const Connection* conn = findByConnectionId(connectionId);
    if (conn == nullptr) return view;
    if (!deviceId.empty() && conn->deviceId != deviceId) return view;
    view.found = true;
    view.connectionId = conn->connectionId;
    view.deviceId = conn->deviceId;
    view.epoch = conn->epoch;
    view.mouseControlGranted = conn->mouseControlGranted;
    view.protocolVersion = conn->protocolVersion;
    for (const auto& ctrl : conn->controllers) {
        if (!ctrl.active) continue;
        SessionView::CtrlView cv;
        cv.ctrlIdx = ctrl.index;
        cv.appliedType = ctrl.controllerType;
        cv.caps = ctrl.caps;
        cv.touchpadMode = ctrl.touchpadMode;
        cv.motionSinkSupportedForType = backend_.supportsMotionForType(ctrl.controllerType);
        cv.motionBackendOk = backend_.motionBackendOk(ctrl.serialNo);
        cv.backendId = backend_.backendIdForSerial(ctrl.serialNo);
        cv.preferredBackend = ctrl.preferredBackend;
        view.controllers.push_back(cv);
    }
    return view;
}

int SessionService::closeSessionById(const std::string& connectionId, const std::string& deviceId,
                                     uint8_t reason, bool notify) {
    std::lock_guard<TrackedMutex> topo(topoMtx_);
    std::vector<RetiredSerial> retired;
    int removed = 0;
    {
        std::lock_guard<TrackedMutex> lk(mtx_);
        Connection* conn = findByConnectionId(connectionId);
        if (conn == nullptr) return -1;
        if (!deviceId.empty() && conn->deviceId != deviceId) return -1;

        if (notify) client_.sendSessionClose(*conn, reason);
        removed = conn->activeControllerCount;
        log_.logMsg(LogLevel::INFO, "service",
                    "Connection closed for " + conn->deviceName + " (" + std::to_string(removed) +
                        " controllers removed, " + closeReasonName(reason) + ")");
        const uint32_t token = conn->token;
        retireConnectionLocked(*conn, retired);
        connections_.erase(token);
    }
    finishTeardown(retired);
    return removed;
}

int SessionService::closeSessionsForDevice(const std::string& deviceId, uint8_t reason) {
    std::lock_guard<TrackedMutex> topo(topoMtx_);
    std::vector<RetiredSerial> retired;
    int closed = 0;
    {
        std::lock_guard<TrackedMutex> lk(mtx_);
        for (auto it = connections_.begin(); it != connections_.end();) {
            if (it->second.deviceId == deviceId) {
                client_.sendSessionClose(it->second, reason);
                log_.logMsg(LogLevel::INFO, "service",
                            "Connection closed for " + it->second.deviceName + " (" +
                                closeReasonName(reason) + ")");
                retireConnectionLocked(it->second, retired);
                it = connections_.erase(it);
                closed++;
            } else {
                ++it;
            }
        }
    }
    if (closed > 0) finishTeardown(retired);
    return closed;
}

void SessionService::closeAllSessions(uint8_t reason) {
    std::lock_guard<TrackedMutex> topo(topoMtx_);
    std::vector<RetiredSerial> retired;
    {
        std::lock_guard<TrackedMutex> lk(mtx_);
        for (auto& [tok, conn] : connections_) { client_.sendSessionClose(conn, reason); }
        for (auto& [tok, conn] : connections_) { retireConnectionLocked(conn, retired); }
        connections_.clear();
    }
    unplugRetired(retired);
    bool idle = false;
    {
        std::lock_guard<TrackedMutex> lk(mtx_);
        settleRetiredLocked(retired);
        std::memset(serialInUse_, 0, sizeof(serialInUse_));
        std::memset(serialQuarantined_, 0, sizeof(serialQuarantined_));
        idle = busIdleLocked();
    }
    closeBusIfIdle(idle);
}

bool SessionService::handleGamepadData(uint32_t token, uint8_t ctrlIdx,
                                       const GamepadReport& report) {
    rx_.enter();
    const bool ok = submitHotFrame(token, ctrlIdx, report, false, 0, 0);
    rx_.exit();
    return ok;
}

void SessionService::handleHeartbeat(uint32_t token) {
    rx_.enter();
    const int index = hot_.find(token);
    if (index >= 0) {
        HotSlot& slot = hot_.slots[static_cast<size_t>(index)];
        SessionEndpoint to;
        to.slot = &slot;
        uint32_t slotToken = 0;
        uint32_t session = 0;
        uint16_t bitmap = 0;
        const bool stable = slot.read([&] {
            slotToken = slot.token.load(std::memory_order_relaxed);
            session = slot.session.load(std::memory_order_relaxed);
            slot.loadKey(to.key);
            bitmap = 0;
            for (size_t i = 0; i < MAX_CONTROLLERS_PER_CONN; i++) {
                const uint32_t word = slot.ctrl[i].load(std::memory_order_relaxed);
                if ((word & HOT_CTRL_ACTIVE) != 0) bitmap |= static_cast<uint16_t>(1u << i);
            }
        });
        if (stable && slotToken == token) {
            to.token = token;
            const int total = activePads_.load(std::memory_order_relaxed);
            client_.sendHeartbeatAck(to, backend_.isBusOpen(), static_cast<uint8_t>(total),
                                     hotSessionEpoch(session), bitmap);
        }
    }
    rx_.exit();
}

static constexpr uint64_t RUMBLE_PENDING = uint64_t{1} << 48;

static uint64_t packPendingRumble(const RumbleReport& report, const uint16_t durationMs) {
    const uint64_t strong = report.strongMagnitude;
    const uint64_t weak = static_cast<uint64_t>(report.weakMagnitude) << 16;
    const uint64_t duration = static_cast<uint64_t>(durationMs) << 32;
    return RUMBLE_PENDING | strong | weak | duration;
}

void SessionService::handleRumbleFromBackend(const uint32_t serial, const RumbleReport& report,
                                             const uint16_t wireDurationMs) {
    if (serial == 0 || serial > MAX_BACKEND_CONTROLLERS) return;
    const uint64_t pending = packPendingRumble(report, wireDurationMs);
    pendingRumble_[serial].store(pending, std::memory_order_release);
    // The maintenance tick drains a missed acquire.
    std::unique_lock<TrackedMutex> lk(mtx_, std::try_to_lock);
    if (!lk.owns_lock()) return;
    drainRumbleLocked(serial);
}

void SessionService::drainRumbleLocked(const uint32_t serial, int* const sent) {
    if (pendingRumble_[serial].load(std::memory_order_acquire) == 0) return;
    Connection* conn = nullptr;
    Controller* ctrl = nullptr;
    if (!findBySerialLocked(serial, conn, ctrl)) return;
    const uint64_t pending = pendingRumble_[serial].exchange(0, std::memory_order_acq_rel);
    if (pending == 0) return;
    ctrl->hidRumble.strongMagnitude = static_cast<uint16_t>(pending);
    ctrl->hidRumble.weakMagnitude = static_cast<uint16_t>(pending >> 16);
    const uint16_t durationMs = static_cast<uint16_t>(pending >> 32);
    const bool forwarded = forwardRumbleLocked(*conn, *ctrl, durationMs, false);
    if (forwarded && sent != nullptr) ++*sent;
}

bool SessionService::forwardRumbleLocked(Connection& conn, Controller& ctrl,
                                         uint16_t wireDurationMs, bool force) {
    // A haptic level past its window is a stream that stopped; mixing it in
    // would hand every later HID update a phantom motor.
    const auto now = std::chrono::steady_clock::now();
    const bool hapticLive = now < ctrl.hapticRumbleExpiresAt;
    // A client past the last command's duration has stopped its motors, so
    // "same as last" no longer means "already running".
    const bool lastExpired = ctrl.lastRumbleValid && now >= ctrl.lastRumbleExpiresAt;
    RumbleReport mixed;
    mixed.strongMagnitude = std::max(ctrl.hidRumble.strongMagnitude,
                                     hapticLive ? ctrl.hapticRumble.strongMagnitude : uint16_t{0});
    mixed.weakMagnitude = std::max(ctrl.hidRumble.weakMagnitude,
                                   hapticLive ? ctrl.hapticRumble.weakMagnitude : uint16_t{0});

    // Coalesce identical back-to-back updates (games hold the motors steady
    // across many frames). wireDurationMs is excluded so the caller can bump the
    // refresh deadline without forcing a packet.
    auto sameAs = [](const RumbleReport& a, const RumbleReport& b) {
        return a.strongMagnitude == b.strongMagnitude && a.weakMagnitude == b.weakMagnitude;
    };
    if (!force && ctrl.lastRumbleValid && !lastExpired && sameAs(ctrl.lastRumble, mixed)) {
        return false;
    }

    RumbleReport stamped = mixed;
    stamped.durationMs = wireDurationMs;

    ctrl.lastRumble = stamped;
    ctrl.lastRumbleValid = true;
    ctrl.lastRumbleSentAt = now;
    // durationMs 0 is "until the next packet" on the wire, which never expires.
    ctrl.lastRumbleExpiresAt = wireDurationMs == 0
                                   ? std::chrono::steady_clock::time_point::max()
                                   : now + std::chrono::milliseconds(wireDurationMs);

    client_.sendRumble(conn, ctrl.index, stamped);
    return true;
}

void SessionService::refreshIdleInput() {
    backend_.refreshIdleInput();
    std::lock_guard<TrackedMutex> lk(mtx_);
    if (!pointerRetryPending_) return;
    pointerRetryPending_ = !submitPointerLocked(0, 0, 0);
}

int SessionService::refreshRumble() {
    std::lock_guard<TrackedMutex> lk(mtx_);
    const auto now = std::chrono::steady_clock::now();
    int sent = 0;
    for (uint32_t serial = 1; serial <= MAX_BACKEND_CONTROLLERS; serial++) {
        drainRumbleLocked(serial, &sent);
    }
    for (auto& [token, conn] : connections_) {
        std::atomic<bool>& pending = conn.hot->feedbackPending;
        if (pending.load(std::memory_order_acquire) &&
            pending.exchange(false, std::memory_order_acq_rel)) {
            resendFeedbackStateLocked(conn);
        }
        for (auto& ctrl : conn.controllers) {
            if (!ctrl.active || ctrl.serialNo == 0) continue;
            const bool hidHeld =
                ctrl.hidRumble.strongMagnitude != 0 || ctrl.hidRumble.weakMagnitude != 0;
            if (hidHeld) {
                // The game still writes this level; keep the client's motors
                // running across its duration boundary.
                if (now - ctrl.lastRumbleSentAt >= std::chrono::milliseconds(RUMBLE_REFRESH_MS)) {
                    if (forwardRumbleLocked(conn, ctrl, RUMBLE_WIRE_DURATION_MS, /*force=*/true)) {
                        sent++;
                    }
                }
                continue;
            }
            // No HID source. A haptic reduction whose stream ended without a
            // silent window leaves its last level in the coalesce cache; once
            // it has expired, the mixed value is zero and the stop goes out
            // (an already-zero cache coalesces to nothing).
            const bool hapticStale = now >= ctrl.hapticRumbleExpiresAt;
            const bool cacheNonZero =
                ctrl.lastRumbleValid &&
                (ctrl.lastRumble.strongMagnitude != 0 || ctrl.lastRumble.weakMagnitude != 0);
            if (hapticStale && cacheNonZero) {
                ctrl.hapticRumble = RumbleReport{};
                if (forwardRumbleLocked(conn, ctrl,
                                        satellite::audio::HAPTIC_RUMBLE_WIRE_DURATION_MS,
                                        /*force=*/false)) {
                    sent++;
                }
            }
        }
    }
    return sent;
}

bool SessionService::readPad(uint32_t token, uint8_t ctrlIdx, PadRead& out) const {
    const int index = hot_.find(token);
    if (index < 0) return false;
    const HotSlot& slot = hot_.slots[static_cast<size_t>(index)];
    const bool knownIdx = ctrlIdx < MAX_CONTROLLERS_PER_CONN;
    uint32_t slotToken = 0;
    uint32_t word = 0;
    const bool stable = slot.read([&] {
        slotToken = slot.token.load(std::memory_order_relaxed);
        out.keyEpoch = slot.keyEpoch.load(std::memory_order_relaxed);
        out.session = slot.session.load(std::memory_order_relaxed);
        if (!knownIdx) return;
        word = slot.ctrl[ctrlIdx].load(std::memory_order_relaxed);
        out.info = slot.padInfo[ctrlIdx].load(std::memory_order_relaxed);
        out.gen = slot.padGen[ctrlIdx].load(std::memory_order_relaxed);
    });
    if (!stable || slotToken != token) return false;
    out.slot = index;
    out.serial = word & HOT_CTRL_SERIAL_MASK;
    out.active = (word & HOT_CTRL_ACTIVE) != 0 && out.serial >= 1 &&
                 out.serial <= static_cast<uint32_t>(MAX_BACKEND_CONTROLLERS);
    return true;
}

void SessionService::noteRxWrite() {
#ifdef SATELLITE_BUILD_TESTS
    const std::thread::id owner = rxOwnerForTest_.load(std::memory_order_relaxed);
    if (owner != std::thread::id{} && owner != std::this_thread::get_id()) {
        foreignRxWrites_.fetch_add(1, std::memory_order_relaxed);
    }
#endif
}

RxPad& SessionService::rxPadFor(const PadRead& pad) {
    RxPad& rx = rxPads_[pad.serial];
    if (rx.ownGen == pad.gen) return rx;
    noteRxWrite();
    rx.ownGen = pad.gen;
    rx.ownFlags = 0;
    rx.lastTouchpad = TouchpadReport{};
    rx.lastTouchpadValid = false;
    rx.touchpadMouseRemX = 0.0f;
    rx.touchpadMouseRemY = 0.0f;
    rx.micWindowStart = 0;
    rx.micPacketsInWindow = 0;
    rx.beginWrite();
    rx.gen.store(pad.gen, std::memory_order_relaxed);
    rx.flags.store(0, std::memory_order_relaxed);
    rx.endWrite();
    return rx;
}

bool SessionService::sessionName(int slot, uint32_t keyEpoch, std::string& out) const {
    const HotSlot& hot = hot_.slots[static_cast<size_t>(slot)];
    uint32_t epoch = 0;
    const bool stable = hot.read([&] {
        epoch = hot.keyEpoch.load(std::memory_order_relaxed);
        out = hot.loadName();
    });
    return stable && epoch == keyEpoch;
}

bool SessionService::handleMotionData(uint32_t token, uint8_t ctrlIdx, const MotionReport& report) {
    rx_.enter();
    PadRead pad;
    bool delivered = false;
    if (readPad(token, ctrlIdx, pad) && pad.active) {
        // Deliberately do NOT gate on ctrl.motionCapable() (CAP_MOTION): the
        // protocol says motion is best-effort and the receiver MUST accept
        // MSG_MOTION even from a dish that never advertised the cap. A backend
        // without an IMU surface returning false is not an error.
        delivered = backend_.submitMotion(pad.serial, report);
        noteRxWrite();
        publishRxMotion(rxPadFor(pad), report, delivered);
    }
    rx_.exit();
    return delivered;
}

SessionService::PointerMotion SessionService::pointerMotion(const TouchpadReport& prev,
                                                            bool prevValid,
                                                            const TouchpadReport& report,
                                                            float& remX, float& remY) {
    PointerMotion motion;
    // Delta only while finger 0 is down across both frames AND the
    // trackingId matches: a lifted finger compacts the survivor into slot 0
    // with a new id, and without the check the cursor teleports. Scale by
    // REFERENCE_MS/dt so velocity is dt-independent (Android's first MOVE
    // lands ~16 ms late, otherwise a visible first-touch jump).
    const bool continuous = prevValid && prev.finger0.active && report.finger0.active &&
                            prev.finger0.trackingId == report.finger0.trackingId;
    if (continuous) {
        // Unsigned subtract through int32_t handles the u32 uptime wrap and
        // cheaply flags out-of-order packets (dt <= 0) in one branch.
        const int32_t dt_ms = static_cast<int32_t>(report.eventTimeMs - prev.eventTimeMs);
        if (dt_ms <= 0) {
            // Duplicate/out-of-order: no motion to integrate. Preserve the
            // remainder so a slow drag still accumulates across resends.
        } else if (dt_ms > TOUCHPAD_MOUSE_MAX_GAP_MS) {
            // Big gap (dish paused/backgrounded/stalled). Re-anchor: no
            // motion, drop the remainder so a stale one doesn't catch up.
            remX = 0.0f;
            remY = 0.0f;
        } else {
            const int32_t dt_clamped =
                dt_ms < TOUCHPAD_MOUSE_MIN_DT_MS ? TOUCHPAD_MOUSE_MIN_DT_MS : dt_ms;
            const float scale =
                static_cast<float>(TOUCHPAD_MOUSE_REFERENCE_MS) / static_cast<float>(dt_clamped);
            const float fx = static_cast<float>(report.finger0.x - prev.finger0.x) *
                                 TOUCHPAD_MOUSE_SENSITIVITY * scale +
                             remX;
            const float fy = static_cast<float>(report.finger0.y - prev.finger0.y) *
                                 TOUCHPAD_MOUSE_SENSITIVITY * scale +
                             remY;
            motion.dx = static_cast<int>(fx);
            motion.dy = static_cast<int>(fy);
            // Keep the sub-pixel remainder so a slow drag still moves.
            remX = fx - static_cast<float>(motion.dx);
            remY = fy - static_cast<float>(motion.dy);
        }
    } else {
        remX = 0.0f;
        remY = 0.0f;
    }
    motion.buttons.left = report.buttonPressed;
    motion.buttons.right = report.rightPressed;
    motion.buttons.middle = report.middlePressed;
    // The wheel is an event: a duplicated or out-of-order frame (same/older
    // eventTimeMs) must not scroll twice. Fresh frames pass it through.
    motion.wheel = report.scrollV;
    if (prevValid && static_cast<int32_t>(report.eventTimeMs - prev.eventTimeMs) <= 0) {
        motion.wheel = 0;
    }
    motion.buttonsChanged =
        prevValid ? (prev.buttonPressed != report.buttonPressed ||
                     prev.rightPressed != report.rightPressed ||
                     prev.middlePressed != report.middlePressed)
                  : (report.buttonPressed || report.rightPressed || report.middlePressed);
    return motion;
}

bool SessionService::handleTouchpadData(uint32_t token, uint8_t ctrlIdx,
                                        const TouchpadReport& report) {
    rx_.enter();
    PadRead pad;
    if (!readPad(token, ctrlIdx, pad) || !pad.active) {
        rx_.exit();
        return false;
    }
    RxPad& rx = rxPadFor(pad);
    const uint8_t mode = hotPadMode(pad.info);

    // Capture the previous sample BEFORE overwriting: relative-mouse mode needs
    // the finger-0 delta between consecutive frames.
    const TouchpadReport prev = rx.lastTouchpad;
    const bool prevValid = rx.lastTouchpadValid && rx.lastTouchpadMode == mode &&
                           rx.lastTouchpadKeyEpoch == pad.keyEpoch;

    // Cache for the web-UI debug pane regardless of routing mode / backend.
    noteRxWrite();
    rx.lastTouchpad = report;
    rx.lastTouchpadValid = true;
    rx.lastTouchpadMode = mode;
    rx.lastTouchpadKeyEpoch = pad.keyEpoch;
    publishRxTouchpad(rx, report, mode, pad.keyEpoch);

    bool ok = false;
    switch (mode) {
    case TOUCHPAD_MODE_OFF:
        break;

    case TOUCHPAD_MODE_MOUSE:
        // Host-input streams are only valid for session-granted features;
        // ungranted streams are dropped (cached above for the debug pane).
        if (hotSessionMouse(pad.session)) {
            const PointerMotion motion =
                pointerMotion(prev, prevValid, report, rx.touchpadMouseRemX, rx.touchpadMouseRemY);
            rx_.exit();
            return injectPointerFrame(token, ctrlIdx, pad, motion);
        }
        break;

    case TOUCHPAD_MODE_DS4:
    default:
        // A false from the default impl (Xbox pad / inert backend) is not an
        // error; cached above.
        ok = backend_.submitTouchpad(pad.serial, report);
        break;
    }
    rx_.exit();
    return ok;
}

bool SessionService::injectPointerFrame(uint32_t token, uint8_t ctrlIdx, const PadRead& pad,
                                        const PointerMotion& motion) {
    std::lock_guard<TrackedMutex> lk(mtx_);
    auto it = connections_.find(token);
    if (it == connections_.end() || ctrlIdx >= MAX_CONTROLLERS_PER_CONN) return false;
    Connection& conn = it->second;
    const Controller& ctrl = conn.controllers[ctrlIdx];
    const bool bound = ctrl.active && ctrl.serialNo == pad.serial && ctrl.plugGen == pad.gen &&
                       ctrl.touchpadMode == TOUCHPAD_MODE_MOUSE && conn.mouseControlGranted;
    if (!bound) return false;
    pointerButtons_[pad.serial] = motion.buttons;
    const bool ok = submitPointerLocked(motion.dx, motion.dy, motion.wheel);
    pointerRetryPending_ = !ok;
    if (motion.dx != 0 || motion.dy != 0 || motion.wheel != 0 || motion.buttonsChanged) {
        recordPointerInjectionLocked(conn, ok);
    }
    return ok;
}

bool SessionService::submitPointerLocked(const int dx, const int dy, const int wheel) {
    MouseButtons buttons;
    for (const auto& owner : pointerButtons_) {
        buttons.left = buttons.left || owner.left;
        buttons.right = buttons.right || owner.right;
        buttons.middle = buttons.middle || owner.middle;
    }
    return backend_.submitRelativeMouse(dx, dy, buttons, wheel);
}

void SessionService::releasePointerLocked(const uint32_t serial) {
    if (serial == 0 || serial > MAX_BACKEND_CONTROLLERS) return;
    const MouseButtons held = pointerButtons_[serial];
    const bool anyHeld = held.left || held.right || held.middle;
    if (!anyHeld) return;
    pointerButtons_[serial] = MouseButtons{};
    pointerRetryPending_ = !submitPointerLocked(0, 0, 0);
}

void SessionService::releaseConnectionPointerLocked(Connection& conn) {
    for (auto& ctrl : conn.controllers) {
        if (ctrl.active) releasePointerLocked(ctrl.serialNo);
    }
}

void SessionService::recordPointerInjectionLocked(Connection& conn, bool ok) {
    if (ok) {
        pointerInjected_.fetch_add(1, std::memory_order_relaxed);
        if (conn.pointerRefusalLogged) {
            log_.logMsg(LogLevel::INFO, "service",
                        "Host is accepting pointer input for " + conn.deviceName + " again after " +
                            std::to_string(conn.pointerRefusedRun) +
                            (conn.pointerRefusedRun == 1 ? " refused frame" : " refused frames"));
        }
        conn.pointerRefusedRun = 0;
        conn.pointerRefusalLogged = false;
        return;
    }
    pointerRefused_.fetch_add(1, std::memory_order_relaxed);
    conn.pointerRefusedRun++;
    if (conn.pointerRefusedRun != 1) return;
    const auto now = std::chrono::steady_clock::now();
    const bool loggedBefore = conn.pointerRefusalLoggedAt.time_since_epoch().count() != 0;
    if (loggedBefore && now - conn.pointerRefusalLoggedAt <
                            std::chrono::seconds(POINTER_REFUSAL_LOG_INTERVAL_SEC)) {
        return;
    }
    conn.pointerRefusalLoggedAt = now;
    conn.pointerRefusalLogged = true;
    log_.logMsg(LogLevel::WARN, "service",
                "Host refused to inject pointer input for " + conn.deviceName +
                    "; the frames arrive but the OS drops them (on Windows this happens while "
                    "the foreground window belongs to a process with higher privileges than "
                    "Satellite)");
}

void SessionService::handleLightbarFromBackend(uint32_t serial, uint8_t r, uint8_t g, uint8_t b) {
    std::unique_lock<TrackedMutex> lk(mtx_, std::try_to_lock);
    if (!lk.owns_lock()) return;

    Connection* foundConn = nullptr;
    Controller* foundCtrl = nullptr;
    if (!findBySerialLocked(serial, foundConn, foundCtrl)) return;

    // Coalesce unchanged colours (same shape as the rumble coalesce).
    if (foundCtrl->lastLightbarValid && foundCtrl->lightbarR == r && foundCtrl->lightbarG == g &&
        foundCtrl->lightbarB == b) {
        return;
    }
    foundCtrl->lightbarR = r;
    foundCtrl->lightbarG = g;
    foundCtrl->lightbarB = b;
    foundCtrl->lastLightbarValid = true;

    // Emit only to senders that advertised CAP_LIGHTBAR; others would drop it.
    if (foundCtrl->lightbarCapable()) {
        client_.sendLightbar(*foundConn, foundCtrl->index, r, g, b);
    }
}

void SessionService::handleTriggerEffectsFromBackend(uint32_t serial,
                                                     const TriggerEffectsReport& report) {
    std::unique_lock<TrackedMutex> lk(mtx_, std::try_to_lock);
    if (!lk.owns_lock()) return;

    Connection* foundConn = nullptr;
    Controller* foundCtrl = nullptr;
    if (!findBySerialLocked(serial, foundConn, foundCtrl)) return;

    if (foundCtrl->lastTriggerEffectsValid && foundCtrl->lastTriggerEffects == report) return;
    foundCtrl->lastTriggerEffects = report;
    foundCtrl->lastTriggerEffectsValid = true;

    if (foundCtrl->triggerEffectsCapable()) {
        client_.sendTriggerEffects(*foundConn, foundCtrl->index, report);
    }
}

void SessionService::handlePlayerLedsFromBackend(uint32_t serial, uint8_t ledMask) {
    std::unique_lock<TrackedMutex> lk(mtx_, std::try_to_lock);
    if (!lk.owns_lock()) return;

    Connection* foundConn = nullptr;
    Controller* foundCtrl = nullptr;
    if (!findBySerialLocked(serial, foundConn, foundCtrl)) return;

    if (foundCtrl->lastPlayerLedsValid && foundCtrl->playerLeds == ledMask) return;
    foundCtrl->playerLeds = ledMask;
    foundCtrl->lastPlayerLedsValid = true;

    if (foundCtrl->playerLedsCapable()) {
        client_.sendPlayerLeds(*foundConn, foundCtrl->index, ledMask);
    }
}

void SessionService::handleMicLedFromBackend(uint32_t serial, uint8_t state) {
    // Malformed state: the DS5 report byte only ever carries off/on/pulse, so
    // anything else is a decode bug or a hostile report. Drop rather than
    // forward a value the client would have to guess at.
    if (state >= MIC_LED_STATE_COUNT) return;

    std::unique_lock<TrackedMutex> lk(mtx_, std::try_to_lock);
    if (!lk.owns_lock()) return;

    Connection* foundConn = nullptr;
    Controller* foundCtrl = nullptr;
    if (!findBySerialLocked(serial, foundConn, foundCtrl)) return;

    if (foundCtrl->lastMicLedValid && foundCtrl->micLedState == state) return;
    foundCtrl->micLedState = state;
    foundCtrl->lastMicLedValid = true;

    // CAP_MIC, not a lamp cap of its own: the LED reports the state of a
    // microphone the client must actually have.
    if (foundCtrl->micCapable()) { client_.sendMicLed(*foundConn, foundCtrl->index, state); }
}

// The caches above hold what the game last set, sent or not (a slot without the
// cap still caches it), and an unchanged state is never sent twice: a client
// that relaunched and re-PUT would not see it again, nor one set before the
// session could be reached or while the slot lacked the cap. Rumble is a
// command with a lifetime that refreshes on its own, and the audio lanes are
// streams. The mic lamp also mutes the pad's microphone on a client, where the
// user's own mute writes the same lamp: a replay is no new write from the game,
// and would undo an unmute. None of those is replayed.
void SessionService::resendFeedbackStateLocked(const Connection& conn) {
    for (const auto& ctrl : conn.controllers) {
        if (ctrl.active) resendControllerFeedbackStateLocked(conn, ctrl);
    }
}

void SessionService::resendControllerFeedbackStateLocked(const Connection& conn,
                                                         const Controller& ctrl) {
    const bool resendLightbar = ctrl.lastLightbarValid && ctrl.lightbarCapable();
    if (resendLightbar) {
        client_.sendLightbar(conn, ctrl.index, ctrl.lightbarR, ctrl.lightbarG, ctrl.lightbarB);
    }
    const bool resendTriggerEffects = ctrl.lastTriggerEffectsValid && ctrl.triggerEffectsCapable();
    if (resendTriggerEffects) client_.sendTriggerEffects(conn, ctrl.index, ctrl.lastTriggerEffects);
    const bool resendPlayerLeds = ctrl.lastPlayerLedsValid && ctrl.playerLedsCapable();
    if (resendPlayerLeds) client_.sendPlayerLeds(conn, ctrl.index, ctrl.playerLeds);
}

bool SessionService::handleSpeakerAudioFromBackend(uint32_t serial, const int16_t* stereo48k,
                                                   size_t frames) {
    if (stereo48k == nullptr || frames == 0) return false;

    // Sampled before mtx_ is taken: the policy reads the config lock, and the
    // two must never nest. Dropping here rather than at encode time is the
    // whole point of the switch -- an off speaker costs no Opus frames.
    if (!audioPolicy().speaker) return false;

    // Dropping a speaker frame is a 20 ms gap the client's PLC already knows how to cover.
    std::unique_lock<TrackedMutex> lk(mtx_, std::try_to_lock);
    if (!lk.owns_lock()) {
        bumpAudio(audio_.speakerLockContended);
        return false;
    }

    Connection* foundConn = nullptr;
    Controller* foundCtrl = nullptr;
    if (!findBySerialLocked(serial, foundConn, foundCtrl)) return false;

    // Emit only to senders that advertised CAP_SPEAKER; encoding for a client
    // that would drop the frame is pure wasted CPU.
    if (!foundCtrl->speakerCapable()) return false;

    return encodeAndSendAudioLocked(*foundConn, *foundCtrl, OutboundAudioLane::Speaker, stereo48k,
                                    frames);
}

bool SessionService::handleHapticAudioFromBackend(uint32_t serial, const int16_t* stereo48k,
                                                  size_t frames) {
    if (stereo48k == nullptr || frames == 0) return false;

    // Same order as the speaker: the gate is sampled before mtx_ (it reads the
    // config lock; the two never nest), and it covers BOTH renderings. A host
    // that turned haptics off wants no haptics, not haptics-as-rumble.
    if (!audioPolicy().haptics) return false;

    std::unique_lock<TrackedMutex> lk(mtx_, std::try_to_lock);
    if (!lk.owns_lock()) {
        bumpAudio(audio_.hapticLockContended);
        return false;
    }

    Connection* foundConn = nullptr;
    Controller* foundCtrl = nullptr;
    if (!findBySerialLocked(serial, foundConn, foundCtrl)) return false;

    // The waveform to a sender that can play it; a reduction to whoever can
    // only rumble; nothing to a sender with neither. Never both: a client
    // playing the waveform is already rendering the effect, and a motor
    // shaking alongside its own voice coils would double it.
    if (foundCtrl->hapticAudioCapable()) {
        return encodeAndSendAudioLocked(*foundConn, *foundCtrl, OutboundAudioLane::Haptic,
                                        stereo48k, frames);
    }
    if (!foundCtrl->rumbleCapable()) return false;

    // Re-window into whole 20 ms frames exactly as the encode path does, so
    // the reduction sees the same windows the waveform would have been cut
    // into. A partial tail waits for the rest of its frame.
    ControllerAudio& audio = ensureControllerAudioLocked(*foundCtrl);
    ControllerAudio::OutboundLane& lane = audio.haptic;
    const size_t frameSamples = static_cast<size_t>(AUDIO_FRAME_SAMPLES) * AUDIO_HAPTIC_CHANNELS;
    size_t consumed = 0;
    if (!lane.pending.empty()) {
        const size_t need = frameSamples - lane.pending.size();
        const size_t offered = frames * AUDIO_HAPTIC_CHANNELS;
        const size_t take = need < offered ? need : offered;
        lane.pending.insert(lane.pending.end(), stereo48k, stereo48k + take);
        consumed = take / AUDIO_HAPTIC_CHANNELS;
        if (lane.pending.size() < frameSamples) return true;
        reduceHapticWindowToRumbleLocked(*foundConn, *foundCtrl, lane.pending.data());
        lane.pending.clear();
    }
    while (frames - consumed >= static_cast<size_t>(AUDIO_FRAME_SAMPLES)) {
        reduceHapticWindowToRumbleLocked(*foundConn, *foundCtrl,
                                         stereo48k + consumed * AUDIO_HAPTIC_CHANNELS);
        consumed += AUDIO_FRAME_SAMPLES;
    }
    if (consumed < frames) {
        lane.pending.assign(stereo48k + consumed * AUDIO_HAPTIC_CHANNELS,
                            stereo48k + frames * AUDIO_HAPTIC_CHANNELS);
    }
    return true;
}

void SessionService::reduceHapticWindowToRumbleLocked(Connection& conn, Controller& ctrl,
                                                      const int16_t* frame) {
    bumpAudio(audio_.hapticReducedToRumble);
    const RumbleReport level =
        satellite::audio::hapticWindowToRumble(frame, static_cast<size_t>(AUDIO_FRAME_SAMPLES));
    const auto now = std::chrono::steady_clock::now();
    ctrl.hapticRumble = level;
    ctrl.hapticRumbleExpiresAt =
        now + std::chrono::milliseconds(satellite::audio::HAPTIC_RUMBLE_WIRE_DURATION_MS);

    // The client runs each MSG_RUMBLE for its duration. A haptic-only level
    // gets the short one, so a stream that stops takes the motor with it; the
    // moment the HID motors contribute, the mixed value carries their long
    // duration instead, or a game holding plain rumble under a haptic burst
    // would lose it when the burst ended.
    const bool hidContributes =
        ctrl.hidRumble.strongMagnitude != 0 || ctrl.hidRumble.weakMagnitude != 0;
    const uint16_t durationMs =
        hidContributes ? RUMBLE_WIRE_DURATION_MS : satellite::audio::HAPTIC_RUMBLE_WIRE_DURATION_MS;
    // A steady non-zero level must be re-sent before the client's timer runs
    // out, coalescing or not.
    const bool steadyNonZero = level.strongMagnitude != 0 || level.weakMagnitude != 0;
    const bool refreshDue = now - ctrl.hapticRumbleSentAt >=
                            std::chrono::milliseconds(satellite::audio::HAPTIC_RUMBLE_REFRESH_MS);
    if (forwardRumbleLocked(conn, ctrl, durationMs, /*force=*/steadyNonZero && refreshDue)) {
        ctrl.hapticRumbleSentAt = now;
    }
}

bool SessionService::handleMicAudio(uint32_t token, uint8_t ctrlIdx, uint16_t seq,
                                    const uint8_t* opus, size_t opusLen) {
    if (opus == nullptr || opusLen == 0) return false;

    // Sampled before mtx_, as in handleSpeakerAudioFromBackend. Read once and
    // carried into the gate below so the drop still gets its one log line: a
    // client whose mic goes nowhere deserves a reason in the log, and the
    // client cannot see this setting.
    const bool hostWantsMic = audioPolicy().mic;

    rx_.enter();
    PadRead pad;
    // No session: nothing to log against, and an unroutable token is exactly
    // what a stale or spoofed datagram looks like.
    if (!readPad(token, ctrlIdx, pad)) {
        rx_.exit();
        return false;
    }

    uint8_t cause = 0;
    if (!pad.active) {
        cause = MIC_DROP_LOG_NO_CONTROLLER;
    } else if ((hotPadCaps(pad.info) & CAP_MIC) == 0) {
        // Caps advertise the client's own source: a sender that never claimed a
        // microphone has no business streaming one, so this is a gate and not a
        // best-effort hint (unlike MSG_MOTION, which is accepted uncapped).
        cause = MIC_DROP_LOG_NO_CAP;
    } else if (!hostWantsMic) {
        // Host-side off switch. Checked after the cap so a misconfigured client
        // still gets told about its own mistake first, and before the rate-limit
        // window so a disabled direction does not quietly consume budget.
        cause = MIC_DROP_LOG_HOST_DISABLED;
    } else {
        // Fixed one-second window: cheap, and the stream is a steady 50 packets/s
        // so there is no burst shape worth a token bucket. A client running hot
        // loses the overflow, not the session.
        RxPad& rx = rxPadFor(pad);
        const auto now = std::chrono::steady_clock::now();
        if (rx.micPacketsInWindow == 0 ||
            now - hotTime(rx.micWindowStart) >= std::chrono::seconds(1)) {
            rx.micWindowStart = hotTicks(now);
            rx.micPacketsInWindow = 0;
        }
        if (rx.micPacketsInWindow >= MIC_AUDIO_MAX_PACKETS_PER_SEC) {
            cause = MIC_DROP_LOG_RATE_LIMIT;
        } else {
            rx.micPacketsInWindow++;
        }
    }
    rx_.exit();

    if (cause != 0) return dropMicFrame(pad, ctrlIdx, cause);
    return deliverMicFrame(token, ctrlIdx, pad, seq, opus, opusLen);
}

bool SessionService::dropMicFrame(const PadRead& pad, uint8_t ctrlIdx, uint8_t cause) {
    bumpAudio(audio_.micDropped);
    RxSession& session = rxSessions_[static_cast<size_t>(pad.slot)];
    if (session.keyEpoch != pad.keyEpoch) {
        session.keyEpoch = pad.keyEpoch;
        session.micDropLogged = 0;
    }
    if ((session.micDropLogged & cause) != 0) return false;
    std::string name;
    if (!sessionName(pad.slot, pad.keyEpoch, name)) return false;
    session.micDropLogged |= cause;
    std::string why;
    if (cause == MIC_DROP_LOG_NO_CONTROLLER) {
        why = "no such bound controller";
    } else if (cause == MIC_DROP_LOG_NO_CAP) {
        why = "sender never advertised the mic cap";
    } else if (cause == MIC_DROP_LOG_HOST_DISABLED) {
        why = "controller microphone is off on the host";
    } else {
        why = "more than " + std::to_string(MIC_AUDIO_MAX_PACKETS_PER_SEC) + " packets/s";
    }
    log_.logMsg(LogLevel::WARN, "service",
                "Mic audio from " + name + " controller #" + std::to_string(ctrlIdx) +
                    " dropped: " + why);
    return false;
}

bool SessionService::deliverMicFrame(uint32_t token, uint8_t ctrlIdx, const PadRead& pad,
                                     uint16_t seq, const uint8_t* opus, size_t opusLen) {
    std::lock_guard<TrackedMutex> lk(mtx_);
    auto it = connections_.find(token);
    if (it == connections_.end() || ctrlIdx >= MAX_CONTROLLERS_PER_CONN) return false;
    Controller& ctrl = it->second.controllers[ctrlIdx];
    if (!ctrl.active || ctrl.plugGen != pad.gen) return false;
    return deliverMicAudioLocked(it->second, ctrl, seq, opus, opusLen);
}

ControllerAudio& SessionService::ensureControllerAudioLocked(Controller& ctrl) {
    if (!ctrl.audio) ctrl.audio = std::make_unique<ControllerAudio>();
    return *ctrl.audio;
}

bool SessionService::deliverMicAudioLocked(Connection& conn, Controller& ctrl, uint16_t seq,
                                           const uint8_t* opus, size_t opusLen) {
    ControllerAudio& audio = ensureControllerAudioLocked(ctrl);
    const AudioJitterWindow::Result pushed = audio.micWindow.push(seq, opus, opusLen);
    // A frame whose slot has already been played (or concealed past) is worse
    // than useless: splicing it in now would be an audible jump backwards.
    if (pushed.accept != AudioJitterWindow::Accept::Ok) {
        bumpAudio(audio_.micLate);
        return false;
    }
    bumpAudio(audio_.micAccepted);
    // Taken, but held for reordering. Nothing is due until the frame in front
    // of it arrives or is proven lost.
    if (pushed.count == 0) return true;

    // Created here, not at plug: a slot that advertises CAP_MIC and never
    // speaks should not carry a decoder around (see ControllerAudio).
    if (!audio.micDecoder && audioCodecs_ != nullptr) {
        audio.micDecoder = audioCodecs_->makeMicDecoder();
    }
    // No codec in this build, or the codec refused to allocate. The frame still
    // passed every gate, so this is an accept-and-drop, not a rejection.
    if (!audio.micDecoder) return true;

    // Sized by the channel count rather than assuming mono: the decoder writes
    // maxFrames * channels, so a buffer that only happened to be right because
    // AUDIO_MIC_CHANNELS is 1 would become an overflow the day it isn't.
    int16_t pcm[AUDIO_FRAME_SAMPLES * AUDIO_MIC_CHANNELS];
    for (int i = 0; i < pushed.count; i++) {
        const AudioJitterWindow::Event& ev = pushed.events[i];
        size_t decoded = 0;
        if (ev.kind == AudioJitterWindow::Event::Kind::Packet) {
            decoded = audio.micDecoder->decode(ev.data, ev.len, pcm, AUDIO_FRAME_SAMPLES);
            if (decoded > 0) bumpAudio(audio_.micDecoded);
        } else if (ev.fecCarrier != nullptr) {
            // The window hands over packet seq+1 precisely because Opus hides a
            // redundant copy of seq inside it. Order is load-bearing: the FEC
            // copy is decoded here, BEFORE the carrier's own frame, which the
            // window emits next.
            decoded = audio.micDecoder->decodeFec(ev.fecCarrier, ev.fecCarrierLen, pcm,
                                                  AUDIO_FRAME_SAMPLES);
            if (decoded > 0) bumpAudio(audio_.micFecRecovered);
        } else {
            decoded = audio.micDecoder->conceal(pcm, AUDIO_FRAME_SAMPLES);
            if (decoded > 0) bumpAudio(audio_.micConcealed);
        }
        if (decoded == 0) continue;
        // A backend with no mic endpoint on this serial returns false, which is
        // not an error: senders keep streaming and the pad simply has nowhere
        // to put it (IGamepadPort::submitMicAudioPcm). The client cannot see
        // that either, so it gets one line per session, like the drop causes.
        if (backend_.submitMicAudioPcm(ctrl.serialNo, pcm, decoded)) continue;
        RxSession& session = rxSessions_[static_cast<size_t>(conn.slot)];
        const uint32_t keyEpoch = conn.hot->keyEpoch.load(std::memory_order_relaxed);
        if (session.keyEpoch != keyEpoch) {
            session.keyEpoch = keyEpoch;
            session.micDropLogged = 0;
        }
        if ((session.micDropLogged & MIC_DROP_LOG_NO_SINK) == 0) {
            session.micDropLogged |= MIC_DROP_LOG_NO_SINK;
            log_.logMsg(LogLevel::INFO, "service",
                        "Mic audio from " + conn.deviceName + " controller #" +
                            std::to_string(ctrl.index) +
                            " decodes but the emulated pad has no microphone endpoint");
        }
    }
    return true;
}

void SessionService::sendAudioFrameLocked(Connection& conn, Controller& ctrl,
                                          OutboundAudioLane lane,
                                          ControllerAudio::OutboundLane& state,
                                          const int16_t* frame) {
    const bool speaker = lane == OutboundAudioLane::Speaker;
    const int channels = speaker ? AUDIO_SPEAKER_CHANNELS : AUDIO_HAPTIC_CHANNELS;

    // Suppressed frames do NOT advance seq, unlike the encode failure below.
    // A failed encode lost real audio and the client should conceal a hole;
    // digital silence lost nothing, and its correct rendering is the silence a
    // starved sink already plays. Advancing here would instead ask the client
    // to run PLC over a gap that has no signal to guess at, and Opus would
    // invent noise where the game wrote none. Skipping also leaves the encoder
    // and the decoder resting on the same last real frame, so neither drifts.
    // For haptics this is also the steady state: a pad's actuators are silent
    // far more than a speaker is, so the lane costs nothing until a game
    // actually plays an effect.
    if (isDigitalSilence(frame, static_cast<size_t>(AUDIO_FRAME_SAMPLES) *
                                    static_cast<size_t>(channels))) {
        bumpAudio(speaker ? audio_.speakerSilenceSuppressed : audio_.hapticSilenceSuppressed);
        return;
    }

    uint8_t packet[MAX_INNER_PAYLOAD_BYTES - AUDIO_WIRE_HEADER_BYTES];
    const size_t bytes = state.encoder->encode(frame, AUDIO_FRAME_SAMPLES, packet, sizeof(packet));
    // The seq advances even when the encode failed. The 20 ms happened; saying
    // so lets the client conceal a hole instead of silently playing the stream
    // short and drifting against the game's clock.
    const uint16_t seq = state.seq++;
    if (bytes == 0) {
        bumpAudio(speaker ? audio_.speakerEncodeFailed : audio_.hapticEncodeFailed);
        return;
    }
    if (speaker) {
        bumpAudio(audio_.speakerSent);
        client_.sendSpeakerAudio(conn, ctrl.index, seq, packet, bytes);
    } else {
        bumpAudio(audio_.hapticSent);
        client_.sendHapticAudio(conn, ctrl.index, seq, packet, bytes);
    }
}

bool SessionService::encodeAndSendAudioLocked(Connection& conn, Controller& ctrl,
                                              OutboundAudioLane lane, const int16_t* stereo48k,
                                              size_t frames) {
    const bool speaker = lane == OutboundAudioLane::Speaker;
    const int channels = speaker ? AUDIO_SPEAKER_CHANNELS : AUDIO_HAPTIC_CHANNELS;
    ControllerAudio& audio = ensureControllerAudioLocked(ctrl);
    ControllerAudio::OutboundLane& state = speaker ? audio.speaker : audio.haptic;
    if (!state.encoder && audioCodecs_ != nullptr) {
        state.encoder =
            speaker ? audioCodecs_->makeSpeakerEncoder() : audioCodecs_->makeHapticEncoder();
    }
    // No codec: accepted and dropped, same as the inbound direction.
    if (!state.encoder) return true;

    const size_t frameSamples = static_cast<size_t>(AUDIO_FRAME_SAMPLES) * channels;
    size_t consumed = 0; // frames taken from stereo48k

    // A backend hands over whatever its ring held, which is a batch boundary,
    // not a codec boundary: half a window, three windows, anything. Top up the
    // leftover tail first so frames stay contiguous across calls.
    if (!state.pending.empty()) {
        const size_t need = frameSamples - state.pending.size();
        const size_t offered = frames * channels;
        const size_t take = need < offered ? need : offered;
        state.pending.insert(state.pending.end(), stereo48k, stereo48k + take);
        consumed = take / channels;
        if (state.pending.size() < frameSamples) return true; // still short of a window
        sendAudioFrameLocked(conn, ctrl, lane, state, state.pending.data());
        state.pending.clear();
    }

    // Whole windows go straight from the caller's buffer; the steady state
    // (exactly one window per call) never copies.
    while (frames - consumed >= static_cast<size_t>(AUDIO_FRAME_SAMPLES)) {
        sendAudioFrameLocked(conn, ctrl, lane, state, stereo48k + consumed * channels);
        consumed += AUDIO_FRAME_SAMPLES;
    }
    if (consumed < frames) {
        state.pending.assign(stereo48k + consumed * channels, stereo48k + frames * channels);
    }
    return true;
}

bool SessionService::handleBatteryUpdate(uint32_t token, uint8_t ctrlIdx,
                                         const BatteryReport& report) {
    // 0xFF is the documented "unknown" sentinel; 101..254 is malformed.
    if (report.level != BATTERY_LEVEL_UNKNOWN && report.level > 100) return false;
    if (report.status >= BATTERY_STATUS_COUNT) return false;

    rx_.enter();
    PadRead pad;
    const bool accepted = readPad(token, ctrlIdx, pad) && pad.active;
    if (accepted) {
        backend_.submitBattery(pad.serial, report); // no-op on backends without a battery surface
        noteRxWrite();
        publishRxBattery(rxPadFor(pad), report);
    }
    rx_.exit();
    return accepted;
}

bool SessionService::getDecryptInfo(uint32_t token, uint8_t outKey[CRYPTO_KEY_SIZE],
                                    uint32_t& outLastCounter, bool* outSeenCounter) const {
    const int index = hot_.find(token);
    if (index < 0) return false;
    const HotSlot& slot = hot_.slots[static_cast<size_t>(index)];
    uint32_t slotToken = 0;
    uint32_t keyEpoch = 0;
    const bool stable = slot.read([&] {
        slotToken = slot.token.load(std::memory_order_relaxed);
        keyEpoch = slot.keyEpoch.load(std::memory_order_relaxed);
        slot.loadKey(outKey);
    });
    if (!stable || slotToken != token) return false;
    const bool seen = slot.replayKeyEpoch.load(std::memory_order_relaxed) == keyEpoch;
    outLastCounter = seen ? slot.lastCounter.load(std::memory_order_relaxed) : 0;
    if (outSeenCounter != nullptr) *outSeenCounter = seen;
    return true;
}

void SessionService::recordHotDatagram(uint32_t token, uint32_t counter, uint64_t clientAddr) {
    const int index = hot_.find(token);
    if (index < 0) return;
    HotSlot& slot = hot_.slots[static_cast<size_t>(index)];
    uint32_t slotToken = 0;
    uint32_t keyEpoch = 0;
    const bool stable = slot.read([&] {
        slotToken = slot.token.load(std::memory_order_relaxed);
        keyEpoch = slot.keyEpoch.load(std::memory_order_relaxed);
    });
    if (stable && slotToken == token) recordDatagram(slot, keyEpoch, counter, clientAddr);
}

bool SessionService::submitHotFrame(uint32_t token, uint8_t ctrlIdx, const GamepadReport& report,
                                    bool record, uint32_t counter, uint64_t clientAddr) {
    const int index = hot_.find(token);
    if (index < 0) return false;
    HotSlot& slot = hot_.slots[static_cast<size_t>(index)];
    const bool knownIdx = ctrlIdx < MAX_CONTROLLERS_PER_CONN;
    uint32_t slotToken = 0;
    uint32_t keyEpoch = 0;
    uint32_t word = 0;
    const bool stable = slot.read([&] {
        slotToken = slot.token.load(std::memory_order_relaxed);
        keyEpoch = slot.keyEpoch.load(std::memory_order_relaxed);
        if (knownIdx) word = slot.ctrl[ctrlIdx].load(std::memory_order_relaxed);
    });
    if (!stable || slotToken != token) return false;
    if (record) recordDatagram(slot, keyEpoch, counter, clientAddr);
    if ((word & HOT_CTRL_ACTIVE) == 0) return false;
    return backend_.submitReport(word & HOT_CTRL_SERIAL_MASK, report);
}

void SessionService::updatePostDecrypt(uint32_t token, uint32_t counter,
                                       const std::string& clientIP, uint16_t clientPort) {
    updatePostDecryptV4(token, counter, parseIPv4Nbo(clientIP), clientPort);
}

void SessionService::updatePostDecryptV4(uint32_t token, uint32_t counter,
                                         uint32_t ipv4NetworkOrder, uint16_t clientPort) {
    rx_.enter();
    recordHotDatagram(token, counter, packClientAddr(ipv4NetworkOrder, clientPort));
    rx_.exit();
}

bool SessionService::handleGamepadDataAndUpdate(uint32_t token, uint32_t counter,
                                                uint32_t ipv4NetworkOrder, uint16_t clientPort,
                                                uint8_t ctrlIdx, const GamepadReport& report) {
    rx_.enter();
    const bool ok = submitHotFrame(token, ctrlIdx, report, true, counter,
                                   packClientAddr(ipv4NetworkOrder, clientPort));
    rx_.exit();
    return ok;
}

SessionService::ConnectionSnapshot::CtrlInfo
SessionService::snapshotController(const Connection& conn, const Controller& ctrl) const {
    ConnectionSnapshot::CtrlInfo info{};
    info.index = ctrl.index;
    info.serial = ctrl.serialNo;
    info.active = true;
    info.pluggedIn = backend_.isDevicePlugged(ctrl.serialNo);
    info.controllerType = ctrl.controllerType;
    info.touchpadMode = ctrl.touchpadMode;
    info.batteryLevel = BATTERY_LEVEL_UNKNOWN;
    info.batteryStatus = BATTERY_STATUS_UNKNOWN;
    info.motionCapable = ctrl.motionCapable();
    info.motionSinkSupportedForType = backend_.supportsMotionForType(ctrl.controllerType);
    info.backendId = backend_.backendIdForSerial(ctrl.serialNo);
    info.motionBackendOk = backend_.motionBackendOk(ctrl.serialNo);
    info.lightbarCapable = ctrl.lightbarCapable();
    info.lightbarKnown = ctrl.lastLightbarValid;
    info.lightbarR = ctrl.lightbarR;
    info.lightbarG = ctrl.lightbarG;
    info.lightbarB = ctrl.lightbarB;
    RxPadView rx;
    const bool known = ctrl.serialNo >= 1 &&
                       ctrl.serialNo <= static_cast<uint32_t>(MAX_BACKEND_CONTROLLERS) &&
                       readRxPad(rxPads_[ctrl.serialNo], rx) && rx.gen == ctrl.plugGen;
    if (!known) return info;
    info.motionActive = (rx.flags & RX_PAD_MOTION) != 0;
    info.motionSink = (rx.flags & RX_PAD_MOTION_SINK) != 0;
    if (info.motionActive) info.motion = rx.motion;
    info.batteryKnown = (rx.flags & RX_PAD_BATTERY) != 0;
    if (info.batteryKnown) {
        info.batteryLevel = rx.battery.level;
        info.batteryStatus = rx.battery.status;
    }
    const uint32_t keyEpoch = conn.hot->keyEpoch.load(std::memory_order_relaxed);
    info.touchpadActive = (rx.flags & RX_PAD_TOUCHPAD) != 0 &&
                          rx.touchpadMode == ctrl.touchpadMode && rx.touchpadKeyEpoch == keyEpoch;
    if (info.touchpadActive) info.touchpad = rx.touchpad;
    return info;
}

// The REST-open grace window counts as liveness so a fresh PUT doesn't flash
// "not responding".
DeviceLinkState SessionService::linkStateAt(const Connection& conn,
                                            std::chrono::steady_clock::time_point now) {
    const auto stallThreshold =
        std::chrono::seconds(HEARTBEAT_INTERVAL_SEC * HEARTBEAT_STALL_FACTOR);
    const bool inGrace = now < conn.graceUntil;
    const bool stalled = !inGrace && now - lastPacketAt(conn) > stallThreshold;
    return stalled ? DeviceLinkState::NotResponding : DeviceLinkState::Active;
}

SessionService::ConnectionSnapshot
SessionService::snapshotConnection(uint32_t token, const Connection& conn,
                                   std::chrono::steady_clock::time_point now) const {
    ConnectionSnapshot cs;
    cs.connectionId = conn.connectionId;
    cs.token = token;
    cs.deviceId = conn.deviceId;
    cs.deviceName = conn.deviceName;
    const uint64_t clientAddr = conn.hot->clientAddr.load(std::memory_order_acquire);
    cs.clientIP = clientAddr != 0 ? formatIPv4Nbo(clientAddrIPv4(clientAddr)) : conn.clientIP;
    cs.connectedAtEpoch =
        std::chrono::duration_cast<std::chrono::seconds>(conn.connectedAt.time_since_epoch())
            .count();
    cs.epoch = conn.epoch;
    cs.activeControllerCount = conn.activeControllerCount;
    cs.mouseControlGranted = conn.mouseControlGranted;
    cs.protocolVersion = conn.protocolVersion;
    cs.linkState = linkStateAt(conn, now);
    for (const auto& ctrl : conn.controllers) {
        if (ctrl.active) cs.controllers.push_back(snapshotController(conn, ctrl));
    }
    return cs;
}

SessionService::ConnectionsSnapshot SessionService::getConnectionsSnapshot() const {
    std::lock_guard<TrackedMutex> lk(mtx_);
    ConnectionsSnapshot snap;
    snap.totalControllers = 0;
    snap.maxControllers = MAX_BACKEND_CONTROLLERS;
    snap.backendAvailable = backend_.isBusOpen();
    const auto now = std::chrono::steady_clock::now();
    for (const auto& [tok, conn] : connections_) {
        ConnectionSnapshot cs = snapshotConnection(tok, conn, now);
        snap.totalControllers += static_cast<int>(cs.controllers.size());
        snap.connections.push_back(std::move(cs));
    }
    return snap;
}

bool SessionService::isDeviceConnected(const std::string& deviceId) const {
    std::lock_guard<TrackedMutex> lk(mtx_);
    for (auto& [tok, c] : connections_) {
        if (c.deviceId == deviceId) return true;
    }
    return false;
}

DeviceLinkState SessionService::linkStateForDevice(const std::string& deviceId) const {
    std::lock_guard<TrackedMutex> lk(mtx_);
    const auto now = std::chrono::steady_clock::now();
    for (auto& [tok, c] : connections_) {
        if (c.deviceId == deviceId) return linkStateAt(c, now);
    }
    return DeviceLinkState::Paired;
}

int SessionService::reapTimedOut() {
    std::unique_lock<TrackedMutex> topo(topoMtx_, std::try_to_lock);
    if (!topo.owns_lock()) return 0;
    std::vector<RetiredSerial> retired;
    std::unique_lock<TrackedMutex> lk(mtx_);
    auto now = std::chrono::steady_clock::now();
    auto timeout = std::chrono::seconds(HEARTBEAT_INTERVAL_SEC * HEARTBEAT_MISS_MAX);

    int reaped = 0;
    for (auto it = connections_.begin(); it != connections_.end();) {
        Connection& conn = it->second;
        const auto lastPacket = lastPacketAt(conn);
        const auto silentFor = std::chrono::duration_cast<std::chrono::seconds>(now - lastPacket);
        // The REST upsert counts as provisional liveness until graceUntil, so a
        // half-open session (UDP blocked) surfaces client-side instead of
        // flapping through here.
        if (now - lastPacket > timeout && now > conn.graceUntil) {
            log_.logMsg(LogLevel::INFO, "service",
                        "Reaper: timed out connection for " + conn.deviceName + " after " +
                            std::to_string(silentFor.count()) + " s without a packet");
            retireConnectionLocked(conn, retired);
            it = connections_.erase(it);
            reaped++;
            continue;
        }
        if (linkStateAt(conn, now) == DeviceLinkState::NotResponding) {
            if (!conn.stallLogged) {
                conn.stallLogged = true;
                const auto reapIn = timeout - silentFor;
                log_.logMsg(LogLevel::WARN, "service",
                            "Link to " + conn.deviceName + " has gone silent: no packet for " +
                                std::to_string(silentFor.count()) + " s (reaper in " +
                                std::to_string(reapIn.count()) + " s)");
            }
        } else {
            conn.stallLogged = false;
        }
        ++it;
    }
    lk.unlock();
    if (reaped > 0) finishTeardown(retired);
    return reaped;
}

bool SessionService::isBackendAvailable() const { return backend_.isBusOpen(); }

int SessionService::activeSessionCount() const {
    std::lock_guard<TrackedMutex> lk(mtx_);
    return static_cast<int>(connections_.size());
}

AudioStreamCounts SessionService::audioCounts() const {
    AudioStreamCounts c;
    c.micAccepted = audio_.micAccepted.load(std::memory_order_relaxed);
    c.micDropped = audio_.micDropped.load(std::memory_order_relaxed);
    c.micLate = audio_.micLate.load(std::memory_order_relaxed);
    c.micDecoded = audio_.micDecoded.load(std::memory_order_relaxed);
    c.micFecRecovered = audio_.micFecRecovered.load(std::memory_order_relaxed);
    c.micConcealed = audio_.micConcealed.load(std::memory_order_relaxed);
    c.speakerSent = audio_.speakerSent.load(std::memory_order_relaxed);
    c.speakerSilenceSuppressed = audio_.speakerSilenceSuppressed.load(std::memory_order_relaxed);
    c.speakerEncodeFailed = audio_.speakerEncodeFailed.load(std::memory_order_relaxed);
    c.speakerLockContended = audio_.speakerLockContended.load(std::memory_order_relaxed);
    c.hapticSent = audio_.hapticSent.load(std::memory_order_relaxed);
    c.hapticSilenceSuppressed = audio_.hapticSilenceSuppressed.load(std::memory_order_relaxed);
    c.hapticEncodeFailed = audio_.hapticEncodeFailed.load(std::memory_order_relaxed);
    c.hapticLockContended = audio_.hapticLockContended.load(std::memory_order_relaxed);
    c.hapticReducedToRumble = audio_.hapticReducedToRumble.load(std::memory_order_relaxed);
    return c;
}

PointerCounts SessionService::pointerCounts() const {
    PointerCounts c;
    c.injected = pointerInjected_.load(std::memory_order_relaxed);
    c.refused = pointerRefused_.load(std::memory_order_relaxed);
    return c;
}

#ifdef SATELLITE_BUILD_TESTS
void SessionService::backdateForTest(uint32_t token, int lastPacketSecondsAgo,
                                     int graceSecondsAgo) {
    std::lock_guard<TrackedMutex> lk(mtx_);
    auto it = connections_.find(token);
    if (it == connections_.end()) return;
    const auto now = std::chrono::steady_clock::now();
    it->second.hot->lastPacketTicks.store(
        hotTicks(now - std::chrono::seconds(lastPacketSecondsAgo)), std::memory_order_relaxed);
    it->second.graceUntil = now - std::chrono::seconds(graceSecondsAgo);
}

void SessionService::resetMicRateWindowForTest(uint32_t token, uint8_t ctrlIdx) {
    std::lock_guard<TrackedMutex> lk(mtx_);
    auto it = connections_.find(token);
    if (it == connections_.end() || ctrlIdx >= MAX_CONTROLLERS_PER_CONN) return;
    const Controller& ctrl = it->second.controllers[ctrlIdx];
    if (!ctrl.active) return;
    RxPad& rx = rxPads_[ctrl.serialNo];
    if (rx.ownGen != ctrl.plugGen) return;
    // Exactly what handleMicAudio's window lapse does, without the second.
    rx.micWindowStart = 0;
    rx.micPacketsInWindow = 0;
}

void SessionService::setSpeakerSeqForTest(uint32_t token, uint8_t ctrlIdx, uint16_t seq) {
    std::lock_guard<TrackedMutex> lk(mtx_);
    auto it = connections_.find(token);
    if (it == connections_.end() || ctrlIdx >= MAX_CONTROLLERS_PER_CONN) return;
    ensureControllerAudioLocked(it->second.controllers[ctrlIdx]).speaker.seq = seq;
}

void SessionService::setHapticSeqForTest(uint32_t token, uint8_t ctrlIdx, uint16_t seq) {
    std::lock_guard<TrackedMutex> lk(mtx_);
    auto it = connections_.find(token);
    if (it == connections_.end() || ctrlIdx >= MAX_CONTROLLERS_PER_CONN) return;
    ensureControllerAudioLocked(it->second.controllers[ctrlIdx]).haptic.seq = seq;
}

void SessionService::backdatePointerRefusalLogForTest(uint32_t token, int secondsAgo) {
    std::lock_guard<TrackedMutex> lk(mtx_);
    auto it = connections_.find(token);
    if (it == connections_.end()) return;
    it->second.pointerRefusalLoggedAt =
        std::chrono::steady_clock::now() - std::chrono::seconds(secondsAgo);
}

SessionService::HeartbeatAckView SessionService::coldHeartbeatAckForTest(uint32_t token) const {
    std::lock_guard<TrackedMutex> lk(mtx_);
    HeartbeatAckView view;
    auto it = connections_.find(token);
    if (it == connections_.end()) return view;
    view.found = true;
    view.backendAvailable = backend_.isBusOpen();
    view.totalActiveControllers = static_cast<uint8_t>(countGlobalActiveControllers());
    view.epoch = it->second.epoch;
    view.activeBitmap = activeBitmapLocked(it->second);
    return view;
}

void SessionService::bindReceiveThreadForTest() {
    rxOwnerForTest_.store(std::this_thread::get_id(), std::memory_order_relaxed);
}

uint64_t SessionService::foreignRxWritesForTest() const {
    return foreignRxWrites_.load(std::memory_order_relaxed);
}
#endif

int SessionService::totalActiveControllers() const {
    std::lock_guard<TrackedMutex> lk(mtx_);
    return countGlobalActiveControllers();
}

int SessionService::availableSlots() const {
    std::lock_guard<TrackedMutex> lk(mtx_);
    int slots = 0;
    for (size_t i = 0; i < MAX_BACKEND_CONTROLLERS; i++) {
        if (!serialInUse_[i] && !serialQuarantined_[i]) slots++;
    }
    return slots;
}
