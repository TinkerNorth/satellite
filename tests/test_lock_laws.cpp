// SPDX-License-Identifier: LGPL-3.0-or-later
#include "config.h"

#include "adapters/audio/opus_codec.h"
#include "adapters/client_adapter.h"
#include "adapters/log_adapter.h"
#include "app/audio_settings.h"
#include "core/byte_order.h"
#include "core/hex.h"
#include "core/json.h"
#include "core/log_writer.h"
#include "core/session_service.h"
#include "core/tracked_mutex.h"
#include "net/admin_json.h"
#include "net/inner_dispatch.h"
#include "net/pairing_service.h"
#include "net/session_crypto.h"

#if defined(_WIN32) || defined(__linux__)
#include "process_barrier.h"
#endif

#ifdef _WIN32
#include <mmsystem.h>

#include "core/gamepad_mux.h"
#include "hidmaestro_adapter.h"
#include "hidmaestro_helper_client.h"
#include "hidmaestro_wire.h"
#include "vigem_adapter.h"
#else
#include <stdlib.h>
#include <unistd.h>
#endif

#include <sodium.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "test_util.h"

namespace {

using Clock = std::chrono::steady_clock;

enum class Allowance { ByDesign, Debt };

const char* allowanceName(Allowance why) {
    return why == Allowance::ByDesign ? "by design" : "debt";
}

struct LockName {
    const char* lock;
    Allowance why;
};

struct LockPair {
    const char* first;
    const char* second;
    Allowance why;
};

const std::vector<LockName> RECEIVE_MAY_ACQUIRE = {
    {"service.state", Allowance::Debt},
};

const std::vector<LockName> LIFECYCLE_MAY_HOLD = {
    {"service.topology", Allowance::ByDesign},
};

#ifdef _WIN32
const std::vector<LockName> HELPER_PIPE_MAY_HOLD = {
    {"hidmaestro.helper", Allowance::Debt},
    {"service.topology", Allowance::ByDesign},
};
#endif

const std::vector<LockPair> BLOCKING_MAY_HOLD = {
    {"backend.lifecycle", "service.topology", Allowance::ByDesign},
#ifdef _WIN32
    {"helper.pipe", "hidmaestro.helper", Allowance::Debt},
    {"helper.pipe", "service.topology", Allowance::ByDesign},
    {"thread.join", "service.topology", Allowance::ByDesign},
    {"vigem.pnp", "service.topology", Allowance::ByDesign},
#endif
};

const std::vector<LockPair> ORDER_MAY_NEST = {
    {"config.write", "config.snapshot", Allowance::ByDesign},
    {"service.topology", "service.state", Allowance::ByDesign},
#ifdef _WIN32
    {"service.topology", "config.snapshot", Allowance::ByDesign},
    {"service.topology", "hidmaestro.bus", Allowance::ByDesign},
    {"service.topology", "hidmaestro.helper", Allowance::ByDesign},
    {"service.topology", "vigem.bus", Allowance::ByDesign},
#endif
};

constexpr int COLD_ROUNDS = 24;
constexpr int REAP_ROUND_EVERY = 6;
constexpr auto RECEIVE_TICK = std::chrono::microseconds(500);
constexpr auto COLD_TICK = std::chrono::milliseconds(4);
constexpr auto WORKER_TICK = std::chrono::milliseconds(2);
constexpr auto PLUG_TIME = std::chrono::milliseconds(2);
constexpr auto WAIT_LIMIT = std::chrono::seconds(60);
constexpr int REAP_SILENCE_SEC = 60;
constexpr uint32_t MOTION_EVERY = 2;
constexpr uint32_t TOUCHPAD_EVERY = 4;
constexpr uint32_t MIC_EVERY = 20;
constexpr uint32_t HEARTBEAT_EVERY = 25;
constexpr uint32_t BATTERY_EVERY = 50;
constexpr uint32_t REAP_EVERY = 5;
constexpr int OPUS_FRAMES = 8;
constexpr double PI = 3.141592653589793;
constexpr double TONE_HZ = 440.0;
constexpr double TONE_AMPLITUDE = 8000.0;
constexpr size_t OPUS_PACKET_LIMIT = 512;
constexpr size_t MOTION_AXES = 6;
constexpr uint32_t MOTION_DELTA_US = 2000;
constexpr uint8_t BATTERY_LEVEL = 80;
constexpr uint8_t TOUCH_TRACKING_ID = 7;
constexpr uint32_t TOUCH_MS_PER_FRAME = 8;
constexpr uint16_t FIRST_PORT = 41000;
constexpr uint32_t SENDER_IPV4 = 0x0100007fu;
constexpr uint16_t GAMEPAD_DATA_MIN_LEN = 1 + sizeof(GamepadReport);
constexpr double NANOS_PER_MICRO = 1000.0;

using NameSet = std::set<std::string>;
using PairSet = std::set<std::pair<std::string, std::string>>;

class DiscardingLogSink : public satellite::ILogSink {
  public:
    bool write(std::string_view) override { return true; }
    bool flush() override { return true; }
    bool rotateIfDue() override { return false; }
    void purge() override {}
};

ProcessBarrierFn productionBarrier() {
#if defined(_WIN32)
    return processbarrier::flushWriteBuffers;
#elif defined(__linux__)
    return processbarrier::registerPrivateExpedited();
#else
    return nullptr;
#endif
}

unsigned long processId() {
#ifdef _WIN32
    return static_cast<unsigned long>(GetCurrentProcessId());
#else
    return static_cast<unsigned long>(getpid());
#endif
}

class ConfigSandbox {
  public:
    ConfigSandbox() : marker_("satellite-lock-laws-" + std::to_string(processId())) {
#ifdef _WIN32
        wchar_t temp[MAX_PATH];
        const DWORD length = GetTempPathW(MAX_PATH, temp);
        if (length == 0 || length >= MAX_PATH) return;
        dir_ = std::filesystem::path(std::wstring(temp, length)) / marker_;
        std::error_code ec;
        std::filesystem::create_directories(dir_, ec);
        if (ec) return;
        ready_ = SetEnvironmentVariableW(L"SATELLITE_CONFIG_DIR", dir_.wstring().c_str()) != 0;
#else
        std::string pattern = "/tmp/" + marker_ + "-XXXXXX";
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        const char* made = mkdtemp(buffer.data());
        if (made == nullptr) return;
        dir_ = made;
        ready_ = setenv("SATELLITE_CONFIG_DIR", made, 1) == 0;
#endif
        ready_ = ready_ && configPath().find(marker_) != std::string::npos;
    }

    ~ConfigSandbox() {
        std::error_code ec;
        if (!dir_.empty()) std::filesystem::remove_all(dir_, ec);
#ifdef _WIN32
        SetEnvironmentVariableW(L"SATELLITE_CONFIG_DIR", nullptr);
#else
        unsetenv("SATELLITE_CONFIG_DIR");
#endif
    }

    ConfigSandbox(const ConfigSandbox&) = delete;
    ConfigSandbox& operator=(const ConfigSandbox&) = delete;

    bool ready() const { return ready_; }

    bool saved() const {
        std::error_code ec;
        return std::filesystem::exists(dir_ / "config.json", ec);
    }

  private:
    const std::string marker_;
    std::filesystem::path dir_;
    bool ready_ = false;
};

struct Credentials {
    uint32_t token = 0;
    std::array<uint8_t, CRYPTO_KEY_SIZE> key{};
};

class Dish {
  public:
    Dish(std::string id, uint8_t seed, uint16_t port) : id_(std::move(id)), port_(port) {
        for (size_t i = 0; i < pairingKey_.size(); ++i) {
            pairingKey_[i] = static_cast<uint8_t>(seed + i);
        }
    }

    Dish(const Dish&) = delete;
    Dish& operator=(const Dish&) = delete;

    const std::string& id() const { return id_; }
    std::string name() const { return "Lock law " + id_; }
    std::string ip() const { return "127.0.0.1"; }
    uint16_t port() const { return port_; }
    const uint8_t* pairingKey() const { return pairingKey_.data(); }
    std::string keyHex() const { return hexEncode(pairingKey_.data(), pairingKey_.size()); }

    void adopt(const SessionUpsertResult& result) {
        if (!result.ok) return;
        auto credentials = std::make_unique<Credentials>();
        credentials->token = result.token;
        deriveSessionKey(pairingKey_.data(), result.sessionSalt, result.token,
                         credentials->key.data());
        std::lock_guard<std::mutex> lk(mtx_);
        current_.store(credentials.get(), std::memory_order_release);
        issued_.push_back(std::move(credentials));
    }

    const Credentials* current() const { return current_.load(std::memory_order_acquire); }

    uint32_t nextCounter(const Credentials* credentials) {
        if (credentials != sealedWith_) {
            sealedWith_ = credentials;
            counter_ = 0;
        }
        return ++counter_;
    }

  private:
    const std::string id_;
    const uint16_t port_;
    std::array<uint8_t, CRYPTO_KEY_SIZE> pairingKey_{};
    std::mutex mtx_;
    std::atomic<const Credentials*> current_{nullptr};
    std::vector<std::unique_ptr<Credentials>> issued_;
    const Credentials* sealedWith_ = nullptr;
    uint32_t counter_ = 0;
};

ControllerDescriptor pad(uint8_t index, uint8_t type, uint16_t caps = 0,
                         uint8_t touchpadMode = TOUCHPAD_MODE_OFF) {
    ControllerDescriptor descriptor;
    descriptor.ctrlIdx = index;
    descriptor.type = type;
    descriptor.caps = caps;
    descriptor.touchpadMode = touchpadMode;
    return descriptor;
}

SessionUpsertResult put(SessionService& svc, Dish& dish,
                        const std::vector<ControllerDescriptor>& pads, bool mouse = false) {
    const SessionUpsertResult result =
        svc.upsertSession(dish.id(), dish.name(), dish.ip(), dish.pairingKey(), pads, mouse);
    dish.adopt(result);
    return result;
}

void pairInConfig(const std::vector<const Dish*>& dishes) {
    Config cfg;
    cfg.controllerAudio = false;
    for (const Dish* dish : dishes) {
        PairedDevice device;
        device.id = dish->id();
        device.name = dish->name();
        device.lastIP = dish->ip();
        device.pairedAt = "2026-10-27";
        device.sharedKeyHex = dish->keyHex();
        cfg.pairedDevices.push_back(device);
    }
    g_configStore = std::make_unique<ConfigStore>(std::move(cfg), saveConfig);
}

class LifecycleWatch {
  public:
    void check(const std::string& where, bool lifecycleCall) {
        const std::vector<std::string> held = lockstats::heldByThisThread();
        std::lock_guard<std::mutex> lk(mtx_);
        calls_++;
        const bool topology = std::find(held.begin(), held.end(), "service.topology") != held.end();
        if (lifecycleCall && !topology) unguarded_.insert(where);
        for (const std::string& lock : held) held_.insert(lock);
    }

    int calls() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return calls_;
    }

    NameSet held() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return held_;
    }

    NameSet unguarded() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return unguarded_;
    }

  private:
    mutable std::mutex mtx_;
    int calls_ = 0;
    NameSet held_;
    NameSet unguarded_;
};

class LawBackend : public IGamepadPort {
  public:
    explicit LawBackend(LifecycleWatch& watch) : watch_(watch) {}

    bool ensureBusOpen() override {
        watch_.check("mock ensureBusOpen", true);
        busOpen_.store(true);
        return true;
    }

    void closeBus() override {
        watch_.check("mock closeBus", true);
        for (auto& plugged : plugged_) plugged.store(false);
        busOpen_.store(false);
    }

    bool isBusOpen() const override { return busOpen_.load(); }

    bool pluginDevice(uint32_t serial, GamepadIdentity) override {
        watch_.check("mock pluginDevice", true);
        return plug(serial);
    }

    bool pluginDevicePreferring(uint32_t serial, GamepadIdentity, const std::string&) override {
        watch_.check("mock pluginDevicePreferring", true);
        return plug(serial);
    }

    bool unplugDevice(uint32_t serial) override {
        watch_.check("mock unplugDevice", true);
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

    bool submitBattery(uint32_t serial, const BatteryReport&) override {
        return isDevicePlugged(serial);
    }

    bool submitTouchpad(uint32_t serial, const TouchpadReport&) override {
        return isDevicePlugged(serial);
    }

    bool submitRelativeMouse(int, int, const MouseButtons&, int) override { return true; }
    bool supportsRelativeMouse() const override { return true; }

    bool supportsMotionForType(uint8_t type) const override {
        return controllerTypeHasMotion(type);
    }

    void setRumbleCallback(RumbleCallback cb) override { rumble_ = std::move(cb); }
    void setLightbarCallback(LightbarCallback cb) override { lightbar_ = std::move(cb); }

    void fireFeedback(uint32_t serial, uint16_t level) {
        RumbleReport report;
        report.strongMagnitude = level;
        report.weakMagnitude = level;
        if (rumble_) rumble_(serial, report);
        if (lightbar_) lightbar_(serial, static_cast<uint8_t>(level & 0xFF), 0, 0);
    }

  private:
    static bool valid(uint32_t serial) { return serial >= 1 && serial <= MAX_BACKEND_CONTROLLERS; }

    bool plug(uint32_t serial) {
        std::this_thread::sleep_for(PLUG_TIME);
        if (!valid(serial)) return false;
        plugged_[serial].store(true);
        return true;
    }

    LifecycleWatch& watch_;
    std::array<std::atomic<bool>, MAX_BACKEND_CONTROLLERS + 1> plugged_{};
    std::atomic<bool> busOpen_{false};
    RumbleCallback rumble_;
    LightbarCallback lightbar_;
};

std::vector<std::vector<uint8_t>> encodeMicFrames() {
    std::vector<std::vector<uint8_t>> frames;
    auto encoder = satellite::audio::OpusStreamEncoder::create(satellite::audio::Stream::Mic);
    if (!encoder) return frames;
    const size_t samples = static_cast<size_t>(AUDIO_FRAME_SAMPLES) * AUDIO_MIC_CHANNELS;
    std::vector<int16_t> pcm(samples);
    for (int frame = 0; frame < OPUS_FRAMES; ++frame) {
        for (size_t i = 0; i < samples; ++i) {
            const double t = static_cast<double>(static_cast<size_t>(frame) * samples + i) /
                             static_cast<double>(AUDIO_SAMPLE_RATE_HZ);
            pcm[i] = static_cast<int16_t>(TONE_AMPLITUDE * std::sin(2.0 * PI * TONE_HZ * t));
        }
        std::array<uint8_t, OPUS_PACKET_LIMIT> packet{};
        const size_t bytes = encoder->encode(pcm.data(), static_cast<size_t>(AUDIO_FRAME_SAMPLES),
                                             packet.data(), packet.size());
        if (bytes > 0) frames.emplace_back(packet.begin(), packet.begin() + bytes);
    }
    return frames;
}

std::vector<uint8_t> gamepadPayload(uint8_t index, uint32_t tick) {
    std::vector<uint8_t> payload(GAMEPAD_DATA_MIN_LEN);
    payload[0] = index;
    GamepadReport report;
    report.wButtons = static_cast<uint16_t>(tick);
    report.sThumbRX = static_cast<int16_t>(tick);
    std::memcpy(payload.data() + 1, &report, sizeof(report));
    return payload;
}

std::vector<uint8_t> motionPayload(uint8_t index, uint32_t tick) {
    std::vector<uint8_t> payload(1 + static_cast<size_t>(MOTION_WIRE_PAYLOAD_BYTES));
    payload[0] = index;
    for (size_t axis = 0; axis < MOTION_AXES; ++axis) {
        satellite::writeLE16(payload.data() + 1 + axis * 2, static_cast<uint16_t>(tick + axis));
    }
    satellite::writeLE32(payload.data() + 1 + MOTION_AXES * 2, MOTION_DELTA_US);
    return payload;
}

std::vector<uint8_t> batteryPayload(uint8_t index) {
    return {index, BATTERY_LEVEL, BATTERY_STATUS_DISCHARGING};
}

std::vector<uint8_t> touchpadPayload(uint8_t index, uint32_t frame) {
    std::vector<uint8_t> payload(1 + static_cast<size_t>(TOUCHPAD_WIRE_PAYLOAD_BYTES_V2));
    payload[0] = index;
    uint8_t* touch = payload.data() + 1;
    touch[0] = 0x01;
    touch[2] = TOUCH_TRACKING_ID;
    satellite::writeLE16(touch + 3, static_cast<uint16_t>((frame * 37u) % 30000u));
    satellite::writeLE16(touch + 5, static_cast<uint16_t>((frame * 11u) % 30000u));
    satellite::writeLE32(touch + 12, frame * TOUCH_MS_PER_FRAME);
    return payload;
}

std::vector<uint8_t> micPayload(uint8_t index, uint16_t seq, const std::vector<uint8_t>& opus) {
    std::vector<uint8_t> payload(static_cast<size_t>(AUDIO_WIRE_HEADER_BYTES) + opus.size());
    encodeAudioFrameHeader(payload.data(), index, seq);
    std::memcpy(payload.data() + AUDIO_WIRE_HEADER_BYTES, opus.data(), opus.size());
    return payload;
}

struct StreamPad {
    uint8_t index = 0;
    bool motion = false;
    bool touchpad = false;
    bool mic = false;
};

struct StreamDish {
    Dish* dish = nullptr;
    std::vector<StreamPad> pads;
};

class ReceiveDriver {
  public:
    ReceiveDriver(SessionService& svc, std::vector<StreamDish> dishes)
        : svc_(svc), dishes_(std::move(dishes)), opus_(encodeMicFrames()) {}

    void run(const std::atomic<bool>& stop) {
        ScopedThreadRole role(ThreadRole::Receive);
        for (uint32_t tick = 0; !stop.load(std::memory_order_acquire); ++tick) {
            for (StreamDish& stream : dishes_) sendTick(stream, tick);
            std::this_thread::sleep_for(RECEIVE_TICK);
        }
    }

    uint64_t handled(uint16_t msgType) const {
        const auto it = handled_.find(msgType);
        return it == handled_.end() ? 0 : it->second;
    }

    bool encoded() const { return !opus_.empty(); }

  private:
    void sendTick(StreamDish& stream, uint32_t tick) {
        Dish& dish = *stream.dish;
        for (const StreamPad& p : stream.pads) {
            send(dish, MSG_GAMEPAD_DATA, gamepadPayload(p.index, tick));
            if (p.motion && tick % MOTION_EVERY == 0) {
                send(dish, MSG_MOTION, motionPayload(p.index, tick));
            }
            if (p.touchpad && tick % TOUCHPAD_EVERY == 0) {
                send(dish, MSG_TOUCHPAD, touchpadPayload(p.index, tick / TOUCHPAD_EVERY));
            }
            if (p.mic && tick % MIC_EVERY == 0 && !opus_.empty()) {
                const auto seq = static_cast<uint16_t>(tick / MIC_EVERY);
                const std::vector<uint8_t>& packet = opus_[seq % opus_.size()];
                send(dish, MSG_MIC_AUDIO, micPayload(p.index, seq, packet));
            }
        }
        if (tick % HEARTBEAT_EVERY == 0) send(dish, MSG_HEARTBEAT_PING, {});
        if (tick % BATTERY_EVERY == 0) send(dish, MSG_BATTERY, batteryPayload(0));
    }

    void send(Dish& dish, uint16_t msgType, const std::vector<uint8_t>& payload) {
        const Credentials* credentials = dish.current();
        if (credentials == nullptr) return;
        std::vector<uint8_t> inner(INNER_HEADER_SIZE + payload.size());
        satellite::writeBE16(inner.data(), msgType);
        satellite::writeBE16(inner.data() + 2, static_cast<uint16_t>(payload.size()));
        if (!payload.empty()) {
            std::memcpy(inner.data() + INNER_HEADER_SIZE, payload.data(), payload.size());
        }
        const uint32_t counter = dish.nextCounter(credentials);
        std::array<uint8_t, UDP_DATAGRAM_MAX_BYTES> datagram{};
        satellite::writeBE32(datagram.data(), credentials->token);
        satellite::writeBE32(datagram.data() + 4, counter);
        unsigned long long sealed = 0;
        if (!encryptPacket(credentials->key.data(), CRYPTO_DIR_CLIENT_TO_SERVER, counter,
                           credentials->token, inner.data(), inner.size(),
                           datagram.data() + HEADER_SIZE, &sealed)) {
            return;
        }
        receive(datagram.data(), static_cast<int>(HEADER_SIZE + sealed), dish.port());
    }

    void receive(uint8_t* buf, int n, uint16_t senderPort) {
        const uint32_t token = satellite::readBE32(buf);
        const uint32_t counter = satellite::readBE32(buf + 4);
        uint8_t key[CRYPTO_KEY_SIZE];
        uint32_t lastCounter = 0;
        bool seenCounter = false;
        if (!svc_.getDecryptInfo(token, key, lastCounter, &seenCounter)) return;
        if (seenCounter && counter <= lastCounter) return;
        uint8_t* plaintext = buf + HEADER_SIZE;
        const auto ctLen = static_cast<size_t>(n - HEADER_SIZE);
        unsigned long long ptLen = 0;
        if (!decryptPacket(key, CRYPTO_DIR_CLIENT_TO_SERVER, counter, token, plaintext, ctLen,
                           plaintext, &ptLen)) {
            return;
        }
        InnerHeader inner;
        if (!parseInnerHeader(plaintext, static_cast<size_t>(ptLen), inner)) return;
        const uint8_t* payload = plaintext + INNER_HEADER_SIZE;
        if (inner.msgType == MSG_GAMEPAD_DATA && inner.msgLen >= GAMEPAD_DATA_MIN_LEN) {
            const uint8_t ctrlIdx = payload[0];
            GamepadReport report;
            std::memcpy(&report, payload + 1, sizeof(GamepadReport));
            svc_.handleGamepadDataAndUpdate(token, counter, SENDER_IPV4, senderPort, ctrlIdx,
                                            report);
            handled_[inner.msgType]++;
            return;
        }
        svc_.updatePostDecryptV4(token, counter, SENDER_IPV4, senderPort);
        const DispatchResult dispatched =
            dispatchInnerMessage(svc_, token, inner.msgType, payload, inner.msgLen);
        if (dispatched.handled) handled_[inner.msgType]++;
    }

    SessionService& svc_;
    std::vector<StreamDish> dishes_;
    const std::vector<std::vector<uint8_t>> opus_;
    std::map<uint16_t, uint64_t> handled_;
};

struct ColdTally {
    std::atomic<int> rounds{0};
    std::atomic<int> refused{0};
    std::atomic<int> reaped{0};
    std::atomic<int> snapshots{0};
    std::atomic<int> feedback{0};
};

using ColdRound = std::function<void(int round, ColdTally& tally)>;
using FeedbackTick = std::function<void(uint16_t level)>;

void runAdmin(SessionService& svc, const std::atomic<bool>& stop, ColdTally& tally) {
    ScopedThreadRole role(ThreadRole::HttpAdmin);
    while (!stop.load(std::memory_order_acquire)) {
        const bool built = !buildConnectionsJson(svc).empty() && !buildDevicesJson(svc).empty() &&
                           !buildLogsJson(0).empty();
        if (built) tally.snapshots++;
        std::this_thread::sleep_for(COLD_TICK);
    }
}

void runMaintenance(SessionService& svc, const std::atomic<bool>& stop, ColdTally& tally) {
    ScopedThreadRole role(ThreadRole::Maintenance);
    const Clock::time_point deadline = Clock::now() + WAIT_LIMIT;
    for (uint32_t tick = 0;; ++tick) {
        const bool done = stop.load(std::memory_order_acquire) && tally.reaped.load() > 0;
        if (done || Clock::now() > deadline) return;
        svc.refreshRumble();
        svc.refreshIdleInput();
        if (tick % REAP_EVERY == 0) tally.reaped += svc.reapTimedOut();
        std::this_thread::sleep_for(WORKER_TICK);
    }
}

void runFeedback(const FeedbackTick& tickFeedback, const std::atomic<bool>& stop,
                 ColdTally& tally) {
    ScopedThreadRole role(ThreadRole::BackendWorker);
    for (uint16_t level = 1; !stop.load(std::memory_order_acquire); ++level) {
        tickFeedback(level);
        tally.feedback++;
        std::this_thread::sleep_for(WORKER_TICK);
    }
}

class LawRun {
  public:
    LawRun(SessionService& svc, std::vector<StreamDish> streams)
        : svc_(svc), receiver_(svc, std::move(streams)) {}

    void run(const ColdRound& coldRound, const FeedbackTick& tickFeedback) {
        std::atomic<bool> stop{false};
        std::thread receive([&] { receiver_.run(stop); });
        std::thread admin([&] { runAdmin(svc_, stop, tally_); });
        std::thread maintenance([&] { runMaintenance(svc_, stop, tally_); });
        std::thread feedback;
        if (tickFeedback) feedback = std::thread([&] { runFeedback(tickFeedback, stop, tally_); });
        {
            ScopedThreadRole role(ThreadRole::HttpClient);
            for (int round = 0; round < COLD_ROUNDS; ++round) {
                coldRound(round, tally_);
                tally_.rounds++;
                std::this_thread::sleep_for(COLD_TICK);
            }
        }
        stop.store(true, std::memory_order_release);
        if (feedback.joinable()) feedback.join();
        maintenance.join();
        admin.join();
        receive.join();
    }

    const ReceiveDriver& receiver() const { return receiver_; }
    const ColdTally& tally() const { return tally_; }

  private:
    SessionService& svc_;
    ReceiveDriver receiver_;
    ColdTally tally_;
};

void putOrRefuse(SessionService& svc, Dish& dish, const std::vector<ControllerDescriptor>& pads,
                 ColdTally& tally) {
    if (!put(svc, dish, pads).ok) tally.refused++;
}

void expectTheRunReachedEverything(const std::string& phase, const LawRun& run, bool feedback) {
    TEST(phase + ": the receive thread handled every message kind");
    EXPECT(run.receiver().encoded());
    for (const uint16_t kind : {MSG_GAMEPAD_DATA, MSG_HEARTBEAT_PING, MSG_MOTION, MSG_BATTERY,
                                MSG_TOUCHPAD, MSG_MIC_AUDIO}) {
        if (run.receiver().handled(kind) == 0) {
            std::cerr << "  " << phase << ": no message of type " << kind << " was handled\n";
        }
        EXPECT(run.receiver().handled(kind) > 0);
    }
    TEST(phase + ": every cold thread ran");
    EXPECT_EQ(run.tally().rounds.load(), COLD_ROUNDS);
    EXPECT_EQ(run.tally().refused.load(), 0);
    EXPECT(run.tally().reaped.load() > 0);
    EXPECT(run.tally().snapshots.load() > 0);
    if (feedback) EXPECT(run.tally().feedback.load() > 0);
}

void runTheServiceOnTheMockBackend(LifecycleWatch& watch, const ConfigSandbox& sandbox) {
    Dish a("dish-a", 0x10, FIRST_PORT);
    Dish b("dish-b", 0x30, FIRST_PORT + 1);
    Dish c("dish-c", 0x50, FIRST_PORT + 2);
    Dish d("dish-d", 0x70, FIRST_PORT + 3);
    Dish r("dish-r", 0x90, FIRST_PORT + 4);
    pairInConfig({&a, &b, &c, &r});

    LawBackend backend(watch);
    ClientAdapter client;
    LogAdapter log;
    satellite::audio::OpusCodecFactory codecs;
    SessionService svc(backend, client, log, deriveSessionKey, &codecs, productionBarrier());
    svc.setAudioPolicy(audioPolicyFromConfig);

    TEST("mock: the streaming sessions open");
    const std::vector<ControllerDescriptor> padsA{
        pad(0, CONTROLLER_TYPE_PLAYSTATION, CAP_MOTION, TOUCHPAD_MODE_DS4),
        pad(1, CONTROLLER_TYPE_PLAYSTATION, 0, TOUCHPAD_MODE_MOUSE),
        pad(2, CONTROLLER_TYPE_PLAYSTATION, CAP_MIC),
        pad(3, CONTROLLER_TYPE_XBOX),
    };
    const SessionUpsertResult openedA = put(svc, a, padsA, true);
    EXPECT(openedA.ok);
    EXPECT(openedA.mouseControlGranted);
    EXPECT(put(svc, b, {pad(0, CONTROLLER_TYPE_XBOX)}).ok);

    LawRun run(svc, {{&a,
                      {{0, true, true, false},
                       {1, false, true, false},
                       {2, false, false, true},
                       {3, false, false, true}}},
                     {&b, {{0, false, false, true}}}});
    run.run(
        [&](int round, ColdTally& tally) {
            std::vector<ControllerDescriptor> padsB{pad(0, CONTROLLER_TYPE_XBOX)};
            if (round % 2 == 1) padsB.push_back(pad(1, CONTROLLER_TYPE_PLAYSTATION, CAP_MOTION));
            const SessionUpsertResult upserted = put(svc, b, padsB);
            if (!upserted.ok) tally.refused++;
            refreshPairedDeviceIdentity(b.id(), b.ip(), b.name());
            ControllerApplyResult applied;
            uint16_t epoch = 0;
            svc.applyController(upserted.connectionId, b.id(), pad(2, CONTROLLER_TYPE_PLAYSTATION),
                                applied, epoch);
            svc.removeController(upserted.connectionId, b.id(), 2, epoch);

            const SessionUpsertResult opened = put(svc, c, {pad(0, CONTROLLER_TYPE_XBOX)});
            if (!opened.ok) tally.refused++;
            svc.closeSessionById(opened.connectionId, c.id(), CLOSE_REASON_REPLACED, false);

            upsertPairedDevice(d.id(), d.name(), d.ip(), d.keyHex());
            putOrRefuse(svc, d, {pad(0, CONTROLLER_TYPE_PLAYSTATION)}, tally);
            std::string rotated;
            rotatePairedDeviceKey(d.id(), d.ip(), rotated);
            svc.closeSessionsForDevice(d.id(), CLOSE_REASON_UNPAIRED);
            forgetPairedDevice(d.id());

            if (round % REAP_ROUND_EVERY == 0) {
                const SessionUpsertResult silent = put(svc, r, {pad(0, CONTROLLER_TYPE_XBOX)});
                if (!silent.ok) tally.refused++;
                svc.backdateForTest(silent.token, REAP_SILENCE_SEC, 1);
            }
        },
        [&](uint16_t level) {
            for (uint32_t serial = 1; serial <= MAX_BACKEND_CONTROLLERS; ++serial) {
                backend.fireFeedback(serial, level);
            }
        });
    svc.closeAllSessions();

    expectTheRunReachedEverything("mock", run, true);
    TEST("mock: the pairing service saved the config into the sandbox");
    EXPECT(g_configStore->flush(WAIT_LIMIT));
    EXPECT(sandbox.saved());
    TEST("mock: an unchanged identity refresh publishes nothing, a renamed one publishes the name");
    const std::shared_ptr<const Config> settled = g_configStore->current();
    refreshPairedDeviceIdentity(b.id(), b.ip(), b.name());
    EXPECT(g_configStore->current() == settled);
    const std::string renamed = "Renamed " + b.name();
    refreshPairedDeviceIdentity(b.id(), b.ip(), renamed);
    PairedDevice stored;
    EXPECT(findPairedDevice(b.id(), stored));
    EXPECT_EQ(stored.name, renamed);
    TEST("mock: the backend saw lifecycle calls");
    EXPECT(watch.calls() > 0);
}

#ifdef _WIN32

namespace hm = satellite::hidmaestro;

LifecycleWatch* g_driverWatch = nullptr;

void checkDriver(const char* where) {
    if (g_driverWatch != nullptr) g_driverWatch->check(where, true);
}

constexpr DWORD PIPE_BUFFER = 64 * 1024;

class FakeHelper {
  public:
    FakeHelper() = default;
    FakeHelper(const FakeHelper&) = delete;
    FakeHelper& operator=(const FakeHelper&) = delete;

    ~FakeHelper() {
        if (thread_.joinable()) thread_.join();
        for (auto& [serial, slot] : live_) close(slot);
        if (server_ != INVALID_HANDLE_VALUE) CloseHandle(server_);
    }

    HANDLE start() {
        const std::wstring name =
            L"\\\\.\\pipe\\satellite-lock-laws-" + std::to_wstring(GetCurrentProcessId());
        server_ = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, PIPE_BUFFER,
                                   PIPE_BUFFER, 0, nullptr);
        if (server_ == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
        HANDLE client = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (client == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
        thread_ = std::thread([this] { serve(); });
        return client;
    }

    int plugs() const { return plugs_.load(); }
    int unplugs() const { return unplugs_.load(); }

  private:
    struct Slot {
        HANDLE input = nullptr;
        HANDLE inputEvent = nullptr;
        HANDLE companionEvent = nullptr;
        HANDLE output = nullptr;
        HANDLE outputEvent = nullptr;
    };

    static void close(Slot& slot) {
        for (HANDLE h :
             {slot.input, slot.inputEvent, slot.companionEvent, slot.output, slot.outputEvent}) {
            if (h != nullptr) CloseHandle(h);
        }
        slot = Slot{};
    }

    static HANDLE duplicate(HANDLE h) {
        HANDLE out = nullptr;
        DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &out, 0, FALSE,
                        DUPLICATE_SAME_ACCESS);
        return out;
    }

    static int64_t handleValue(HANDLE h) {
        return static_cast<int64_t>(reinterpret_cast<uintptr_t>(h));
    }

    void serve() {
        std::string line;
        while (readLine(line)) {
            bool last = false;
            const std::string reply = answer(line, last);
            if (!writeLine(reply) || last) return;
        }
    }

    bool readLine(std::string& line) {
        line.clear();
        for (;;) {
            char ch = 0;
            DWORD read = 0;
            if (!ReadFile(server_, &ch, 1, &read, nullptr) || read != 1) return false;
            if (ch == '\n') return true;
            line.push_back(ch);
        }
    }

    bool writeLine(std::string reply) {
        reply.push_back('\n');
        DWORD written = 0;
        const BOOL ok =
            WriteFile(server_, reply.data(), static_cast<DWORD>(reply.size()), &written, nullptr);
        return ok != 0 && written == reply.size();
    }

    std::string answer(const std::string& request, bool& last) {
        satellite::Json j;
        if (!satellite::jsonParse(request, j)) return R"({"ok":false})";
        const std::string op = satellite::jsonStr(j, "op");
        const auto serial = static_cast<uint32_t>(satellite::jsonInt(j, "serial", 0));
        if (op == "plug") return plug(serial);
        if (op == "unplug") unplug(serial);
        if (op == "shutdown") last = true;
        return R"({"ok":true})";
    }

    std::string plug(uint32_t serial) {
        unplug(serial);
        Slot slot;
        slot.input = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                        static_cast<DWORD>(hm::INPUT_SECTION_SIZE), nullptr);
        slot.inputEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        slot.companionEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        slot.output = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                         static_cast<DWORD>(hm::OUTPUT_SECTION_SIZE), nullptr);
        slot.outputEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        satellite::JsonOut reply;
        reply["ok"] = true;
        reply["index"] = serial;
        reply["input"] = handleValue(duplicate(slot.input));
        reply["inputEvent"] = handleValue(duplicate(slot.inputEvent));
        reply["companionEvent"] = handleValue(duplicate(slot.companionEvent));
        reply["output"] = handleValue(duplicate(slot.output));
        reply["outputEvent"] = handleValue(duplicate(slot.outputEvent));
        live_[serial] = slot;
        plugs_++;
        return satellite::jsonDump(reply);
    }

    void unplug(uint32_t serial) {
        const auto it = live_.find(serial);
        if (it == live_.end()) return;
        close(it->second);
        live_.erase(it);
        unplugs_++;
    }

    HANDLE server_ = INVALID_HANDLE_VALUE;
    std::thread thread_;
    std::map<uint32_t, Slot> live_;
    std::atomic<int> plugs_{0};
    std::atomic<int> unplugs_{0};
};

class CheckedProvisioner : public hm::IHidMaestroProvisioner {
  public:
    CheckedProvisioner(hm::HelperClient& helper, LifecycleWatch& watch)
        : helper_(helper), watch_(watch) {}

    bool installed() const override {
        watch_.check("helper installed", false);
        return true;
    }

    bool isReady() const override {
        watch_.check("helper isReady", false);
        return helper_.isReady();
    }

    bool ensureReady() override {
        watch_.check("helper ensureReady", true);
        return helper_.isReady();
    }

    void shutdown() override {
        watch_.check("helper shutdown", false);
        helper_.shutdown();
    }

    bool provision(uint32_t serial, GamepadIdentity identity, bool audio,
                   hm::ProvisionResult& out) override {
        watch_.check("helper provision", true);
        return helper_.provision(serial, identity, audio, out);
    }

    bool deprovision(uint32_t serial) override {
        watch_.check("helper deprovision", true);
        return helper_.deprovision(serial);
    }

  private:
    hm::HelperClient& helper_;
    LifecycleWatch& watch_;
};

void runTheServiceOnTheWindowsBackends(LifecycleWatch& watch) {
    Dish w1("dish-w1", 0xA0, FIRST_PORT + 10);
    Dish w2("dish-w2", 0xB0, FIRST_PORT + 11);
    Dish w3("dish-w3", 0xC0, FIRST_PORT + 12);
    Dish w4("dish-w4", 0xD0, FIRST_PORT + 13);
    Dish wr("dish-wr", 0xE0, FIRST_PORT + 14);
    pairInConfig({&w1, &w2, &w3, &w4, &wr});

    FakeHelper fake;
    const HANDLE channel = fake.start();
    hm::HelperClient helper;
    TEST("windows: the helper client talks to an in-process helper over a real pipe");
    EXPECT(channel != INVALID_HANDLE_VALUE);
    EXPECT(channel != INVALID_HANDLE_VALUE && helper.adoptChannelForTest(channel));
    CheckedProvisioner provisioner(helper, watch);
    g_driverWatch = &watch;
    {
        ViGEmAdapter vigem;
        HidMaestroAdapter hidMaestro(provisioner, controllerAudioEnabled);
        satellite::GamepadMux mux({&vigem, &hidMaestro});
        ClientAdapter client;
        LogAdapter log;
        satellite::audio::OpusCodecFactory codecs;
        SessionService svc(mux, client, log, deriveSessionKey, &codecs, productionBarrier());
        svc.setAudioPolicy(audioPolicyFromConfig);

        TEST("windows: the streaming sessions open on both backends");
        EXPECT(put(svc, w1, {pad(0, CONTROLLER_TYPE_XBOX)}).ok);
        EXPECT(
            put(svc, w2, {pad(0, CONTROLLER_TYPE_PLAYSTATION, CAP_MOTION, TOUCHPAD_MODE_DS4)}).ok);
        EXPECT(put(svc, w3,
                   {pad(0, CONTROLLER_TYPE_DUALSENSE, CAP_MOTION | CAP_MIC, TOUCHPAD_MODE_DS4)})
                   .ok);

        LawRun run(svc, {{&w1, {{0, false, false, false}}},
                         {&w2, {{0, true, true, false}}},
                         {&w3, {{0, true, true, true}}}});
        run.run(
            [&](int round, ColdTally& tally) {
                const bool grow = round % 2 == 1;
                std::vector<ControllerDescriptor> padsW2{
                    pad(0, CONTROLLER_TYPE_PLAYSTATION, CAP_MOTION, TOUCHPAD_MODE_DS4)};
                if (grow) padsW2.push_back(pad(1, CONTROLLER_TYPE_XBOX));
                putOrRefuse(svc, w2, padsW2, tally);
                std::vector<ControllerDescriptor> padsW3{
                    pad(0, CONTROLLER_TYPE_DUALSENSE, CAP_MOTION | CAP_MIC, TOUCHPAD_MODE_DS4)};
                if (grow) padsW3.push_back(pad(1, CONTROLLER_TYPE_SWITCHPRO));
                putOrRefuse(svc, w3, padsW3, tally);
                refreshPairedDeviceIdentity(w3.id(), w3.ip(), w3.name());

                const SessionUpsertResult opened =
                    put(svc, w4, {pad(0, CONTROLLER_TYPE_SWITCHPRO)});
                if (!opened.ok) tally.refused++;
                svc.closeSessionById(opened.connectionId, w4.id(), CLOSE_REASON_REPLACED, false);

                if (round % REAP_ROUND_EVERY == 0) {
                    const SessionUpsertResult silent = put(svc, wr, {pad(0, CONTROLLER_TYPE_XBOX)});
                    if (!silent.ok) tally.refused++;
                    svc.backdateForTest(silent.token, REAP_SILENCE_SEC, 1);
                }
            },
            FeedbackTick{});
        svc.closeAllSessions();

        expectTheRunReachedEverything("windows", run, false);
        TEST("windows: the helper provisioned and removed pads");
        EXPECT(fake.plugs() > 0);
        EXPECT_EQ(fake.unplugs(), fake.plugs());
    }
    g_driverWatch = nullptr;
}

#endif

NameSet receiveAcquired(const lockstats::Snapshot& snap) {
    NameSet names;
    for (const lockstats::LockStats& lock : snap.locks) {
        if (lock.role(ThreadRole::Receive).touched()) names.insert(lock.name);
    }
    return names;
}

PairSet pairsOf(const std::vector<lockstats::NamePair>& pairs) {
    PairSet out;
    for (const lockstats::NamePair& pair : pairs) out.insert({pair.first, pair.second});
    return out;
}

uint64_t regionHits(const lockstats::Snapshot& snap, const std::string& region) {
    for (const lockstats::RegionHit& hit : snap.regions) {
        if (hit.region == region) return hit.hits;
    }
    return 0;
}

#ifdef _WIN32
NameSet heldDuring(const lockstats::Snapshot& snap, const std::string& region) {
    NameSet names;
    for (const lockstats::NamePair& pair : snap.blocking) {
        if (pair.first == region) names.insert(pair.second);
    }
    return names;
}
#endif

void expectExactly(const std::string& law, const NameSet& observed,
                   const std::vector<LockName>& listed) {
    TEST(law);
    NameSet names;
    for (const LockName& entry : listed) names.insert(entry.lock);
    for (const std::string& lock : observed) {
        if (names.count(lock) != 0) continue;
        std::cerr << "  " << law << ": " << lock << " is not on the list\n";
        EXPECT(false);
    }
    for (const LockName& entry : listed) {
        if (observed.count(entry.lock) != 0) continue;
        std::cerr << "  " << law << ": " << entry.lock << " (" << allowanceName(entry.why)
                  << ") is listed but no longer observed; delete it\n";
        EXPECT(false);
    }
    EXPECT_EQ(observed.size(), listed.size());
}

void expectExactly(const std::string& law, const PairSet& observed,
                   const std::vector<LockPair>& listed) {
    TEST(law);
    PairSet pairs;
    for (const LockPair& entry : listed) pairs.insert({entry.first, entry.second});
    for (const auto& [first, second] : observed) {
        if (pairs.count({first, second}) != 0) continue;
        std::cerr << "  " << law << ": (" << first << ", " << second << ") is not on the list\n";
        EXPECT(false);
    }
    for (const LockPair& entry : listed) {
        if (observed.count({entry.first, entry.second}) != 0) continue;
        std::cerr << "  " << law << ": (" << entry.first << ", " << entry.second << ") ("
                  << allowanceName(entry.why) << ") is listed but no longer observed; delete it\n";
        EXPECT(false);
    }
    EXPECT_EQ(observed.size(), listed.size());
}

bool findCycle(const PairSet& edges, std::vector<std::string>& cycle) {
    std::map<std::string, std::vector<std::string>> next;
    for (const auto& [from, to] : edges) next[from].push_back(to);
    std::map<std::string, int> state;
    std::vector<std::string> path;
    std::function<bool(const std::string&)> visit = [&](const std::string& node) {
        state[node] = 1;
        path.push_back(node);
        for (const std::string& to : next[node]) {
            if (state[to] == 1) {
                cycle.assign(std::find(path.begin(), path.end(), to), path.end());
                cycle.push_back(to);
                return true;
            }
            if (state[to] == 0 && visit(to)) return true;
        }
        path.pop_back();
        state[node] = 2;
        return false;
    };
    for (const auto& [from, to] : edges) {
        if (state[from] == 0 && visit(from)) return true;
    }
    return false;
}

void checkTheReceiveLaw(const lockstats::Snapshot& snap) {
    expectExactly("receive law: the locks the receive role takes", receiveAcquired(snap),
                  RECEIVE_MAY_ACQUIRE);
}

void checkTheLifecycleLaw(const LifecycleWatch& watch, const lockstats::Snapshot& snap) {
    TEST("lifecycle law: every backend lifecycle call runs inside a topology change");
    for (const std::string& where : watch.unguarded()) {
        std::cerr << "  lifecycle law: " << where << " ran without service.topology\n";
    }
    EXPECT(watch.unguarded().empty());
    expectExactly("lifecycle law: the locks held across backend lifecycle work", watch.held(),
                  LIFECYCLE_MAY_HOLD);
#ifdef _WIN32
    expectExactly("lifecycle law: the locks held while the helper is involved",
                  heldDuring(snap, "helper.pipe"), HELPER_PIPE_MAY_HOLD);
#else
    (void)snap;
#endif
}

void checkTheBlockingLaw(const lockstats::Snapshot& snap) {
    expectExactly("blocking law: the locks held across marked blocking regions",
                  pairsOf(snap.blocking), BLOCKING_MAY_HOLD);
}

void checkTheOrderLaw(const lockstats::Snapshot& snap) {
    const PairSet edges = pairsOf(snap.edges);
    TEST("order law: the nesting graph has no cycle");
    std::vector<std::string> cycle;
    const bool cyclic = findCycle(edges, cycle);
    if (cyclic) {
        std::cerr << "  order law: cycle";
        for (const std::string& lock : cycle) std::cerr << " " << lock;
        std::cerr << "\n";
    }
    EXPECT(!cyclic);
    expectExactly("order law: every nesting edge is in today's order", edges, ORDER_MAY_NEST);
}

void checkTheLoggerLaw(const lockstats::Snapshot& snap) {
    TEST("logger law: the log writer takes no lock");
    for (const lockstats::LockStats& lock : snap.locks) {
        if (!lock.role(ThreadRole::Logger).touched()) continue;
        std::cerr << "  logger law: the Logger role took " << lock.name << "\n";
        EXPECT(false);
    }
    TEST("logger law: the log writer reached its sink, where the blocking law sees what it holds");
    EXPECT(regionHits(snap, "log.write") > 0);
}

double micros(uint64_t nanos) { return static_cast<double>(nanos) / NANOS_PER_MICRO; }

void report(const lockstats::Snapshot& snap) {
    std::cout << "  lock stats (role: acquisitions, contended, failed tries, max wait us, p99 "
                 "wait us, max hold us)\n";
    std::cout << std::fixed << std::setprecision(1);
    for (const lockstats::LockStats& lock : snap.locks) {
        for (size_t r = 0; r < THREAD_ROLE_COUNT; ++r) {
            const lockstats::RoleStats& stats = lock.roles[r];
            if (!stats.touched()) continue;
            std::cout << "    " << lock.name << " / " << threadRoleName(static_cast<ThreadRole>(r))
                      << ": " << stats.acquisitions << ", " << stats.contended << ", "
                      << stats.tryFailures << ", " << micros(stats.waitMaxNs) << ", "
                      << micros(lockstats::histogramPercentileNs(stats.waitHistogram, 0.99)) << ", "
                      << micros(stats.holdMaxNs) << "\n";
        }
    }
    std::cout << "  nesting edges:";
    for (const lockstats::NamePair& edge : snap.edges) {
        std::cout << " (" << edge.first << " -> " << edge.second << ")";
    }
    std::cout << "\n  blocking regions:";
    for (const lockstats::NamePair& pair : snap.blocking) {
        std::cout << " (" << pair.first << " under " << pair.second << ")";
    }
    std::cout << "\n  regions reached:";
    for (const lockstats::RegionHit& hit : snap.regions) {
        std::cout << " " << hit.region << " x" << hit.hits;
    }
    std::cout << "\n";
}

} // namespace

#ifdef _WIN32

HANDLE openVigemBus() {
    checkDriver("vigem openVigemBus");
    return CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

bool pluginTarget(HANDLE, ULONG) {
    checkDriver("vigem pluginTarget");
    std::this_thread::sleep_for(PLUG_TIME);
    return true;
}

bool pluginTargetDS4(HANDLE, ULONG) {
    checkDriver("vigem pluginTargetDS4");
    std::this_thread::sleep_for(PLUG_TIME);
    return true;
}

bool unplugTarget(HANDLE, ULONG) {
    checkDriver("vigem unplugTarget");
    return true;
}

bool submitXusbSync(HANDLE, ULONG, XUSB_SUBMIT_REPORT&, HANDLE, const void*) { return true; }

bool submitDs4Sync(HANDLE, ULONG, DS4_SUBMIT_REPORT&, HANDLE, const DS4_REPORT&) { return true; }

bool submitDs4ExSync(HANDLE, ULONG, DS4_SUBMIT_REPORT_EX&, HANDLE, const DS4_REPORT_EX&) {
    return true;
}

bool waitNextXusbNotification(HANDLE, ULONG, HANDLE cancel, XUSB_REQUEST_NOTIFICATION&) {
    WaitForSingleObject(cancel, INFINITE);
    return false;
}

bool waitNextDS4Notification(HANDLE, ULONG, HANDLE cancel, DS4_REQUEST_NOTIFICATION&) {
    WaitForSingleObject(cancel, INFINITE);
    return false;
}

#endif

int main() {
    if (sodium_init() < 0) {
        std::cerr << "sodium_init failed\n";
        return 1;
    }
    ConfigSandbox sandbox;
    if (!sandbox.ready()) {
        std::cerr << "configPath() could not be pointed at a sandbox directory; refusing to run "
                     "anything that saves the config\n";
        return 1;
    }

    lockstats::reset();
    TEST("logger law: the log writer starts");
    EXPECT(logWriter().start(std::make_unique<DiscardingLogSink>()));
    LifecycleWatch watch;
#ifdef _WIN32
    timeBeginPeriod(1);
#endif
    runTheServiceOnTheMockBackend(watch, sandbox);
#ifdef _WIN32
    runTheServiceOnTheWindowsBackends(watch);
    timeEndPeriod(1);
#endif
    g_configStore.reset();
    logWriter().stop();
    const lockstats::Snapshot snap = lockstats::snapshot();
    report(snap);

    TEST("stats: nothing overflowed the registry");
    EXPECT_EQ(snap.overflows, 0u);
    checkTheReceiveLaw(snap);
    checkTheLifecycleLaw(watch, snap);
    checkTheBlockingLaw(snap);
    checkTheOrderLaw(snap);
    checkTheLoggerLaw(snap);

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
