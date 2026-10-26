// SPDX-License-Identifier: LGPL-3.0-or-later
#include "core/byte_order.h"
#include "core/ipv4_util.h"
#include "core/session_service.h"
#include "net/inner_dispatch.h"
#include "net/session_crypto.h"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifndef BENCH_BUILD_TYPE
#define BENCH_BUILD_TYPE "unknown"
#endif

namespace {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Nanos = std::chrono::nanoseconds;

constexpr int64_t NANOS_PER_MICRO = 1'000;
constexpr int64_t NANOS_PER_MILLI = 1'000'000;
constexpr int64_t NANOS_PER_SECOND = 1'000'000'000;

constexpr size_t HEADER_BYTES = static_cast<size_t>(HEADER_SIZE);
constexpr size_t INNER_HEADER_BYTES = static_cast<size_t>(INNER_HEADER_SIZE);
constexpr size_t TAG_BYTES = static_cast<size_t>(AUTH_TAG_SIZE);
constexpr size_t REPORT_BYTES = sizeof(GamepadReport);
constexpr size_t INNER_BYTES = INNER_HEADER_BYTES + 1 + REPORT_BYTES;
constexpr size_t DATAGRAM_BYTES = HEADER_BYTES + INNER_BYTES + TAG_BYTES;
constexpr size_t RECEIVE_BUFFER_BYTES = static_cast<size_t>(UDP_DATAGRAM_MAX_BYTES);
constexpr uint16_t GAMEPAD_DATA_MIN_LEN = 13;
static_assert(GAMEPAD_DATA_MIN_LEN == 1 + REPORT_BYTES, "ctrlIdx plus one whole report");
static_assert(DATAGRAM_BYTES <= RECEIVE_BUFFER_BYTES, "one datagram fits the receive buffer");

constexpr size_t RING_CAPACITY = 1600;
constexpr size_t CACHE_LINE_BYTES = 64;

constexpr double DEFAULT_SECS = 3.0;
constexpr double MIN_SECS = 0.05;
constexpr double SMOKE_SECS = 0.2;
constexpr int DEFAULT_REPS = 3;
constexpr int DEFAULT_RATE_A = 8000;
constexpr int DEFAULT_RATE_B = 75;
constexpr int DEFAULT_SUBMIT_US = 15;
constexpr int DEFAULT_PLUG_MS = 200;
constexpr int UNPLUG_MS = 5;
constexpr int SCENARIO_ONE_RATE_HZ = 1000;
constexpr int MAX_RATE_HZ = 100000;
constexpr int MAX_REPS = 99;
constexpr int MAX_SUBMIT_US = 1'000'000;
constexpr int MAX_PLUG_MS = 60'000;

constexpr double PUT_AT_SECS = 1.0;
constexpr double SSE_PERIOD_SECS = 1.0;
constexpr double MAINTENANCE_PERIOD_SECS = 0.1;
constexpr uint64_t REAP_EVERY_TICKS = 10;
constexpr int64_t START_DELAY_MS = 30;

constexpr int NO_CPU = -1;
constexpr int FIRST_PREFERRED_CPU = 2;
constexpr int MAX_CPUS = static_cast<int>(sizeof(DWORD_PTR) * 8);

constexpr uint64_t BASIS_POINTS = 10'000;
constexpr uint64_t P50_BASIS_POINTS = 5'000;
constexpr uint64_t P99_BASIS_POINTS = 9'900;
constexpr uint64_t P999_BASIS_POINTS = 9'990;

constexpr uint16_t FIRST_CLIENT_PORT = 50000;
constexpr uint8_t PAIRING_KEY_BASE = 0x20;
constexpr uint8_t PAIRING_KEY_STRIDE = 0x10;

Nanos secondsToNanos(double seconds) {
    return Nanos(static_cast<int64_t>(seconds * static_cast<double>(NANOS_PER_SECOND)));
}

double nanosToMicros(int64_t nanos) {
    return static_cast<double>(nanos) / static_cast<double>(NANOS_PER_MICRO);
}

double nanosToMillis(int64_t nanos) {
    return static_cast<double>(nanos) / static_cast<double>(NANOS_PER_MILLI);
}

int64_t elapsedNanos(TimePoint from, TimePoint to) {
    return std::chrono::duration_cast<Nanos>(to - from).count();
}

void spinUntil(TimePoint due) {
    while (Clock::now() < due) YieldProcessor();
}

void spinFor(Nanos span) { spinUntil(Clock::now() + span); }

std::string trimmed(const std::string& text) {
    const size_t first = text.find_first_not_of(' ');
    if (first == std::string::npos) return std::string();
    const size_t last = text.find_last_not_of(' ');
    return text.substr(first, last - first + 1);
}

std::string cpuName() {
    std::array<char, 256> name{};
    DWORD size = static_cast<DWORD>(name.size());
    const LSTATUS status =
        RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                     "ProcessorNameString", RRF_RT_REG_SZ, nullptr, name.data(), &size);
    return status == ERROR_SUCCESS ? trimmed(name.data()) : std::string("unknown");
}

bool cpuAllowed(DWORD_PTR mask, int cpu) {
    const bool inRange = cpu >= 0 && cpu < MAX_CPUS;
    return inRange && ((mask >> cpu) & 1) != 0;
}

int chooseReceiverCpu(int requested) {
    DWORD_PTR processMask = 0;
    DWORD_PTR systemMask = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask)) return NO_CPU;
    if (requested != NO_CPU) return cpuAllowed(processMask, requested) ? requested : NO_CPU;
    for (int cpu = FIRST_PREFERRED_CPU; cpu < MAX_CPUS; ++cpu) {
        if (cpuAllowed(processMask, cpu)) return cpu;
    }
    for (int cpu = 1; cpu < FIRST_PREFERRED_CPU; ++cpu) {
        if (cpuAllowed(processMask, cpu)) return cpu;
    }
    return NO_CPU;
}

struct Options {
    double secs = DEFAULT_SECS;
    int reps = DEFAULT_REPS;
    int rateA = DEFAULT_RATE_A;
    int rateB = DEFAULT_RATE_B;
    int submitUs = DEFAULT_SUBMIT_US;
    int plugMs = DEFAULT_PLUG_MS;
    int rxCpu = NO_CPU;
    std::string csvPath;
    bool smoke = false;
    bool verbose = false;
    bool help = false;
};

struct ParsedOptions {
    Options options;
    std::string error;
};

bool parseDouble(const char* text, double& out) {
    char* end = nullptr;
    const double value = std::strtod(text, &end);
    const bool whole = end != text && *end == '\0';
    if (!whole) return false;
    out = value;
    return true;
}

bool parseIntInRange(const char* text, int lowest, int highest, int& out) {
    char* end = nullptr;
    const long long value = std::strtoll(text, &end, 10);
    const bool whole = end != text && *end == '\0';
    const bool inRange = value >= lowest && value <= highest;
    if (!whole || !inRange) return false;
    out = static_cast<int>(value);
    return true;
}

bool applyValuedOption(Options& options, const std::string& name, const char* value) {
    if (name == "--secs") return parseDouble(value, options.secs) && options.secs >= MIN_SECS;
    if (name == "--reps") return parseIntInRange(value, 1, MAX_REPS, options.reps);
    if (name == "--rate-a") return parseIntInRange(value, 1, MAX_RATE_HZ, options.rateA);
    if (name == "--rate-b") return parseIntInRange(value, 1, MAX_RATE_HZ, options.rateB);
    if (name == "--submit-us") return parseIntInRange(value, 0, MAX_SUBMIT_US, options.submitUs);
    if (name == "--plug-ms") return parseIntInRange(value, 0, MAX_PLUG_MS, options.plugMs);
    if (name == "--rx-cpu") return parseIntInRange(value, 0, MAX_CPUS - 1, options.rxCpu);
    if (name == "--csv") {
        options.csvPath = value;
        return !options.csvPath.empty();
    }
    return false;
}

bool applyFlag(Options& options, const std::string& name) {
    if (name == "--smoke")
        options.smoke = true;
    else if (name == "--verbose")
        options.verbose = true;
    else if (name == "--help" || name == "-h")
        options.help = true;
    else
        return false;
    return true;
}

ParsedOptions parseOptions(int argc, char** argv) {
    ParsedOptions parsed;
    for (int i = 1; i < argc; ++i) {
        const std::string name = argv[i];
        if (applyFlag(parsed.options, name)) continue;
        const bool hasValue = i + 1 < argc;
        if (!hasValue) {
            parsed.error = "unknown option or missing value: " + name;
            return parsed;
        }
        const char* value = argv[++i];
        if (!applyValuedOption(parsed.options, name, value)) {
            parsed.error = "bad option: " + name + " " + value;
            return parsed;
        }
    }
    return parsed;
}

void printUsage(std::FILE* out) {
    std::fputs("usage: bench_frame_path [options]\n"
               "  --secs <s>        seconds per run (default 3)\n"
               "  --reps <n>        interleaved repetitions, the median rep is reported "
               "(default 3)\n"
               "  --rate-a <hz>     dish A rate in S2-S4 (default 8000)\n"
               "  --rate-b <hz>     dish B rate (default 75)\n"
               "  --submit-us <us>  spin time of the mock submitReport (default 15)\n"
               "  --plug-ms <ms>    sleep time of the mock pluginDevice (default 200)\n"
               "  --rx-cpu <n>      CPU of the receiver thread (default: the first CPU from 2 "
               "up)\n"
               "  --csv <path>      also write the table as CSV\n"
               "  --smoke           every scenario once for 0.2 s, no warm-up pass\n"
               "  --verbose         print the service's log lines to stderr\n",
               out);
}

class BenchGamepad : public IGamepadPort {
  public:
    BenchGamepad(int submitUs, int plugMs)
        : submitSpan_(std::chrono::microseconds(submitUs)),
          plugSpan_(std::chrono::milliseconds(plugMs)),
          unplugSpan_(std::chrono::milliseconds(UNPLUG_MS)) {}

    void armPlugDelays() { delaysArmed_.store(true, std::memory_order_release); }

    uint64_t submitCalls() const { return submitCalls_.load(std::memory_order_relaxed); }

    bool ensureBusOpen() override {
        busOpen_.store(true, std::memory_order_release);
        return true;
    }

    void closeBus() override { busOpen_.store(false, std::memory_order_release); }

    bool isBusOpen() const override { return busOpen_.load(std::memory_order_acquire); }

    bool pluginDevice(uint32_t, GamepadIdentity) override {
        sleepIfArmed(plugSpan_);
        return true;
    }

    bool unplugDevice(uint32_t) override {
        sleepIfArmed(unplugSpan_);
        return true;
    }

    bool submitReport(uint32_t, const GamepadReport&) override {
        submitCalls_.fetch_add(1, std::memory_order_relaxed);
        spinFor(submitSpan_);
        return true;
    }

    void setRumbleCallback(RumbleCallback) override {}

  private:
    void sleepIfArmed(Nanos span) const {
        if (delaysArmed_.load(std::memory_order_acquire)) std::this_thread::sleep_for(span);
    }

    const Nanos submitSpan_;
    const Nanos plugSpan_;
    const Nanos unplugSpan_;
    std::atomic<bool> delaysArmed_{false};
    std::atomic<bool> busOpen_{false};
    std::atomic<uint64_t> submitCalls_{0};
};

class BenchClient : public IClientPort {
  public:
    void updateClientAddr(uint32_t, const std::string&, uint16_t) override {}
    void removeClientAddr(uint32_t) override {}
    void sendHeartbeatAck(const Connection&, bool, uint8_t, uint16_t, uint16_t) override {}
    void sendSessionClose(const Connection&, uint8_t) override {}
    void sendRumble(const Connection&, uint8_t, const RumbleReport&) override {}
    void sendLightbar(const Connection&, uint8_t, uint8_t, uint8_t, uint8_t) override {}
    void sendTriggerEffects(const Connection&, uint8_t, const TriggerEffectsReport&) override {}
    void sendPlayerLeds(const Connection&, uint8_t, uint8_t) override {}
    void sendSpeakerAudio(const Connection&, uint8_t, uint16_t, const uint8_t*, size_t) override {}
    void sendHapticAudio(const Connection&, uint8_t, uint16_t, const uint8_t*, size_t) override {}
    void sendMicLed(const Connection&, uint8_t, uint8_t) override {}
};

class BenchLog : public ILogPort {
  public:
    explicit BenchLog(bool verbose) : verbose_(verbose) {}

    void logMsg(LogLevel, const std::string& source, const std::string& message) override {
        if (!verbose_) return;
        std::fprintf(stderr, "[%s] %s\n", source.c_str(), message.c_str());
    }

  private:
    const bool verbose_;
};

struct Frame {
    TimePoint stamp;
    size_t length = 0;
    std::array<uint8_t, DATAGRAM_BYTES> bytes{};
};

struct PaddedCounter {
    std::atomic<uint64_t> value{0};
    std::array<uint8_t, CACHE_LINE_BYTES - sizeof(std::atomic<uint64_t>)> padding{};
};

class SpscRing {
  public:
    explicit SpscRing(size_t capacity) : slots_(capacity) {}

    bool tryPush(const Frame& frame) {
        const uint64_t head = head_.value.load(std::memory_order_relaxed);
        const uint64_t tail = tail_.value.load(std::memory_order_acquire);
        const bool full = head - tail >= slots_.size();
        if (full) return false;
        slots_[head % slots_.size()] = frame;
        head_.value.store(head + 1, std::memory_order_release);
        return true;
    }

    const Frame* front() const {
        const uint64_t tail = tail_.value.load(std::memory_order_relaxed);
        const uint64_t head = head_.value.load(std::memory_order_acquire);
        const bool empty = head == tail;
        return empty ? nullptr : &slots_[tail % slots_.size()];
    }

    void pop() {
        const uint64_t tail = tail_.value.load(std::memory_order_relaxed);
        tail_.value.store(tail + 1, std::memory_order_release);
    }

  private:
    std::vector<Frame> slots_;
    PaddedCounter head_;
    PaddedCounter tail_;
};

struct Credentials {
    uint32_t token = 0;
    std::array<uint8_t, CRYPTO_KEY_SIZE> key{};
};

struct DishSpec {
    std::string name;
    std::string deviceId;
    std::string deviceName;
    std::string clientIp;
    uint16_t clientPort = 0;
    int rateHz = 0;
    std::array<uint8_t, CRYPTO_KEY_SIZE> pairingKey{};
};

struct DishTally {
    std::vector<uint32_t> latencyNs;
    uint64_t completed = 0;
    uint64_t overOneMs = 0;
    int64_t longestGapNs = 0;
    TimePoint lastDone;
    uint64_t refused = 0;
    uint64_t rejected = 0;
    uint64_t replayed = 0;
    uint64_t undecryptable = 0;
    uint64_t malformed = 0;
};

struct DishRun {
    DishRun(DishSpec dishSpec, uint64_t frames)
        : spec(std::move(dishSpec)), senderIPv4(satellite::parseIPv4Nbo(spec.clientIp)),
          plannedFrames(frames), ring(RING_CAPACITY) {
        tally.latencyNs.assign(frames, 0);
    }

    const DishSpec spec;
    const uint32_t senderIPv4;
    const uint64_t plannedFrames;
    SpscRing ring;
    std::atomic<const Credentials*> credentials{nullptr};
    std::vector<std::unique_ptr<Credentials>> issued;
    uint64_t produced = 0;
    uint64_t ringDrops = 0;
    uint64_t sealFailures = 0;
    DishTally tally;
};

DishSpec makeDishSpec(const char* name, int index, int rateHz) {
    DishSpec spec;
    spec.name = name;
    spec.deviceId = std::string("bench-dish-") + name;
    spec.deviceName = std::string("Bench dish ") + name;
    spec.clientIp = "192.168.1." + std::to_string(100 + index);
    spec.clientPort = static_cast<uint16_t>(FIRST_CLIENT_PORT + index);
    spec.rateHz = rateHz;
    for (size_t i = 0; i < spec.pairingKey.size(); ++i) {
        const int offset = PAIRING_KEY_BASE + index * PAIRING_KEY_STRIDE + static_cast<int>(i);
        spec.pairingKey[i] = static_cast<uint8_t>(offset);
    }
    return spec;
}

std::unique_ptr<DishRun> makeDish(const char* name, int index, int rateHz, double secs) {
    const uint64_t planned = std::max<uint64_t>(1, static_cast<uint64_t>(secs * rateHz));
    return std::make_unique<DishRun>(makeDishSpec(name, index, rateHz), planned);
}

ControllerDescriptor makeDescriptor(uint8_t ctrlIdx) {
    ControllerDescriptor descriptor;
    descriptor.ctrlIdx = ctrlIdx;
    descriptor.type = CONTROLLER_TYPE_XBOX;
    descriptor.caps = 0;
    descriptor.touchpadMode = TOUCHPAD_MODE_OFF;
    return descriptor;
}

bool allApplied(const SessionUpsertResult& result) {
    return std::all_of(
        result.controllers.begin(), result.controllers.end(),
        [](const ControllerApplyResult& applied) { return applied.result == APPLY_OK; });
}

SessionUpsertResult upsertDish(SessionService& service, const DishRun& dish,
                               const std::vector<ControllerDescriptor>& descriptors) {
    const DishSpec& spec = dish.spec;
    return service.upsertSession(spec.deviceId, spec.deviceName, spec.clientIp,
                                 spec.pairingKey.data(), descriptors, false);
}

void publishSession(DishRun& dish, const SessionUpsertResult& result) {
    auto credentials = std::make_unique<Credentials>();
    credentials->token = result.token;
    deriveSessionKey(dish.spec.pairingKey.data(), result.sessionSalt, result.token,
                     credentials->key.data());
    const Credentials* published = credentials.get();
    dish.issued.push_back(std::move(credentials));
    dish.credentials.store(published, std::memory_order_release);
}

bool sealFrame(const Credentials& credentials, uint32_t counter, Frame& frame) {
    std::array<uint8_t, INNER_BYTES> inner{};
    satellite::writeBE16(inner.data(), MSG_GAMEPAD_DATA);
    satellite::writeBE16(inner.data() + 2, static_cast<uint16_t>(1 + REPORT_BYTES));
    inner[INNER_HEADER_BYTES] = 0;
    GamepadReport report;
    report.wButtons = static_cast<uint16_t>(counter);
    report.sThumbRX = static_cast<int16_t>(counter);
    std::memcpy(inner.data() + INNER_HEADER_BYTES + 1, &report, REPORT_BYTES);

    satellite::writeBE32(frame.bytes.data(), credentials.token);
    satellite::writeBE32(frame.bytes.data() + 4, counter);
    unsigned long long cipherLen = 0;
    const bool sealed = encryptPacket(credentials.key.data(), CRYPTO_DIR_CLIENT_TO_SERVER, counter,
                                      credentials.token, inner.data(), inner.size(),
                                      frame.bytes.data() + HEADER_BYTES, &cipherLen);
    frame.length = HEADER_BYTES + static_cast<size_t>(cipherLen);
    return sealed;
}

void runProducer(DishRun& dish, TimePoint start) {
    uint32_t counter = 0;
    const Credentials* credentials = dish.credentials.load(std::memory_order_acquire);
    for (uint64_t tick = 0; tick < dish.plannedFrames; ++tick) {
        const int64_t offset = static_cast<int64_t>(tick) * NANOS_PER_SECOND / dish.spec.rateHz;
        spinUntil(start + Nanos(offset));
        const Credentials* latest = dish.credentials.load(std::memory_order_acquire);
        if (latest != credentials) {
            credentials = latest;
            counter = 0;
        }
        counter += 1;
        Frame frame;
        if (!sealFrame(*credentials, counter, frame)) {
            dish.sealFailures += 1;
            continue;
        }
        frame.stamp = Clock::now();
        if (!dish.ring.tryPush(frame)) dish.ringDrops += 1;
        dish.produced += 1;
    }
}

enum class Verdict { Completed, Rejected, UnknownToken, Replayed, Undecryptable, Malformed };

struct Handled {
    Verdict verdict;
    TimePoint done;
};

Handled discarded(Verdict verdict) { return Handled{verdict, TimePoint()}; }

Handled handleDatagram(SessionService& service, uint8_t* buf, int n, uint32_t senderIPv4,
                       uint16_t senderPort) {
    const uint32_t token = satellite::readBE32(buf);
    const uint32_t counter = satellite::readBE32(buf + 4);

    uint8_t key[CRYPTO_KEY_SIZE];
    uint32_t lastCounter = 0;
    bool seenCounter = false;
    if (!service.getDecryptInfo(token, key, lastCounter, &seenCounter)) {
        return discarded(Verdict::UnknownToken);
    }

    if (seenCounter && counter <= lastCounter) return discarded(Verdict::Replayed);

    uint8_t* plaintext = buf + HEADER_SIZE;
    const auto ctLen = static_cast<size_t>(n - HEADER_SIZE);
    unsigned long long ptLen = 0;
    if (!decryptPacket(key, CRYPTO_DIR_CLIENT_TO_SERVER, counter, token, plaintext, ctLen,
                       plaintext, &ptLen)) {
        return discarded(Verdict::Undecryptable);
    }

    InnerHeader inner;
    if (!parseInnerHeader(plaintext, static_cast<size_t>(ptLen), inner)) {
        return discarded(Verdict::Malformed);
    }
    const uint8_t* payload = plaintext + INNER_HEADER_SIZE;

    const bool gamepadData =
        inner.msgType == MSG_GAMEPAD_DATA && inner.msgLen >= GAMEPAD_DATA_MIN_LEN;
    if (!gamepadData) return discarded(Verdict::Malformed);

    const uint8_t ctrlIdx = payload[0];
    GamepadReport report;
    std::memcpy(&report, payload + 1, sizeof(GamepadReport));
    const bool accepted =
        service.handleGamepadDataAndUpdate(token, counter, senderIPv4, senderPort, ctrlIdx, report);
    return Handled{accepted ? Verdict::Completed : Verdict::Rejected, Clock::now()};
}

void recordCompleted(DishTally& tally, TimePoint done, TimePoint stamp) {
    const int64_t latency = std::max<int64_t>(0, elapsedNanos(stamp, done));
    if (tally.completed < tally.latencyNs.size()) {
        const int64_t clamped = std::min<int64_t>(latency, UINT32_MAX);
        tally.latencyNs[tally.completed] = static_cast<uint32_t>(clamped);
    }
    if (tally.completed > 0) {
        const int64_t gap = elapsedNanos(tally.lastDone, done);
        tally.longestGapNs = std::max(tally.longestGapNs, gap);
    }
    if (latency > NANOS_PER_MILLI) tally.overOneMs += 1;
    tally.lastDone = done;
    tally.completed += 1;
}

void recordHandled(DishTally& tally, const Handled& handled, TimePoint stamp) {
    switch (handled.verdict) {
    case Verdict::Completed:
        recordCompleted(tally, handled.done, stamp);
        return;
    case Verdict::Rejected:
        tally.rejected += 1;
        return;
    case Verdict::UnknownToken:
        tally.refused += 1;
        return;
    case Verdict::Replayed:
        tally.replayed += 1;
        return;
    case Verdict::Undecryptable:
        tally.undecryptable += 1;
        return;
    case Verdict::Malformed:
        tally.malformed += 1;
        return;
    }
}

struct ReceiverContext {
    ReceiverContext(SessionService& svc, std::vector<DishRun*> served, int pinTo)
        : service(svc), dishes(std::move(served)), cpu(pinTo) {}

    SessionService& service;
    const std::vector<DishRun*> dishes;
    const int cpu;
    std::atomic<bool> ready{false};
    std::atomic<bool> producersDone{false};
    bool pinned = false;
    bool raised = false;
};

void configureReceiverThread(ReceiverContext& rx) {
    if (rx.cpu == NO_CPU) return;
    const DWORD_PTR mask = static_cast<DWORD_PTR>(1) << rx.cpu;
    rx.pinned = SetThreadAffinityMask(GetCurrentThread(), mask) != 0;
    rx.raised = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL) != 0;
}

DishRun* oldestPending(const std::vector<DishRun*>& dishes) {
    DishRun* oldest = nullptr;
    TimePoint oldestStamp = TimePoint::max();
    for (DishRun* dish : dishes) {
        const Frame* front = dish->ring.front();
        if (front == nullptr || front->stamp >= oldestStamp) continue;
        oldest = dish;
        oldestStamp = front->stamp;
    }
    return oldest;
}

void serveOldest(SessionService& service, DishRun& dish, uint8_t* buf) {
    const Frame& front = *dish.ring.front();
    const TimePoint stamp = front.stamp;
    const int length = static_cast<int>(front.length);
    std::memcpy(buf, front.bytes.data(), front.length);
    dish.ring.pop();
    const Handled handled =
        handleDatagram(service, buf, length, dish.senderIPv4, dish.spec.clientPort);
    recordHandled(dish.tally, handled, stamp);
}

void runReceiver(ReceiverContext& rx) {
    configureReceiverThread(rx);
    rx.ready.store(true, std::memory_order_release);
    std::array<uint8_t, RECEIVE_BUFFER_BYTES> buf{};
    for (;;) {
        DishRun* const next = oldestPending(rx.dishes);
        if (next != nullptr) {
            serveOldest(rx.service, *next, buf.data());
            continue;
        }
        const bool finished = rx.producersDone.load(std::memory_order_acquire);
        if (finished && oldestPending(rx.dishes) == nullptr) return;
        YieldProcessor();
    }
}

struct TickSchedule {
    TimePoint first;
    Nanos period;
    TimePoint end;
};

bool waitForTick(const TickSchedule& schedule, uint64_t index) {
    const int64_t offset = schedule.period.count() * static_cast<int64_t>(index);
    const TimePoint due = schedule.first + Nanos(offset);
    if (due >= schedule.end) return false;
    std::this_thread::sleep_until(due);
    return true;
}

struct SseWorker {
    SseWorker(SessionService& svc, std::vector<std::string> ids, TickSchedule plan)
        : service(svc), deviceIds(std::move(ids)), schedule(plan) {}

    SessionService& service;
    const std::vector<std::string> deviceIds;
    const TickSchedule schedule;
    uint64_t ticks = 0;
    uint64_t connectionsSeen = 0;
    uint64_t activeLinks = 0;
    uint64_t sessionsSeen = 0;
};

void runSse(SseWorker& worker) {
    for (uint64_t index = 0; waitForTick(worker.schedule, index); ++index) {
        const SessionService::ConnectionsSnapshot snapshot =
            worker.service.getConnectionsSnapshot();
        worker.connectionsSeen += snapshot.connections.size();
        for (const std::string& deviceId : worker.deviceIds) {
            const DeviceLinkState state = worker.service.linkStateForDevice(deviceId);
            if (state == DeviceLinkState::Active) worker.activeLinks += 1;
        }
        worker.sessionsSeen += static_cast<uint64_t>(worker.service.activeSessionCount());
        worker.ticks += 1;
    }
}

struct MaintenanceWorker {
    MaintenanceWorker(SessionService& svc, TickSchedule plan) : service(svc), schedule(plan) {}

    SessionService& service;
    const TickSchedule schedule;
    uint64_t ticks = 0;
    uint64_t rumblePackets = 0;
    uint64_t reaped = 0;
};

void runMaintenance(MaintenanceWorker& worker) {
    for (uint64_t index = 0; waitForTick(worker.schedule, index); ++index) {
        worker.rumblePackets += static_cast<uint64_t>(worker.service.refreshRumble());
        worker.service.refreshIdleInput();
        worker.ticks += 1;
        if (worker.ticks % REAP_EVERY_TICKS == 0) {
            worker.reaped += static_cast<uint64_t>(worker.service.reapTimedOut());
        }
    }
}

struct PutWorker {
    PutWorker(SessionService& svc, DishRun& target, TimePoint when)
        : service(svc), dish(target), due(when) {}

    SessionService& service;
    DishRun& dish;
    const TimePoint due;
    bool fired = false;
    bool applied = false;
    double durationMs = 0.0;
};

void runPut(PutWorker& worker) {
    std::this_thread::sleep_until(worker.due);
    const std::vector<ControllerDescriptor> descriptors{makeDescriptor(0), makeDescriptor(1)};
    const TimePoint began = Clock::now();
    const SessionUpsertResult result = upsertDish(worker.service, worker.dish, descriptors);
    const TimePoint finished = Clock::now();
    worker.fired = true;
    worker.durationMs = nanosToMillis(elapsedNanos(began, finished));
    worker.applied = result.ok && allApplied(result);
    if (worker.applied) publishSession(worker.dish, result);
}

struct Scenario {
    const char* id;
    const char* summary;
    bool withDishB;
    int dishARateHz;
    bool withColdReaders;
    bool withPut;
};

constexpr std::array<Scenario, 4> SCENARIOS{{
    {"S1", "dish A alone at 1000 Hz, no cold threads", false, SCENARIO_ONE_RATE_HZ, false, false},
    {"S2", "dish A (--rate-a) plus dish B (--rate-b), no cold threads", true, 0, false, false},
    {"S3", "S2 plus the sse thread (1 Hz) and the maintenance thread (10 Hz, reap every 10th tick)",
     true, 0, true, false},
    {"S4", "S3 plus a PUT for dish B at 1.0 s that plugs a second controller under the lock", true,
     0, true, true},
}};

struct RunPlan {
    double secs;
    double scheduleScale;
    int rxCpu;
};

struct DishRow {
    std::string scenario;
    std::string dish;
    int rateHz = 0;
    uint64_t frames = 0;
    double p50Us = 0.0;
    double p99Us = 0.0;
    double p999Us = 0.0;
    double maxUs = 0.0;
    uint64_t overOneMs = 0;
    double longestGapMs = 0.0;
    uint64_t drops = 0;
    uint64_t refused = 0;
};

struct ScenarioResult {
    std::vector<DishRow> rows;
    std::vector<std::string> problems;
    std::vector<std::string> warnings;
    bool putFired = false;
    double putMs = 0.0;
};

uint32_t percentileNs(const std::vector<uint32_t>& sorted, uint64_t basisPoints) {
    if (sorted.empty()) return 0;
    const uint64_t rank = (sorted.size() * basisPoints + BASIS_POINTS - 1) / BASIS_POINTS;
    const uint64_t index = rank == 0 ? 0 : rank - 1;
    return sorted[std::min<uint64_t>(index, sorted.size() - 1)];
}

double percentileUs(const std::vector<uint32_t>& sorted, uint64_t basisPoints) {
    return nanosToMicros(static_cast<int64_t>(percentileNs(sorted, basisPoints)));
}

DishRow summarize(const Scenario& scenario, const DishRun& dish) {
    const DishTally& tally = dish.tally;
    const auto recorded = static_cast<std::ptrdiff_t>(
        std::min<uint64_t>(tally.completed, static_cast<uint64_t>(tally.latencyNs.size())));
    std::vector<uint32_t> sorted(tally.latencyNs.begin(), tally.latencyNs.begin() + recorded);
    std::sort(sorted.begin(), sorted.end());

    DishRow row;
    row.scenario = scenario.id;
    row.dish = dish.spec.name;
    row.rateHz = dish.spec.rateHz;
    row.frames = tally.completed;
    row.p50Us = percentileUs(sorted, P50_BASIS_POINTS);
    row.p99Us = percentileUs(sorted, P99_BASIS_POINTS);
    row.p999Us = percentileUs(sorted, P999_BASIS_POINTS);
    row.maxUs = sorted.empty() ? 0.0 : nanosToMicros(static_cast<int64_t>(sorted.back()));
    row.overOneMs = tally.overOneMs;
    row.longestGapMs = nanosToMillis(tally.longestGapNs);
    row.drops = dish.ringDrops;
    row.refused = tally.refused;
    return row;
}

std::string describe(const std::string& dish, const std::string& what, uint64_t count) {
    return "dish " + dish + ": " + std::to_string(count) + " " + what;
}

void checkDish(const DishRun& dish, bool rotated, std::vector<std::string>& problems) {
    const DishTally& tally = dish.tally;
    const std::string& name = dish.spec.name;
    const uint64_t queued = dish.produced - dish.ringDrops;
    const uint64_t served = tally.completed + tally.refused + tally.rejected + tally.replayed +
                            tally.undecryptable + tally.malformed;
    if (served != queued) {
        problems.push_back(describe(name, "frames reached the service but", served) + " " +
                           std::to_string(queued) + " were queued");
    }
    if (dish.sealFailures != 0) {
        problems.push_back(describe(name, "frames failed to encrypt", dish.sealFailures));
    }
    if (tally.completed > tally.latencyNs.size()) {
        problems.push_back(describe(name, "frames did not fit the latency array", tally.completed));
    }
    const uint64_t bad = tally.rejected + tally.replayed + tally.undecryptable + tally.malformed;
    if (bad != 0) {
        problems.push_back(describe(name, "frames were rejected, replayed or malformed", bad));
    }
    if (!rotated && tally.refused != 0) {
        problems.push_back(describe(name, "frames carried an unknown token", tally.refused));
    }
}

void checkColdWorkers(const SseWorker* sse, const MaintenanceWorker* maintenance,
                      const PutWorker* put, size_t dishCount, std::vector<std::string>& problems) {
    if (sse != nullptr) {
        const uint64_t expected = sse->ticks * dishCount;
        const bool consistent = sse->connectionsSeen == expected && sse->activeLinks == expected &&
                                sse->sessionsSeen == expected;
        if (!consistent) problems.push_back("sse thread saw an unexpected session list");
    }
    if (maintenance != nullptr && (maintenance->rumblePackets != 0 || maintenance->reaped != 0)) {
        problems.push_back("maintenance thread sent rumble or reaped a session");
    }
    if (put != nullptr && !(put->fired && put->applied)) {
        problems.push_back("the PUT did not fire or was not applied");
    }
}

class ScenarioRun {
  public:
    ScenarioRun(const Options& options, const Scenario& scenario, const RunPlan& plan)
        : scenario_(scenario), plan_(plan), gamepad_(options.submitUs, options.plugMs),
          log_(options.verbose), service_(gamepad_, client_, log_, deriveSessionKey) {
        const int rateA = scenario.dishARateHz > 0 ? scenario.dishARateHz : options.rateA;
        dishes_.push_back(makeDish("A", 0, rateA, plan.secs));
        if (scenario.withDishB) dishes_.push_back(makeDish("B", 1, options.rateB, plan.secs));
    }

    ScenarioResult execute() {
        ScenarioResult result;
        if (!openSessions(result)) return result;
        gamepad_.armPlugDelays();
        startReceiver();
        const TimePoint start = Clock::now() + std::chrono::milliseconds(START_DELAY_MS);
        const TimePoint end = start + secondsToNanos(plan_.secs);
        startColdWorkers(start, end);
        startProducers(start);
        finish();
        collect(result);
        return result;
    }

  private:
    bool openSessions(ScenarioResult& result) {
        const std::vector<ControllerDescriptor> descriptors{makeDescriptor(0)};
        for (const auto& dish : dishes_) {
            const SessionUpsertResult opened = upsertDish(service_, *dish, descriptors);
            if (!opened.ok || !allApplied(opened)) {
                result.problems.push_back("could not open the session for dish " + dish->spec.name);
                return false;
            }
            publishSession(*dish, opened);
        }
        return true;
    }

    std::vector<DishRun*> dishPointers() const {
        std::vector<DishRun*> pointers;
        for (const auto& dish : dishes_) pointers.push_back(dish.get());
        return pointers;
    }

    void startReceiver() {
        receiver_ = std::make_unique<ReceiverContext>(service_, dishPointers(), plan_.rxCpu);
        receiverThread_ = std::thread(runReceiver, std::ref(*receiver_));
        while (!receiver_->ready.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    void startProducers(TimePoint start) {
        for (const auto& dish : dishes_) {
            producerThreads_.emplace_back(runProducer, std::ref(*dish), start);
        }
    }

    TickSchedule scheduleFor(TimePoint start, TimePoint end, double periodSecs) const {
        const Nanos period = secondsToNanos(periodSecs);
        return TickSchedule{start + period / 2, period, end};
    }

    void startColdWorkers(TimePoint start, TimePoint end) {
        if (scenario_.withColdReaders) {
            std::vector<std::string> deviceIds;
            for (const auto& dish : dishes_) deviceIds.push_back(dish->spec.deviceId);
            const double scale = plan_.scheduleScale;
            sse_ = std::make_unique<SseWorker>(service_, deviceIds,
                                               scheduleFor(start, end, SSE_PERIOD_SECS * scale));
            maintenance_ = std::make_unique<MaintenanceWorker>(
                service_, scheduleFor(start, end, MAINTENANCE_PERIOD_SECS * scale));
            coldThreads_.emplace_back(runSse, std::ref(*sse_));
            coldThreads_.emplace_back(runMaintenance, std::ref(*maintenance_));
        }
        if (scenario_.withPut) {
            const double putAtSecs = std::min(PUT_AT_SECS * plan_.scheduleScale, plan_.secs / 2.0);
            put_ = std::make_unique<PutWorker>(service_, *dishes_.back(),
                                               start + secondsToNanos(putAtSecs));
            coldThreads_.emplace_back(runPut, std::ref(*put_));
        }
    }

    void finish() {
        for (std::thread& producer : producerThreads_) producer.join();
        receiver_->producersDone.store(true, std::memory_order_release);
        receiverThread_.join();
        for (std::thread& worker : coldThreads_) worker.join();
    }

    void collect(ScenarioResult& result) const {
        for (const auto& dish : dishes_) {
            const bool rotated = put_ != nullptr && dish.get() == &put_->dish;
            result.rows.push_back(summarize(scenario_, *dish));
            checkDish(*dish, rotated, result.problems);
        }
        checkColdWorkers(sse_.get(), maintenance_.get(), put_.get(), dishes_.size(),
                         result.problems);
        uint64_t completed = 0;
        for (const auto& dish : dishes_) completed += dish->tally.completed;
        if (gamepad_.submitCalls() != completed) {
            result.problems.push_back("the mock saw " + std::to_string(gamepad_.submitCalls()) +
                                      " submits for " + std::to_string(completed) +
                                      " completed frames");
        }
        const bool configurable = plan_.rxCpu != NO_CPU;
        if (configurable && !receiver_->raised) {
            result.warnings.push_back("receiver thread priority not raised");
        }
        if (configurable && !receiver_->pinned) {
            result.warnings.push_back("receiver thread not pinned");
        }
        if (put_ != nullptr) {
            result.putFired = put_->fired;
            result.putMs = put_->durationMs;
        }
    }

    const Scenario& scenario_;
    const RunPlan plan_;
    BenchGamepad gamepad_;
    BenchClient client_;
    BenchLog log_;
    SessionService service_;
    std::vector<std::unique_ptr<DishRun>> dishes_;
    std::unique_ptr<ReceiverContext> receiver_;
    std::unique_ptr<SseWorker> sse_;
    std::unique_ptr<MaintenanceWorker> maintenance_;
    std::unique_ptr<PutWorker> put_;
    std::thread receiverThread_;
    std::vector<std::thread> producerThreads_;
    std::vector<std::thread> coldThreads_;
};

std::string fixed(double value, int decimals) {
    std::array<char, 64> text{};
    std::snprintf(text.data(), text.size(), "%.*f", decimals, value);
    return text.data();
}

void reportRun(const Scenario& scenario, int rep, int reps, const ScenarioResult& result) {
    std::string line = std::string("[") + scenario.id + " rep " + std::to_string(rep + 1) + "/" +
                       std::to_string(reps) + "]";
    for (const DishRow& row : result.rows) {
        line += " " + row.dish + ": frames " + std::to_string(row.frames) + " p99.9 " +
                fixed(row.p999Us, 1) + " us max " + fixed(row.maxUs, 1) + " us drops " +
                std::to_string(row.drops) + " refused " + std::to_string(row.refused) + ";";
    }
    if (result.putFired) line += " put " + fixed(result.putMs, 1) + " ms";
    std::fprintf(stderr, "%s\n", line.c_str());
    for (const std::string& warning : result.warnings) {
        std::fprintf(stderr, "  warning: %s\n", warning.c_str());
    }
    for (const std::string& problem : result.problems) {
        std::fprintf(stderr, "  PROBLEM: %s\n", problem.c_str());
    }
}

const ScenarioResult& medianRep(const std::vector<ScenarioResult>& reps) {
    std::vector<size_t> order(reps.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::sort(order.begin(), order.end(), [&reps](size_t a, size_t b) {
        return reps[a].rows.front().p999Us < reps[b].rows.front().p999Us;
    });
    return reps[order[(order.size() - 1) / 2]];
}

constexpr size_t COLUMN_COUNT = 12;
using Cells = std::array<std::string, COLUMN_COUNT>;

constexpr std::array<const char*, COLUMN_COUNT> MARKDOWN_HEADERS{
    "scenario", "dish",   "rate Hz",   "frames",         "p50 us", "p99 us",
    "p99.9 us", "max us", "over 1 ms", "longest gap ms", "drops",  "refused"};

constexpr std::array<const char*, COLUMN_COUNT> CSV_HEADERS{
    "scenario", "dish",   "rate_hz",  "frames",         "p50_us", "p99_us",
    "p99_9_us", "max_us", "over_1ms", "longest_gap_ms", "drops",  "refused"};

Cells headerCells(const std::array<const char*, COLUMN_COUNT>& names) {
    Cells cells;
    for (size_t i = 0; i < COLUMN_COUNT; ++i) cells[i] = names[i];
    return cells;
}

Cells rowCells(const DishRow& row) {
    return Cells{row.scenario,
                 row.dish,
                 std::to_string(row.rateHz),
                 std::to_string(row.frames),
                 fixed(row.p50Us, 1),
                 fixed(row.p99Us, 1),
                 fixed(row.p999Us, 1),
                 fixed(row.maxUs, 1),
                 std::to_string(row.overOneMs),
                 fixed(row.longestGapMs, 3),
                 std::to_string(row.drops),
                 std::to_string(row.refused)};
}

std::string joinCells(const Cells& cells, const std::string& separator) {
    std::string line;
    for (size_t i = 0; i < COLUMN_COUNT; ++i) {
        if (i != 0) line += separator;
        line += cells[i];
    }
    return line;
}

std::string markdownLine(const Cells& cells) { return "| " + joinCells(cells, " | ") + " |"; }

std::string markdownRule() {
    constexpr size_t TEXT_COLUMNS = 2;
    Cells cells;
    for (size_t i = 0; i < COLUMN_COUNT; ++i) cells[i] = i < TEXT_COLUMNS ? "---" : "---:";
    return markdownLine(cells);
}

void printMarkdown(const std::vector<DishRow>& rows) {
    std::printf("%s\n", markdownLine(headerCells(MARKDOWN_HEADERS)).c_str());
    std::printf("%s\n", markdownRule().c_str());
    for (const DishRow& row : rows) std::printf("%s\n", markdownLine(rowCells(row)).c_str());
}

bool writeCsv(const std::string& path, const std::vector<DishRow>& rows) {
    std::ofstream out(path);
    if (!out) return false;
    out << joinCells(headerCells(CSV_HEADERS), ",") << '\n';
    for (const DishRow& row : rows) out << joinCells(rowCells(row), ",") << '\n';
    return static_cast<bool>(out);
}

void printBanner(const Options& options, int rxCpu) {
    const std::string receiver = rxCpu == NO_CPU ? "normal priority, unpinned (no CPU to spare)"
                                                 : "TIME_CRITICAL, cpu " + std::to_string(rxCpu);
    std::printf("frame_path_bench\n");
    std::printf("cpu: %s\n", cpuName().c_str());
    std::printf("build: %s\n", BENCH_BUILD_TYPE);
    std::printf("submit-us: %d\n", options.submitUs);
    std::printf("plug-ms: %d\n", options.plugMs);
    std::printf("rate-a: %d Hz, rate-b: %d Hz, secs: %g, reps: %d%s\n", options.rateA,
                options.rateB, options.secs, options.reps, options.smoke ? " (smoke)" : "");
    std::printf("receiver thread: %s\n\n", receiver.c_str());
    std::fflush(stdout);
}

void printLegend(const Options& options) {
    std::printf("\n");
    for (const Scenario& scenario : SCENARIOS) {
        std::printf("%s: %s\n", scenario.id, scenario.summary);
    }
    std::printf("\nEach row is the rep whose dish A p99.9 is the median of %d rep(s).\n",
                options.reps);
}

bool runScenarioOnce(const Options& options, const Scenario& scenario, const RunPlan& plan, int rep,
                     int reps, ScenarioResult& out) {
    ScenarioRun run(options, scenario, plan);
    out = run.execute();
    reportRun(scenario, rep, reps, out);
    return out.problems.empty();
}

bool warmUp(const Options& options, int rxCpu) {
    const RunPlan plan{SMOKE_SECS, SMOKE_SECS / DEFAULT_SECS, rxCpu};
    std::fprintf(stderr, "warm-up pass, results discarded\n");
    bool healthy = true;
    for (const Scenario& scenario : SCENARIOS) {
        ScenarioResult warmResult;
        healthy = runScenarioOnce(options, scenario, plan, 0, 1, warmResult) && healthy;
    }
    return healthy;
}

bool measure(const Options& options, int rxCpu, std::vector<DishRow>& rows) {
    const double scale = options.smoke ? SMOKE_SECS / DEFAULT_SECS : 1.0;
    const RunPlan plan{options.secs, scale, rxCpu};
    std::vector<std::vector<ScenarioResult>> byScenario(SCENARIOS.size());
    bool healthy = true;
    for (int rep = 0; rep < options.reps; ++rep) {
        for (size_t s = 0; s < SCENARIOS.size(); ++s) {
            ScenarioResult result;
            healthy =
                runScenarioOnce(options, SCENARIOS[s], plan, rep, options.reps, result) && healthy;
            if (result.rows.empty()) return false;
            byScenario[s].push_back(std::move(result));
        }
    }
    for (const std::vector<ScenarioResult>& reps : byScenario) {
        const ScenarioResult& median = medianRep(reps);
        rows.insert(rows.end(), median.rows.begin(), median.rows.end());
    }
    return healthy;
}

} // namespace

int main(int argc, char** argv) {
    const ParsedOptions parsed = parseOptions(argc, argv);
    if (!parsed.error.empty()) {
        std::fprintf(stderr, "%s\n", parsed.error.c_str());
        printUsage(stderr);
        return 2;
    }
    Options options = parsed.options;
    if (options.help) {
        printUsage(stdout);
        return 0;
    }
    if (options.smoke) {
        options.secs = SMOKE_SECS;
        options.reps = 1;
    }
    if (sodium_init() < 0) {
        std::fprintf(stderr, "sodium_init failed\n");
        return 1;
    }

    const int rxCpu = chooseReceiverCpu(options.rxCpu);
    printBanner(options, rxCpu);

    bool healthy = options.smoke || warmUp(options, rxCpu);
    std::vector<DishRow> rows;
    healthy = measure(options, rxCpu, rows) && healthy;
    if (rows.empty()) return 1;

    printMarkdown(rows);
    printLegend(options);
    std::fflush(stdout);

    if (!options.csvPath.empty() && !writeCsv(options.csvPath, rows)) {
        std::fprintf(stderr, "could not write %s\n", options.csvPath.c_str());
        return 1;
    }
    return healthy ? 0 : 1;
}
