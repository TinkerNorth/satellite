// SPDX-License-Identifier: LGPL-3.0-or-later
#include "../src/core/session_service.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "test_util.h"

namespace {

using Clock = std::chrono::steady_clock;

constexpr int SESSION_COUNT = 2;
constexpr uint8_t PADS_PER_SESSION = 8;
constexpr int NO_OWNER = 0;
constexpr int ZOMBIE_OWNER = -1;
constexpr int PHASES = 3;
constexpr int STRESS_PHASE = 0;
constexpr int SLOW_PLUG_PHASE = 1;
constexpr int DONE_PHASE = 2;
constexpr auto STRESS_SPAN = std::chrono::seconds(1);
constexpr int SLOW_PLUGS = 4;
constexpr int SLOW_PLUG_MS = 200;
constexpr auto BLOCKED_CALL_LIMIT = std::chrono::milliseconds(50);
constexpr auto THREAD_TIMEOUT = std::chrono::seconds(60);
constexpr uint32_t SENDER_IPV4 = 0x0100007fu;
constexpr uint16_t SENDER_PORT = 40100;
constexpr uint32_t PLUG_FAILURE_ONE_IN = 10;
constexpr uint32_t UNPLUG_FAILURE_ONE_IN = 20;
constexpr int REAP_SILENCE_SEC = 60;

int ownerCode(int tag, int ctrlIdx) { return tag * 256 + ctrlIdx + 1; }

std::string ownerName(int tag, int ctrlIdx) {
    return std::to_string(tag) + ":" + std::to_string(ctrlIdx);
}

int parseOwner(const std::string& name) {
    const size_t colon = name.find(':');
    if (colon == std::string::npos) return ZOMBIE_OWNER;
    return ownerCode(std::stoi(name.substr(0, colon)), std::stoi(name.substr(colon + 1)));
}

class OwnershipBackend : public IGamepadPort {
  public:
    std::atomic<int> slowPlugMs{0};
    std::atomic<bool> flaky{true};
    std::atomic<uint64_t> violations{0};
    std::atomic<uint64_t> plugs{0};
    std::atomic<uint64_t> unplugs{0};
    std::atomic<uint64_t> coldSubmits{0};
    std::atomic<uint64_t> micSubmits{0};

    bool ensureBusOpen() override {
        busOpen_.store(true);
        return true;
    }

    void closeBus() override {
        for (uint32_t serial = 1; serial <= MAX_BACKEND_CONTROLLERS; serial++) {
            Serial& s = serials_[serial];
            s.busy.store(true);
            if (s.submitting.load()) violations++;
            s.plugged.store(false);
            s.owner.store(NO_OWNER);
            s.busy.store(false);
        }
        busOpen_.store(false);
    }

    bool isBusOpen() const override { return busOpen_.load(); }

    bool pluginDevice(uint32_t serial, GamepadIdentity identity) override {
        return pluginDevicePreferring(serial, identity, std::string());
    }

    bool pluginDevicePreferring(uint32_t serial, GamepadIdentity,
                                const std::string& preferredBackend) override {
        if (!valid(serial)) return false;
        Serial& s = serials_[serial];
        s.busy.store(true);
        const uint64_t plugNumber = plugNumber_.fetch_add(1) + 1;
        plugState_.store((plugNumber << 1) | 1u);
        if (s.submitting.load()) violations++;
        if (s.plugged.load()) violations++;
        const int sleepMs = slowPlugMs.load();
        if (sleepMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
        const bool ok = !flaky.load() || rng_() % PLUG_FAILURE_ONE_IN != 0;
        if (ok) {
            s.owner.store(parseOwner(preferredBackend));
            s.plugged.store(true);
            plugs++;
        }
        if (s.submitting.load()) violations++;
        plugState_.store(plugNumber << 1);
        s.busy.store(false);
        return ok;
    }

    bool unplugDevice(uint32_t serial) override {
        if (!valid(serial)) return true;
        Serial& s = serials_[serial];
        s.busy.store(true);
        if (s.submitting.load()) violations++;
        const bool confirmed = !flaky.load() || rng_() % UNPLUG_FAILURE_ONE_IN != 0;
        if (confirmed) {
            s.plugged.store(false);
            s.owner.store(NO_OWNER);
        } else {
            s.owner.store(ZOMBIE_OWNER);
        }
        if (s.submitting.load()) violations++;
        s.busy.store(false);
        unplugs++;
        return confirmed;
    }

    bool isDevicePlugged(uint32_t serial) const override {
        return valid(serial) && serials_[serial].plugged.load();
    }

    bool submitReport(uint32_t serial, const GamepadReport& report) override {
        return ownedSubmit(serial, report.sThumbLX, report.sThumbLY);
    }

    bool submitMotion(uint32_t serial, const MotionReport& report) override {
        coldSubmits++;
        return ownedSubmit(serial, report.gyroX, report.gyroY);
    }

    bool submitTouchpad(uint32_t serial, const TouchpadReport& report) override {
        coldSubmits++;
        return ownedSubmit(serial, report.finger0.x, report.finger0.y);
    }

    bool submitBattery(uint32_t serial, const BatteryReport& report) override {
        coldSubmits++;
        return ownedSubmit(serial, report.level / PADS_PER_SESSION,
                           report.level % PADS_PER_SESSION);
    }

    bool submitMicAudioPcm(uint32_t serial, const int16_t* mono48k, size_t samples) override {
        micSubmits++;
        if (samples < 2) {
            violations++;
            return false;
        }
        return ownedSubmit(serial, mono48k[0], mono48k[1]);
    }

    void setRumbleCallback(RumbleCallback) override {}

    uint64_t plugState() const { return plugState_.load(); }

    static bool withinOnePlug(uint64_t before, uint64_t after) {
        return before == after && (before & 1u) != 0;
    }

  private:
    struct Serial {
        std::atomic<bool> busy{false};
        std::atomic<int> submitting{0};
        std::atomic<bool> plugged{false};
        std::atomic<int> owner{NO_OWNER};
    };

    static bool valid(uint32_t serial) { return serial >= 1 && serial <= MAX_BACKEND_CONTROLLERS; }

    bool ownedSubmit(uint32_t serial, int tag, int ctrlIdx) {
        if (!valid(serial)) {
            violations++;
            return false;
        }
        Serial& s = serials_[serial];
        s.submitting.fetch_add(1);
        if (s.busy.load()) violations++;
        if (!s.plugged.load()) violations++;
        if (s.owner.load() != ownerCode(tag, ctrlIdx)) violations++;
        s.submitting.fetch_sub(1);
        return true;
    }

    std::array<Serial, MAX_BACKEND_CONTROLLERS + 1> serials_{};
    std::atomic<bool> busOpen_{false};
    std::atomic<uint64_t> plugNumber_{0};
    std::atomic<uint64_t> plugState_{0};
    std::mt19937 rng_{20261026u};
};

struct SilentClient : IClientPort {
    void updateClientAddr(uint32_t, const std::string&, uint16_t) override {}
    void removeClientAddr(uint32_t) override {}
    void sendHeartbeatAck(const SessionEndpoint&, bool, uint8_t, uint16_t, uint16_t) override {}
    void sendSessionClose(const Connection&, uint8_t) override {}
    void sendRumble(const Connection&, uint8_t, const RumbleReport&) override {}
    void sendLightbar(const Connection&, uint8_t, uint8_t, uint8_t, uint8_t) override {}
    void sendTriggerEffects(const Connection&, uint8_t, const TriggerEffectsReport&) override {}
    void sendPlayerLeds(const Connection&, uint8_t, uint8_t) override {}
    void sendSpeakerAudio(const Connection&, uint8_t, uint16_t, const uint8_t*, size_t) override {}
    void sendHapticAudio(const Connection&, uint8_t, uint16_t, const uint8_t*, size_t) override {}
    void sendMicLed(const Connection&, uint8_t, uint8_t) override {}
};

struct SilentLog : ILogPort {
    void logMsg(LogLevel, const std::string&, const std::string&) override {}
};

class OwnerEchoDecoder : public IAudioDecoder {
  public:
    size_t decode(const uint8_t* opus, size_t opusLen, int16_t* pcm, size_t maxFrames) override {
        if (opusLen < 2 || maxFrames < static_cast<size_t>(AUDIO_FRAME_SAMPLES)) return 0;
        tag_ = opus[0];
        idx_ = opus[1];
        return conceal(pcm, maxFrames);
    }

    size_t conceal(int16_t* pcm, size_t maxFrames) override {
        if (maxFrames < static_cast<size_t>(AUDIO_FRAME_SAMPLES)) return 0;
        for (size_t i = 0; i < static_cast<size_t>(AUDIO_FRAME_SAMPLES); i++) pcm[i] = 0;
        pcm[0] = static_cast<int16_t>(tag_);
        pcm[1] = static_cast<int16_t>(idx_);
        return static_cast<size_t>(AUDIO_FRAME_SAMPLES);
    }

    size_t decodeFec(const uint8_t* opus, size_t opusLen, int16_t* pcm, size_t maxFrames) override {
        return decode(opus, opusLen, pcm, maxFrames);
    }

  private:
    int tag_ = 0;
    int idx_ = 0;
};

struct OwnerEchoCodecs : IAudioCodecFactory {
    std::unique_ptr<IAudioDecoder> makeMicDecoder() override {
        return std::make_unique<OwnerEchoDecoder>();
    }
    std::unique_ptr<IAudioEncoder> makeSpeakerEncoder() override { return nullptr; }
    std::unique_ptr<IAudioEncoder> makeHapticEncoder() override { return nullptr; }
};

struct Shared {
    std::array<std::atomic<uint32_t>, SESSION_COUNT + 1> tokens{};
    std::atomic<int> phase{STRESS_PHASE};
    std::atomic<bool> stop{false};
    std::atomic<bool> receiverDone{false};
    std::atomic<bool> coldDone{false};
    Clock::duration longest[PHASES] = {};
    uint64_t submits[PHASES][SESSION_COUNT + 1] = {};
    uint64_t lookupMisses[PHASES][SESSION_COUNT + 1] = {};
    uint64_t lookupsInPlug[PHASES][SESSION_COUNT + 1] = {};
    uint64_t missesInPlug[PHASES][SESSION_COUNT + 1] = {};
    uint64_t keyMismatches = 0;
    uint64_t calls = 0;
    uint64_t coldCalls = 0;
    uint64_t coldOps = 0;
    uint64_t coldRefusals = 0;
    uint64_t valueChecks = 0;
    uint64_t staleValues = 0;
    uint16_t micSeq[SESSION_COUNT + 1][PADS_PER_SESSION] = {};
};

struct SessionState {
    int tag = 0;
    std::string deviceId;
    std::string connectionId;
    uint8_t pairingKey[CRYPTO_KEY_SIZE] = {};
};

void keyFor(int tag, uint8_t out[CRYPTO_KEY_SIZE]) {
    for (int i = 0; i < CRYPTO_KEY_SIZE; i++) out[i] = static_cast<uint8_t>(tag * 16 + i);
}

template <typename Call> void timed(Shared& sh, Call&& call) {
    const int startedIn = sh.phase.load(std::memory_order_acquire);
    const auto start = Clock::now();
    call();
    const auto spent = Clock::now() - start;
    const int endedIn = sh.phase.load(std::memory_order_acquire);
    for (int phase = startedIn; phase <= endedIn; phase++) {
        if (spent > sh.longest[phase]) sh.longest[phase] = spent;
    }
}

void coldTraffic(SessionService& svc, Shared& sh, int tag, uint32_t token, uint8_t idx,
                 uint32_t& counter) {
    MotionReport motion{};
    motion.gyroX = static_cast<int16_t>(tag);
    motion.gyroY = static_cast<int16_t>(idx);
    TouchpadReport touch{};
    touch.finger0.active = true;
    touch.finger0.x = static_cast<int16_t>(tag);
    touch.finger0.y = static_cast<int16_t>(idx);
    BatteryReport battery{};
    battery.level = static_cast<uint8_t>(tag * PADS_PER_SESSION + idx);
    battery.status = BATTERY_STATUS_DISCHARGING;
    const auto record = [&] {
        timed(sh, [&] { svc.updatePostDecryptV4(token, ++counter, SENDER_IPV4, SENDER_PORT); });
    };
    record();
    timed(sh, [&] { svc.handleMotionData(token, idx, motion); });
    record();
    timed(sh, [&] { svc.handleTouchpadData(token, idx, touch); });
    record();
    timed(sh, [&] { svc.handleBatteryUpdate(token, idx, battery); });
    record();
    const uint8_t owner[2] = {static_cast<uint8_t>(tag), idx};
    timed(sh, [&] { svc.handleMicAudio(token, idx, sh.micSeq[tag][idx]++, owner, sizeof(owner)); });
    sh.coldCalls += 4;
}

int tagOf(const std::string& deviceId) {
    for (int tag = 1; tag <= SESSION_COUNT; tag++) {
        if (deviceId == "dev" + std::to_string(tag)) return tag;
    }
    return 0;
}

void checkSnapshotValues(SessionService& svc, Shared& sh) {
    for (const auto& conn : svc.getConnectionsSnapshot().connections) {
        const int tag = tagOf(conn.deviceId);
        for (const auto& ctrl : conn.controllers) {
            const int idx = ctrl.index;
            if (ctrl.motionActive) {
                sh.valueChecks++;
                if (ctrl.motion.gyroX != tag || ctrl.motion.gyroY != idx) sh.staleValues++;
            }
            if (ctrl.batteryKnown) {
                sh.valueChecks++;
                if (ctrl.batteryLevel != tag * PADS_PER_SESSION + idx) sh.staleValues++;
            }
            if (ctrl.touchpadActive) {
                sh.valueChecks++;
                if (ctrl.touchpad.finger0.x != tag || ctrl.touchpad.finger0.y != idx) {
                    sh.staleValues++;
                }
            }
        }
    }
}

void receiveLoop(SessionService& svc, const OwnershipBackend& backend, Shared& sh) {
    svc.bindReceiveThreadForTest();
    uint8_t expected[SESSION_COUNT + 1][CRYPTO_KEY_SIZE];
    for (int tag = 1; tag <= SESSION_COUNT; tag++) keyFor(tag, expected[tag]);
    uint32_t counter = 0;
    while (!sh.stop.load(std::memory_order_acquire)) {
        const int phase = sh.phase.load(std::memory_order_acquire);
        for (int tag = 1; tag <= SESSION_COUNT; tag++) {
            const uint32_t token = sh.tokens[tag].load(std::memory_order_acquire);
            if (token == 0) continue;
            uint8_t key[CRYPTO_KEY_SIZE];
            uint32_t last = 0;
            bool seen = false;
            const uint64_t before = backend.plugState();
            const bool found = svc.getDecryptInfo(token, key, last, &seen);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (OwnershipBackend::withinOnePlug(before, backend.plugState())) {
                sh.lookupsInPlug[phase][tag]++;
                if (!found) sh.missesInPlug[phase][tag]++;
            }
            if (!found) {
                sh.lookupMisses[phase][tag]++;
                continue;
            }
            if (std::memcmp(key, expected[tag], CRYPTO_KEY_SIZE) != 0) sh.keyMismatches++;
            for (uint8_t idx = 0; idx < PADS_PER_SESSION; idx++) {
                GamepadReport report{};
                report.sThumbLX = static_cast<int16_t>(tag);
                report.sThumbLY = static_cast<int16_t>(idx);
                bool ok = false;
                timed(sh, [&] {
                    ok = svc.handleGamepadDataAndUpdate(token, ++counter, SENDER_IPV4, SENDER_PORT,
                                                        idx, report);
                });
                if (ok) sh.submits[phase][tag]++;
                sh.calls++;
                coldTraffic(svc, sh, tag, token, idx, counter);
            }
            timed(sh, [&] { svc.updatePostDecryptV4(token, ++counter, SENDER_IPV4, SENDER_PORT); });
            timed(sh, [&] { svc.handleHeartbeat(token); });
            sh.coldCalls++;
        }
    }
    sh.receiverDone.store(true, std::memory_order_release);
}

ControllerDescriptor descriptorFor(int tag, uint8_t idx, uint8_t type) {
    ControllerDescriptor d;
    d.ctrlIdx = idx;
    d.type = type;
    d.touchpadMode = TOUCHPAD_MODE_DS4;
    d.caps = CAP_MIC;
    d.preferredBackend = ownerName(tag, idx);
    return d;
}

uint8_t randomType(std::mt19937& rng) {
    return rng() % 2 == 0 ? CONTROLLER_TYPE_XBOX : CONTROLLER_TYPE_PLAYSTATION;
}

void markClosed(SessionState& st, Shared& sh) {
    st.connectionId.clear();
    sh.tokens[st.tag].store(0, std::memory_order_release);
}

void upsert(SessionService& svc, SessionState& st, Shared& sh,
            const std::vector<ControllerDescriptor>& descriptors) {
    const SessionUpsertResult r =
        svc.upsertSession(st.deviceId, st.deviceId, "127.0.0.1", st.pairingKey, descriptors, false);
    if (!r.ok) {
        sh.coldRefusals++;
        return;
    }
    st.connectionId = r.connectionId;
    sh.tokens[st.tag].store(r.token, std::memory_order_release);
}

void randomUpsert(SessionService& svc, SessionState& st, Shared& sh, std::mt19937& rng) {
    std::vector<ControllerDescriptor> descriptors;
    for (uint8_t idx = 0; idx < PADS_PER_SESSION; idx++) {
        if (rng() % 2 == 0) descriptors.push_back(descriptorFor(st.tag, idx, randomType(rng)));
    }
    upsert(svc, st, sh, descriptors);
}

void randomColdOp(SessionService& svc, SessionState& st, Shared& sh, std::mt19937& rng) {
    if (st.connectionId.empty()) {
        randomUpsert(svc, st, sh, rng);
        return;
    }
    const uint8_t idx = static_cast<uint8_t>(rng() % PADS_PER_SESSION);
    uint16_t epoch = 0;
    switch (rng() % 6) {
    case 0:
        randomUpsert(svc, st, sh, rng);
        break;
    case 1:
    case 2: {
        ControllerApplyResult applied;
        const auto desc = descriptorFor(st.tag, idx, randomType(rng));
        if (!svc.applyController(st.connectionId, st.deviceId, desc, applied, epoch)) {
            sh.coldRefusals++;
        }
        break;
    }
    case 3:
        if (!svc.removeController(st.connectionId, st.deviceId, idx, epoch)) sh.coldRefusals++;
        break;
    case 4:
        svc.closeSessionById(st.connectionId, st.deviceId, CLOSE_REASON_KICKED, false);
        markClosed(st, sh);
        break;
    default: {
        const uint32_t token = sh.tokens[st.tag].load(std::memory_order_relaxed);
        svc.backdateForTest(token, REAP_SILENCE_SEC, 1);
        svc.reapTimedOut();
        break;
    }
    }
    sh.coldOps++;
}

std::vector<ControllerDescriptor> everyPad(int tag) {
    std::vector<ControllerDescriptor> descriptors;
    for (uint8_t idx = 0; idx < PADS_PER_SESSION; idx++) {
        descriptors.push_back(descriptorFor(tag, idx, CONTROLLER_TYPE_XBOX));
    }
    return descriptors;
}

void coldLoop(SessionService& svc, OwnershipBackend& backend, Shared& sh,
              std::array<SessionState, SESSION_COUNT + 1>& sessions) {
    std::mt19937 rng(7u);
    const auto stressEnd = Clock::now() + STRESS_SPAN;
    while (Clock::now() < stressEnd) {
        const int tag = 1 + static_cast<int>(rng() % SESSION_COUNT);
        randomColdOp(svc, sessions[static_cast<size_t>(tag)], sh, rng);
        checkSnapshotValues(svc, sh);
        for (int other = 1; other <= SESSION_COUNT; other++) {
            SessionState& st = sessions[static_cast<size_t>(other)];
            if (!st.connectionId.empty() && !svc.isDeviceConnected(st.deviceId)) {
                markClosed(st, sh);
            }
        }
    }

    backend.flaky.store(false);
    svc.closeAllSessions(CLOSE_REASON_SHUTDOWN);
    for (int tag = 1; tag <= SESSION_COUNT; tag++) {
        markClosed(sessions[static_cast<size_t>(tag)], sh);
    }
    SessionState& steady = sessions[1];
    SessionState& changing = sessions[2];
    upsert(svc, steady, sh, everyPad(steady.tag));
    upsert(svc, changing, sh, {});

    sh.phase.store(SLOW_PLUG_PHASE, std::memory_order_release);
    backend.slowPlugMs.store(SLOW_PLUG_MS);
    for (int i = 0; i < SLOW_PLUGS; i++) {
        ControllerApplyResult applied;
        uint16_t epoch = 0;
        const auto desc = descriptorFor(changing.tag, 0, CONTROLLER_TYPE_XBOX);
        svc.applyController(changing.connectionId, changing.deviceId, desc, applied, epoch);
        svc.removeController(changing.connectionId, changing.deviceId, 0, epoch);
    }
    backend.slowPlugMs.store(0);
    sh.phase.store(DONE_PHASE, std::memory_order_release);
    sh.coldDone.store(true, std::memory_order_release);
}

bool waitFor(const std::atomic<bool>& flag) {
    const auto deadline = Clock::now() + THREAD_TIMEOUT;
    while (!flag.load(std::memory_order_acquire)) {
        if (Clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

double micros(Clock::duration d) { return std::chrono::duration<double, std::micro>(d).count(); }

void test_hotPath_coldOperationsNeverCrossFrames() {
    TEST("hot path: topology churn on the cold path never lets a frame reach the wrong pad");
    OwnershipBackend backend;
    SilentClient client;
    SilentLog log;
    OwnerEchoCodecs codecs;
    SessionService svc(backend, client, log, {}, &codecs);
    Shared sh;
    std::array<SessionState, SESSION_COUNT + 1> sessions;
    for (int tag = 1; tag <= SESSION_COUNT; tag++) {
        SessionState& st = sessions[static_cast<size_t>(tag)];
        st.tag = tag;
        st.deviceId = "dev" + std::to_string(tag);
        keyFor(tag, st.pairingKey);
        upsert(svc, st, sh, everyPad(tag));
    }

    std::thread receiver(receiveLoop, std::ref(svc), std::cref(backend), std::ref(sh));
    std::thread cold(coldLoop, std::ref(svc), std::ref(backend), std::ref(sh), std::ref(sessions));
    const bool coldFinished = waitFor(sh.coldDone);
    sh.stop.store(true, std::memory_order_release);
    const bool receiverFinished = waitFor(sh.receiverDone);
    if (!coldFinished || !receiverFinished) {
        std::cerr << "  FAIL [" << g_currentTest << "] a thread never finished (cold "
                  << coldFinished << ", receiver " << receiverFinished << ")\n";
        std::exit(1);
    }
    cold.join();
    receiver.join();
    svc.closeAllSessions(CLOSE_REASON_SHUTDOWN);

    EXPECT_EQ(backend.violations.load(), 0u);
    EXPECT_EQ(sh.keyMismatches, 0u);
    EXPECT(sh.coldOps > 100);
    EXPECT(backend.plugs.load() > 100);
    EXPECT(sh.submits[STRESS_PHASE][1] > 0);
    EXPECT(sh.submits[STRESS_PHASE][2] > 0);
    EXPECT(backend.coldSubmits.load() > 0);
    EXPECT_EQ(svc.totalActiveControllers(), 0);

    TEST("hot path: the snapshot's cached stream values always belong to the pad they show");
    EXPECT(sh.valueChecks > 0);
    EXPECT_EQ(sh.staleValues, 0u);
    TEST("hot path: mic submits reach only their own plugged pad, never during its plug or unplug");
    EXPECT(backend.micSubmits.load() > 0);
    std::cout << "  mic: " << backend.micSubmits.load() << " submits\n";
    TEST("hot path: only the receive thread writes the receive-owned pad state");
    EXPECT_EQ(svc.foreignRxWritesForTest(), 0u);
    std::cout << "  stream values: " << sh.valueChecks << " checked, " << sh.staleValues
              << " stale, foreign writes " << svc.foreignRxWritesForTest() << "\n";
    std::cout << "  stress: " << sh.coldOps << " cold ops (" << sh.coldRefusals << " refused), "
              << backend.plugs.load() << " plugs, " << backend.unplugs.load() << " unplugs, "
              << sh.calls << " hot calls, " << sh.coldCalls << " cold-dispatch calls ("
              << backend.coldSubmits.load() << " reached the backend), submits A "
              << sh.submits[STRESS_PHASE][1] << " B " << sh.submits[STRESS_PHASE][2]
              << ", violations " << backend.violations.load() << "\n";

    TEST("hot path: a 50 ms plug for one session never blocks another session's frames");
    EXPECT(sh.submits[SLOW_PLUG_PHASE][1] > 0);
    EXPECT(sh.longest[SLOW_PLUG_PHASE] < BLOCKED_CALL_LIMIT);
    std::cout << "  slow plugs: longest receive-thread call " << micros(sh.longest[SLOW_PLUG_PHASE])
              << " us across " << SLOW_PLUGS << " plugs of " << SLOW_PLUG_MS
              << " ms; steady session submits " << sh.submits[SLOW_PLUG_PHASE][1] << "\n";

    TEST("hot path: a session's own lookups are never refused while its plug runs");
    EXPECT(sh.lookupsInPlug[SLOW_PLUG_PHASE][2] > 0);
    EXPECT_EQ(sh.missesInPlug[SLOW_PLUG_PHASE][2], 0u);
    std::cout << "  own plug: changing session lookups inside its plugs "
              << sh.lookupsInPlug[SLOW_PLUG_PHASE][2] << ", refused "
              << sh.missesInPlug[SLOW_PLUG_PHASE][2] << "\n";
}

} // namespace

int main() {
    test_hotPath_coldOperationsNeverCrossFrames();

    std::cout << "\n=== Test Results ===\n";
    std::cout << "  Passed: " << g_pass << "\n";
    std::cout << "  Failed: " << g_fail << "\n";
    if (g_fail > 0) {
        std::cout << "  STATUS: FAIL\n";
        return 1;
    }
    std::cout << "  STATUS: ALL PASSED\n";
    return 0;
}
