// SPDX-License-Identifier: LGPL-3.0-or-later
#include "../src/core/session_service.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "test_util.h"

namespace {

using Clock = std::chrono::steady_clock;

constexpr auto PROBE_LIMIT = std::chrono::milliseconds(200);
constexpr int SLOW_PLUG_MS = 200;
constexpr int SLOW_ROUNDS = 4;
constexpr auto BLOCKED_CALL_LIMIT = std::chrono::milliseconds(50);
constexpr auto THREAD_TIMEOUT = std::chrono::seconds(60);
constexpr uint64_t FRAMES_ON_NEW_TOKEN = 200;
constexpr uint32_t SENDER_IPV4 = 0x0100007fu;
constexpr uint16_t SENDER_PORT = 40200;
constexpr int REAP_SILENCE_SEC = 60;
constexpr uint8_t BATTERY_HALF = 50;
const uint8_t FULL_BUS_SECOND_SESSION_PADS = static_cast<uint8_t>(MAX_BACKEND_CONTROLLERS - 1);

const uint8_t PAIRING_KEY[CRYPTO_KEY_SIZE] = {7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17,
                                              18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28,
                                              29, 30, 31, 32, 33, 34, 35, 36, 37, 38};

struct SilentClient : IClientPort {
    void updateClientAddr(uint32_t, const std::string&, uint16_t) override {}
    void removeClientAddr(uint32_t) override {}
    void sendHeartbeatAck(const SessionEndpoint&, bool, uint8_t, uint16_t, uint16_t) override {}
    void sendSessionClose(const Connection&, uint8_t) override {}
    void sendRumble(const Connection&, uint8_t, const RumbleReport&) override {}
    void sendLightbar(const Connection&, uint8_t, uint8_t, uint8_t, uint8_t) override {}
    void sendTriggerEffects(const Connection&, uint8_t, const TriggerEffectsReport&) override {}
    void sendPlayerLeds(const Connection&, uint8_t, uint8_t) override {}
    void sendSpeakerAudio(const SessionEndpoint&, uint8_t, uint16_t, const uint8_t*,
                          size_t) override {}
    void sendHapticAudio(const SessionEndpoint&, uint8_t, uint16_t, const uint8_t*,
                         size_t) override {}
    void sendMicLed(const Connection&, uint8_t, uint8_t) override {}
};

struct SilentLog : ILogPort {
    void logMsg(LogLevel, const std::string&, const std::string&) override {}
};

ControllerDescriptor padAt(uint8_t idx, uint8_t type = CONTROLLER_TYPE_XBOX, uint16_t caps = 0,
                           uint8_t touchpadMode = TOUCHPAD_MODE_OFF) {
    ControllerDescriptor d;
    d.ctrlIdx = idx;
    d.type = type;
    d.caps = caps;
    d.touchpadMode = touchpadMode;
    return d;
}

SessionUpsertResult put(SessionService& svc, const std::string& deviceId,
                        const std::vector<ControllerDescriptor>& pads) {
    return svc.upsertSession(deviceId, deviceId, "127.0.0.1", PAIRING_KEY, pads, false);
}

bool waitUntil(const std::atomic<bool>& flag) {
    const auto deadline = Clock::now() + THREAD_TIMEOUT;
    while (!flag.load(std::memory_order_acquire)) {
        if (Clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

double micros(Clock::duration d) { return std::chrono::duration<double, std::micro>(d).count(); }

class ServiceLockProbe {
  public:
    explicit ServiceLockProbe(SessionService& svc)
        : svc_(svc), worker_(&ServiceLockProbe::run, this) {}

    ~ServiceLockProbe() {
        {
            std::lock_guard<std::mutex> lk(m_);
            stopping_ = true;
        }
        cv_.notify_all();
        worker_.join();
    }

    ServiceLockProbe(const ServiceLockProbe&) = delete;
    ServiceLockProbe& operator=(const ServiceLockProbe&) = delete;

    bool serviceLockFree() {
        std::unique_lock<std::mutex> lk(m_);
        const uint64_t ticket = ++requested_;
        cv_.notify_all();
        return cv_.wait_for(lk, PROBE_LIMIT, [&] { return answered_ >= ticket; });
    }

  private:
    void run() {
        std::unique_lock<std::mutex> lk(m_);
        for (;;) {
            cv_.wait(lk, [&] { return stopping_ || answered_ < requested_; });
            if (answered_ == requested_) return;
            const uint64_t ticket = requested_;
            lk.unlock();
            svc_.activeSessionCount();
            lk.lock();
            answered_ = ticket;
            cv_.notify_all();
        }
    }

    SessionService& svc_;
    std::mutex m_;
    std::condition_variable cv_;
    uint64_t requested_ = 0;
    uint64_t answered_ = 0;
    bool stopping_ = false;
    std::thread worker_;
};

enum class Lifecycle { EnsureBusOpen, CloseBus, Plug, PlugPreferring, Unplug, Count };

const char* lifecycleName(Lifecycle call) {
    switch (call) {
    case Lifecycle::EnsureBusOpen:
        return "ensureBusOpen";
    case Lifecycle::CloseBus:
        return "closeBus";
    case Lifecycle::Plug:
        return "pluginDevice";
    case Lifecycle::PlugPreferring:
        return "pluginDevicePreferring";
    case Lifecycle::Unplug:
        return "unplugDevice";
    case Lifecycle::Count:
        break;
    }
    return "?";
}

class LockCheckingBackend : public IGamepadPort {
  public:
    ServiceLockProbe* probe = nullptr;

    bool ensureBusOpen() override {
        check(Lifecycle::EnsureBusOpen);
        busOpen_ = true;
        return true;
    }

    void closeBus() override {
        check(Lifecycle::CloseBus);
        busOpen_ = false;
        plugged_.clear();
    }

    bool isBusOpen() const override { return busOpen_; }

    bool pluginDevice(uint32_t serial, GamepadIdentity) override {
        check(Lifecycle::Plug);
        plugged_.insert(serial);
        return true;
    }

    bool pluginDevicePreferring(uint32_t serial, GamepadIdentity, const std::string&) override {
        check(Lifecycle::PlugPreferring);
        plugged_.insert(serial);
        return true;
    }

    bool unplugDevice(uint32_t serial) override {
        check(Lifecycle::Unplug);
        plugged_.erase(serial);
        return true;
    }

    bool isDevicePlugged(uint32_t serial) const override { return plugged_.count(serial) != 0; }

    bool submitReport(uint32_t, const GamepadReport&) override { return true; }

    void setRumbleCallback(RumbleCallback) override {}

    int calls() const { return total_; }
    int calls(Lifecycle call) const { return byCall_[static_cast<size_t>(call)]; }
    int underLock() const { return underLock_; }
    int underLock(Lifecycle call) const { return underLockByCall_[static_cast<size_t>(call)]; }

  private:
    void check(Lifecycle call) {
        total_++;
        byCall_[static_cast<size_t>(call)]++;
        if (probe == nullptr || probe->serviceLockFree()) return;
        underLock_++;
        underLockByCall_[static_cast<size_t>(call)]++;
    }

    bool busOpen_ = false;
    std::set<uint32_t> plugged_;
    int total_ = 0;
    int underLock_ = 0;
    std::array<int, static_cast<size_t>(Lifecycle::Count)> byCall_{};
    std::array<int, static_cast<size_t>(Lifecycle::Count)> underLockByCall_{};
};

void reportLifecycle(const LockCheckingBackend& backend) {
    std::cout << "  lifecycle calls " << backend.calls() << ", under the service lock "
              << backend.underLock() << " (";
    for (size_t i = 0; i < static_cast<size_t>(Lifecycle::Count); i++) {
        const auto call = static_cast<Lifecycle>(i);
        if (i != 0) std::cout << ", ";
        std::cout << lifecycleName(call) << " " << backend.underLock(call) << "/"
                  << backend.calls(call);
    }
    std::cout << ")\n";
}

void test_lifecycleCallsRunWithTheServiceLockFree() {
    TEST("topology: no backend lifecycle call runs while the service lock is held");
    LockCheckingBackend backend;
    SilentClient client;
    SilentLog log;
    SessionService svc(backend, client, log);
    ServiceLockProbe probe(svc);
    backend.probe = &probe;

    const SessionUpsertResult opened = put(svc, "dish", {padAt(0)});
    EXPECT(opened.ok);
    put(svc, "dish", {padAt(0), padAt(1)});
    put(svc, "dish", {padAt(0, CONTROLLER_TYPE_PLAYSTATION), padAt(1)});
    put(svc, "dish", {padAt(0, CONTROLLER_TYPE_PLAYSTATION)});

    const int beforeKeyOnly = backend.calls();
    const SessionUpsertResult keyOnly = put(svc, "dish", {padAt(0, CONTROLLER_TYPE_PLAYSTATION)});
    EXPECT(keyOnly.ok);
    EXPECT_EQ(backend.calls(), beforeKeyOnly);

    ControllerApplyResult applied;
    uint16_t epoch = 0;
    EXPECT(svc.applyController(opened.connectionId, "dish", padAt(2), applied, epoch));
    EXPECT_EQ(applied.result, APPLY_OK);
    EXPECT(svc.removeController(opened.connectionId, "dish", 2, epoch));

    const SessionUpsertResult kicked = put(svc, "kick", {padAt(0)});
    EXPECT_EQ(svc.closeSessionById(kicked.connectionId, "", CLOSE_REASON_KICKED, true), 1);
    put(svc, "unpair", {padAt(0)});
    EXPECT_EQ(svc.closeSessionsForDevice("unpair", CLOSE_REASON_UNPAIRED), 1);
    const SessionUpsertResult silent = put(svc, "reap", {padAt(0)});
    svc.backdateForTest(silent.token, REAP_SILENCE_SEC, 1);
    EXPECT_EQ(svc.reapTimedOut(), 1);

    const int closesBefore = backend.calls(Lifecycle::CloseBus);
    put(svc, "dish", {});
    EXPECT(!backend.isBusOpen());
    EXPECT_EQ(backend.calls(Lifecycle::CloseBus), closesBefore + 1);

    put(svc, "dish", {padAt(0)});
    std::vector<ControllerDescriptor> full;
    for (uint8_t idx = 0; idx < FULL_BUS_SECOND_SESSION_PADS; idx++) full.push_back(padAt(idx));
    put(svc, "full", full);
    EXPECT_EQ(svc.availableSlots(), 0);
    full[0] = padAt(0, CONTROLLER_TYPE_PLAYSTATION);
    const SessionUpsertResult inPlace = put(svc, "full", full);
    EXPECT_EQ(inPlace.controllers[0].result, APPLY_OK);
    EXPECT_EQ(inPlace.controllers[0].appliedType, CONTROLLER_TYPE_PLAYSTATION);
    EXPECT_EQ(svc.totalActiveControllers(), MAX_BACKEND_CONTROLLERS);

    svc.closeAllSessions(CLOSE_REASON_SHUTDOWN);
    EXPECT(!backend.isBusOpen());
    EXPECT_EQ(svc.totalActiveControllers(), 0);

    EXPECT(backend.calls(Lifecycle::EnsureBusOpen) >= 2);
    EXPECT(backend.calls(Lifecycle::CloseBus) >= 2);
    EXPECT(backend.calls(Lifecycle::PlugPreferring) > MAX_BACKEND_CONTROLLERS);
    EXPECT(backend.calls(Lifecycle::Unplug) > MAX_BACKEND_CONTROLLERS);
    EXPECT_EQ(backend.underLock(), 0);
    reportLifecycle(backend);
}

class SlowPlugBackend : public IGamepadPort {
  public:
    std::atomic<int> plugMs{0};

    bool ensureBusOpen() override {
        busOpen_.store(true);
        return true;
    }

    void closeBus() override {
        for (auto& plugged : plugged_) plugged.store(false);
        busOpen_.store(false);
    }

    bool isBusOpen() const override { return busOpen_.load(); }

    bool pluginDevice(uint32_t serial, GamepadIdentity identity) override {
        return pluginDevicePreferring(serial, identity, std::string());
    }

    bool pluginDevicePreferring(uint32_t serial, GamepadIdentity, const std::string&) override {
        if (!valid(serial)) return false;
        const uint64_t number = plugNumber_.fetch_add(1) + 1;
        plugState_.store((number << 1) | 1u);
        const int ms = plugMs.load();
        if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        plugged_[serial].store(true);
        plugState_.store(number << 1);
        return true;
    }

    bool unplugDevice(uint32_t serial) override {
        if (valid(serial)) plugged_[serial].store(false);
        return true;
    }

    bool isDevicePlugged(uint32_t serial) const override {
        return valid(serial) && plugged_[serial].load();
    }

    bool submitReport(uint32_t serial, const GamepadReport&) override {
        return isDevicePlugged(serial);
    }

    bool submitMotion(uint32_t serial, const MotionReport&) override {
        return isDevicePlugged(serial);
    }

    bool submitTouchpad(uint32_t serial, const TouchpadReport&) override {
        return isDevicePlugged(serial);
    }

    bool submitBattery(uint32_t serial, const BatteryReport&) override {
        return isDevicePlugged(serial);
    }

    bool supportsMotionForType(uint8_t controllerType) const override {
        return controllerTypeHasMotion(controllerType);
    }

    void setRumbleCallback(RumbleCallback) override {}

    uint64_t plugState() const { return plugState_.load(); }

    static bool plugging(uint64_t state) { return (state & 1u) != 0; }

    static bool withinOnePlug(uint64_t before, uint64_t after) {
        return before == after && plugging(before);
    }

    static bool overlapsPlug(uint64_t before, uint64_t after) {
        return plugging(before) || plugging(after) || before != after;
    }

  private:
    static bool valid(uint32_t serial) { return serial >= 1 && serial <= MAX_BACKEND_CONTROLLERS; }

    std::array<std::atomic<bool>, MAX_BACKEND_CONTROLLERS + 1> plugged_{};
    std::atomic<bool> busOpen_{false};
    std::atomic<uint64_t> plugNumber_{0};
    std::atomic<uint64_t> plugState_{0};
};

struct ColdStream {
    std::atomic<bool> stop{false};
    std::atomic<bool> started{false};
    std::atomic<bool> done{false};
    Clock::duration longestOverlappingPlug{};
    uint64_t callsOverlappingPlug = 0;
    uint64_t motionAccepted = 0;
    uint64_t touchpadAccepted = 0;
    uint64_t batteryAccepted = 0;
    uint64_t heartbeats = 0;
};

template <typename Call>
void timeCall(const SlowPlugBackend& backend, ColdStream& stream, Call&& call) {
    const uint64_t before = backend.plugState();
    const auto start = Clock::now();
    call();
    const auto spent = Clock::now() - start;
    if (!SlowPlugBackend::overlapsPlug(before, backend.plugState())) return;
    stream.callsOverlappingPlug++;
    if (spent > stream.longestOverlappingPlug) stream.longestOverlappingPlug = spent;
}

void streamColdDispatch(SessionService& svc, const SlowPlugBackend& backend, uint32_t token,
                        ColdStream& stream) {
    uint32_t counter = 0;
    MotionReport motion{};
    motion.gyroX = 1;
    TouchpadReport touch{};
    touch.finger0.active = true;
    BatteryReport battery{};
    battery.level = BATTERY_HALF;
    battery.status = BATTERY_STATUS_DISCHARGING;
    const auto record = [&] {
        timeCall(backend, stream,
                 [&] { svc.updatePostDecryptV4(token, ++counter, SENDER_IPV4, SENDER_PORT); });
    };
    stream.started.store(true, std::memory_order_release);
    while (!stream.stop.load(std::memory_order_acquire)) {
        record();
        timeCall(backend, stream, [&] {
            if (svc.handleMotionData(token, 0, motion)) stream.motionAccepted++;
        });
        record();
        timeCall(backend, stream, [&] {
            svc.handleHeartbeat(token);
            stream.heartbeats++;
        });
        record();
        timeCall(backend, stream, [&] {
            if (svc.handleTouchpadData(token, 0, touch)) stream.touchpadAccepted++;
        });
        record();
        timeCall(backend, stream, [&] {
            if (svc.handleBatteryUpdate(token, 0, battery)) stream.batteryAccepted++;
        });
    }
    stream.done.store(true, std::memory_order_release);
}

void test_coldDispatchNeverWaitsBehindAnotherSessionsPlug() {
    TEST("topology: motion, heartbeat, touchpad and battery never wait behind another session's "
         "plug");
    SlowPlugBackend backend;
    SilentClient client;
    SilentLog log;
    SessionService svc(backend, client, log);
    const SessionUpsertResult a =
        put(svc, "dishA", {padAt(0, CONTROLLER_TYPE_PLAYSTATION, CAP_MOTION, TOUCHPAD_MODE_DS4)});
    const SessionUpsertResult b = put(svc, "dishB", {padAt(0)});
    EXPECT(a.ok);
    EXPECT(b.ok);
    backend.plugMs.store(SLOW_PLUG_MS);

    ColdStream stream;
    std::thread receiver(streamColdDispatch, std::ref(svc), std::cref(backend), a.token,
                         std::ref(stream));
    const bool started = waitUntil(stream.started);
    for (int round = 0; started && round < SLOW_ROUNDS; round++) {
        const SessionUpsertResult grown = put(svc, "dishB", {padAt(0), padAt(1)});
        EXPECT_EQ(grown.controllers[1].result, APPLY_OK);
        put(svc, "dishB", {padAt(0)});
    }
    stream.stop.store(true, std::memory_order_release);
    if (!waitUntil(stream.done)) {
        std::cerr << "  FAIL [" << g_currentTest << "] the receive thread never finished\n";
        std::exit(1);
    }
    receiver.join();

    EXPECT(started);
    EXPECT(stream.callsOverlappingPlug > 0);
    EXPECT(stream.longestOverlappingPlug < BLOCKED_CALL_LIMIT);
    EXPECT(stream.motionAccepted > 0);
    EXPECT(stream.touchpadAccepted > 0);
    EXPECT(stream.batteryAccepted > 0);
    EXPECT(stream.heartbeats > 0);
    std::cout << "  cold dispatch: longest call overlapping dish B's plugs "
              << micros(stream.longestOverlappingPlug) << " us over " << stream.callsOverlappingPlug
              << " calls (" << SLOW_ROUNDS << " plugs of " << SLOW_PLUG_MS << " ms)\n";
}

struct HotStream {
    std::atomic<uint32_t> token{0};
    std::atomic<uint32_t> nextToken{0};
    std::atomic<uint64_t> framesOnNextToken{0};
    std::atomic<bool> stop{false};
    std::atomic<bool> started{false};
    std::atomic<bool> done{false};
    uint64_t framesInPlug = 0;
    uint64_t failuresInPlug = 0;
    uint64_t failuresOnNextToken = 0;
};

void streamPadZero(SessionService& svc, const SlowPlugBackend& backend, HotStream& stream) {
    uint32_t counter = 0;
    GamepadReport report{};
    stream.started.store(true, std::memory_order_release);
    while (!stream.stop.load(std::memory_order_acquire)) {
        const uint32_t token = stream.token.load();
        const uint32_t expected = stream.nextToken.load();
        const uint64_t before = backend.plugState();
        uint8_t key[CRYPTO_KEY_SIZE];
        uint32_t lastCounter = 0;
        const bool ok =
            svc.getDecryptInfo(token, key, lastCounter) &&
            svc.handleGamepadDataAndUpdate(token, ++counter, SENDER_IPV4, SENDER_PORT, 0, report);
        std::atomic_thread_fence(std::memory_order_acquire);
        const uint64_t after = backend.plugState();
        const bool onNextToken = token == expected && stream.nextToken.load() == expected;
        if (onNextToken) {
            if (!ok) stream.failuresOnNextToken++;
            stream.framesOnNextToken.fetch_add(1);
        }
        if (!SlowPlugBackend::withinOnePlug(before, after)) continue;
        stream.framesInPlug++;
        if (!ok) stream.failuresInPlug++;
    }
    stream.done.store(true, std::memory_order_release);
}

bool waitForFramesOnNextToken(const HotStream& stream) {
    const auto deadline = Clock::now() + THREAD_TIMEOUT;
    while (stream.framesOnNextToken.load() < FRAMES_ON_NEW_TOKEN) {
        if (Clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

SessionUpsertResult putThenSwitch(SessionService& svc, HotStream& stream,
                                  const std::vector<ControllerDescriptor>& pads, bool& switched) {
    stream.nextToken.store(0);
    const SessionUpsertResult result = put(svc, "dishA", pads);
    stream.framesOnNextToken.store(0);
    stream.nextToken.store(result.token);
    stream.token.store(result.token);
    switched = waitForFramesOnNextToken(stream) && switched;
    return result;
}

void stopHotStream(HotStream& stream, std::thread& receiver) {
    stream.stop.store(true, std::memory_order_release);
    if (!waitUntil(stream.done)) {
        std::cerr << "  FAIL [" << g_currentTest << "] the receive thread never finished\n";
        std::exit(1);
    }
    receiver.join();
}

void test_padKeepsSubmittingThroughItsSessionsControllerPlug() {
    TEST("topology: a pad keeps submitting while applyController plugs a sibling in its session");
    SlowPlugBackend backend;
    SilentClient client;
    SilentLog log;
    SessionService svc(backend, client, log);
    const SessionUpsertResult a = put(svc, "dishA", {padAt(0)});
    EXPECT(a.ok);
    backend.plugMs.store(SLOW_PLUG_MS);

    HotStream stream;
    stream.token.store(a.token);
    std::thread receiver(streamPadZero, std::ref(svc), std::cref(backend), std::ref(stream));
    const bool started = waitUntil(stream.started);
    for (int round = 0; started && round < SLOW_ROUNDS; round++) {
        ControllerApplyResult applied;
        uint16_t epoch = 0;
        EXPECT(svc.applyController(a.connectionId, "dishA", padAt(1), applied, epoch));
        EXPECT_EQ(applied.result, APPLY_OK);
        EXPECT(svc.removeController(a.connectionId, "dishA", 1, epoch));
    }
    stopHotStream(stream, receiver);

    EXPECT(started);
    EXPECT(stream.framesInPlug > 0);
    EXPECT_EQ(stream.failuresInPlug, 0u);
    std::cout << "  applyController: pad 0 frames inside " << SLOW_ROUNDS << " plugs of "
              << SLOW_PLUG_MS << " ms " << stream.framesInPlug << ", refused "
              << stream.failuresInPlug << "\n";
}

void test_putKeepsTheOldTokenThroughItsPlugThenTheNewOne() {
    TEST("topology: a PUT keeps the old token working through its plug, then the new one");
    SlowPlugBackend backend;
    SilentClient client;
    SilentLog log;
    SessionService svc(backend, client, log);
    const SessionUpsertResult a = put(svc, "dishA", {padAt(0)});
    EXPECT(a.ok);
    backend.plugMs.store(SLOW_PLUG_MS);

    HotStream stream;
    stream.token.store(a.token);
    std::thread receiver(streamPadZero, std::ref(svc), std::cref(backend), std::ref(stream));
    const bool started = waitUntil(stream.started);
    bool switched = started;
    uint32_t previous = a.token;
    for (int round = 0; switched && round < SLOW_ROUNDS; round++) {
        const SessionUpsertResult grown =
            putThenSwitch(svc, stream, {padAt(0), padAt(1)}, switched);
        EXPECT_EQ(grown.controllers[1].result, APPLY_OK);
        EXPECT(grown.token != previous);
        const SessionUpsertResult shrunk = putThenSwitch(svc, stream, {padAt(0)}, switched);
        EXPECT(shrunk.token != grown.token);
        previous = shrunk.token;
    }
    stopHotStream(stream, receiver);

    EXPECT(started);
    EXPECT(switched);
    EXPECT(stream.framesInPlug > 0);
    EXPECT_EQ(stream.failuresInPlug, 0u);
    EXPECT_EQ(stream.failuresOnNextToken, 0u);
    std::cout << "  PUT: old-token frames inside " << SLOW_ROUNDS << " plugs of " << SLOW_PLUG_MS
              << " ms " << stream.framesInPlug << ", refused " << stream.failuresInPlug
              << "; new-token frames refused after each PUT returned " << stream.failuresOnNextToken
              << "\n";
}

} // namespace

int main() {
    test_lifecycleCallsRunWithTheServiceLockFree();
    test_coldDispatchNeverWaitsBehindAnotherSessionsPlug();
    test_padKeepsSubmittingThroughItsSessionsControllerPlug();
    test_putKeepsTheOldTokenThroughItsPlugThenTheNewOne();

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
