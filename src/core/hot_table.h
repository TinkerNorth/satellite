// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include "core/types.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <type_traits>

using ProcessBarrierFn = void (*)();

inline constexpr uint32_t HOT_CTRL_ACTIVE = 0x80000000u;
inline constexpr uint32_t HOT_CTRL_SERIAL_MASK = 0x000000FFu;
inline constexpr size_t HOT_KEY_WORDS = static_cast<size_t>(CRYPTO_KEY_SIZE) / sizeof(uint64_t);
inline constexpr size_t HOT_NAME_WORDS = 16;
inline constexpr size_t HOT_NAME_BYTES = HOT_NAME_WORDS * sizeof(uint64_t);
inline constexpr uint32_t HOT_SESSION_MOUSE = 0x00010000u;

inline constexpr uint32_t hotCtrlWord(bool active, uint32_t serial) {
    return active ? HOT_CTRL_ACTIVE | (serial & HOT_CTRL_SERIAL_MASK) : 0u;
}

inline constexpr uint32_t hotPadInfo(uint16_t caps, uint8_t type, uint8_t touchpadMode) {
    return static_cast<uint32_t>(caps) | (static_cast<uint32_t>(type) << 16) |
           (static_cast<uint32_t>(touchpadMode) << 24);
}

inline constexpr uint16_t hotPadCaps(uint32_t info) { return static_cast<uint16_t>(info); }

inline constexpr uint8_t hotPadType(uint32_t info) { return static_cast<uint8_t>(info >> 16); }

inline constexpr uint8_t hotPadMode(uint32_t info) { return static_cast<uint8_t>(info >> 24); }

inline constexpr uint32_t hotSessionWord(uint16_t epoch, bool mouseGranted) {
    return static_cast<uint32_t>(epoch) | (mouseGranted ? HOT_SESSION_MOUSE : 0u);
}

inline constexpr uint16_t hotSessionEpoch(uint32_t word) { return static_cast<uint16_t>(word); }

inline constexpr bool hotSessionMouse(uint32_t word) { return (word & HOT_SESSION_MOUSE) != 0; }

template <typename T, size_t N> void storeWords(std::atomic<uint64_t> (&words)[N], const T& value) {
    static_assert(std::is_trivially_copyable_v<T> && sizeof(T) <= N * sizeof(uint64_t));
    uint64_t raw[N] = {};
    std::memcpy(raw, &value, sizeof(T));
    for (size_t i = 0; i < N; i++) words[i].store(raw[i], std::memory_order_relaxed);
}

template <typename T, size_t N> T loadWords(const std::atomic<uint64_t> (&words)[N]) {
    static_assert(std::is_trivially_copyable_v<T> && sizeof(T) <= N * sizeof(uint64_t));
    uint64_t raw[N] = {};
    for (size_t i = 0; i < N; i++) raw[i] = words[i].load(std::memory_order_relaxed);
    T value{};
    std::memcpy(&value, raw, sizeof(T));
    return value;
}

inline size_t utf8Prefix(const std::string& text, size_t limit) {
    if (text.size() <= limit) return text.size();
    size_t cut = limit;
    while (cut > 0 && (static_cast<uint8_t>(text[cut]) & 0xC0u) == 0x80u) cut--;
    return cut;
}

inline constexpr uint64_t packClientAddr(uint32_t ipv4NetworkOrder, uint16_t port) {
    return (static_cast<uint64_t>(ipv4NetworkOrder) << 16) | port;
}

inline constexpr uint32_t clientAddrIPv4(uint64_t addr) {
    return static_cast<uint32_t>(addr >> 16);
}

inline constexpr uint16_t clientAddrPort(uint64_t addr) { return static_cast<uint16_t>(addr); }

inline int64_t hotTicks(std::chrono::steady_clock::time_point at) {
    return static_cast<int64_t>(at.time_since_epoch().count());
}

inline std::chrono::steady_clock::time_point hotTime(int64_t ticks) {
    return std::chrono::steady_clock::time_point(
        std::chrono::steady_clock::duration(static_cast<std::chrono::steady_clock::rep>(ticks)));
}

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

struct HotSlot {
    alignas(64) std::atomic<uint32_t> gen{0};
    std::atomic<uint32_t> token{0};
    std::atomic<uint32_t> keyEpoch{0};
    std::atomic<uint64_t> key[HOT_KEY_WORDS]{};
    std::atomic<uint32_t> ctrl[MAX_CONTROLLERS_PER_CONN]{};
    std::atomic<uint32_t> padInfo[MAX_CONTROLLERS_PER_CONN]{};
    std::atomic<uint32_t> padGen[MAX_CONTROLLERS_PER_CONN]{};
    std::atomic<uint32_t> padStamp[MAX_CONTROLLERS_PER_CONN]{};
    std::atomic<uint32_t> session{0};
    std::atomic<uint32_t> row{0};
    std::atomic<uint32_t> nameLength{0};
    std::atomic<uint64_t> name[HOT_NAME_WORDS]{};

    alignas(64) std::atomic<uint32_t> replayKeyEpoch{0};
    std::atomic<uint32_t> lastCounter{0};
    std::atomic<int64_t> lastPacketTicks{0};
    std::atomic<uint64_t> clientAddr{0};
    std::atomic<uint32_t> txCounter{0};
    std::atomic<bool> feedbackPending{false};

    alignas(64) std::atomic<uint32_t> txGate{0};
    std::atomic<uint64_t> txDeferredAck{0};

    void beginWrite() {
        gen.fetch_add(1, std::memory_order_acq_rel);
        std::atomic_thread_fence(std::memory_order_release);
    }

    void endWrite() { gen.fetch_add(1, std::memory_order_release); }

    bool writing() const { return (gen.load(std::memory_order_relaxed) & 1u) != 0; }

    template <typename Load> bool read(Load&& load) const {
        const uint32_t before = gen.load(std::memory_order_acquire);
        if ((before & 1u) != 0) return false;
        load();
        std::atomic_thread_fence(std::memory_order_acquire);
        return gen.load(std::memory_order_relaxed) == before;
    }

    void storeKey(const uint8_t bytes[CRYPTO_KEY_SIZE]) {
        for (size_t i = 0; i < HOT_KEY_WORDS; i++) {
            uint64_t word = 0;
            std::memcpy(&word, bytes + i * sizeof(word), sizeof(word));
            key[i].store(word, std::memory_order_relaxed);
        }
    }

    void loadKey(uint8_t out[CRYPTO_KEY_SIZE]) const {
        for (size_t i = 0; i < HOT_KEY_WORDS; i++) {
            const uint64_t word = key[i].load(std::memory_order_relaxed);
            std::memcpy(out + i * sizeof(word), &word, sizeof(word));
        }
    }

    void storeName(const std::string& text) {
        const size_t length = utf8Prefix(text, HOT_NAME_BYTES);
        uint64_t raw[HOT_NAME_WORDS] = {};
        std::memcpy(raw, text.data(), length);
        for (size_t i = 0; i < HOT_NAME_WORDS; i++)
            name[i].store(raw[i], std::memory_order_relaxed);
        nameLength.store(static_cast<uint32_t>(length), std::memory_order_relaxed);
    }

    std::string loadName() const {
        uint64_t raw[HOT_NAME_WORDS] = {};
        for (size_t i = 0; i < HOT_NAME_WORDS; i++)
            raw[i] = name[i].load(std::memory_order_relaxed);
        const size_t length =
            std::min<size_t>(nameLength.load(std::memory_order_relaxed), HOT_NAME_BYTES);
        return std::string(reinterpret_cast<const char*>(raw), length);
    }
};

inline constexpr uint32_t RX_PAD_MOTION = 1u << 0;
inline constexpr uint32_t RX_PAD_MOTION_SINK = 1u << 1;
inline constexpr uint32_t RX_PAD_BATTERY = 1u << 2;
inline constexpr uint32_t RX_PAD_TOUCHPAD = 1u << 3;
inline constexpr size_t RX_MOTION_WORDS = (sizeof(MotionReport) + 7) / 8;
inline constexpr size_t RX_TOUCHPAD_WORDS = (sizeof(TouchpadReport) + 7) / 8;

struct RxPadView {
    uint32_t gen = 0;
    uint32_t flags = 0;
    uint8_t touchpadMode = 0;
    uint32_t touchpadKeyEpoch = 0;
    MotionReport motion{};
    BatteryReport battery{};
    TouchpadReport touchpad{};
};

struct alignas(64) RxPad {
    std::atomic<uint32_t> seq{0};
    std::atomic<uint32_t> gen{0};
    std::atomic<uint32_t> flags{0};
    std::atomic<uint32_t> touchpadMode{0};
    std::atomic<uint32_t> touchpadKeyEpoch{0};
    std::atomic<uint32_t> battery{0};
    std::atomic<uint64_t> motion[RX_MOTION_WORDS]{};
    std::atomic<uint64_t> touchpad[RX_TOUCHPAD_WORDS]{};

    uint32_t ownGen = 0;
    uint32_t ownFlags = 0;
    int64_t micWindowStart = 0;
    uint16_t micPacketsInWindow = 0;

    void beginWrite() {
        seq.store(seq.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
    }

    void endWrite() {
        seq.store(seq.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    }

    bool read(RxPadView& out) const {
        const uint32_t before = seq.load(std::memory_order_acquire);
        if ((before & 1u) != 0) return false;
        out.gen = gen.load(std::memory_order_relaxed);
        out.flags = flags.load(std::memory_order_relaxed);
        out.touchpadMode = static_cast<uint8_t>(touchpadMode.load(std::memory_order_relaxed));
        out.touchpadKeyEpoch = touchpadKeyEpoch.load(std::memory_order_relaxed);
        const uint32_t level = battery.load(std::memory_order_relaxed);
        out.battery.level = static_cast<uint8_t>(level);
        out.battery.status = static_cast<uint8_t>(level >> 8);
        out.motion = loadWords<MotionReport>(motion);
        out.touchpad = loadWords<TouchpadReport>(touchpad);
        std::atomic_thread_fence(std::memory_order_acquire);
        return seq.load(std::memory_order_relaxed) == before;
    }
};

struct RxSession {
    uint32_t keyEpoch = 0;
    uint8_t micDropLogged = 0;
};

class alignas(64) RxEpoch {
  public:
    explicit RxEpoch(ProcessBarrierFn barrier = nullptr)
        : barrier_(barrier), needFence_(barrier == nullptr) {}

    void enter() {
        epoch_.store(epoch_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
        std::atomic_signal_fence(std::memory_order_seq_cst);
        if (needFence_) std::atomic_thread_fence(std::memory_order_seq_cst);
    }

    void exit() {
        std::atomic_signal_fence(std::memory_order_seq_cst);
        epoch_.store(epoch_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    }

    void waitQuiescent() const {
        if (barrier_ != nullptr) {
            barrier_();
        } else {
            std::atomic_thread_fence(std::memory_order_seq_cst);
        }
        const uint64_t seen = epoch_.load(std::memory_order_acquire);
        if ((seen & 1u) == 0) return;
        while (epoch_.load(std::memory_order_acquire) == seen) std::this_thread::yield();
    }

    uint64_t value() const { return epoch_.load(std::memory_order_acquire); }

  private:
    std::atomic<uint64_t> epoch_{0};
    const ProcessBarrierFn barrier_;
    const bool needFence_;
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

class HotTable {
  public:
    std::array<std::atomic<uint32_t>, MAX_SESSIONS> tokens{};
    std::array<HotSlot, MAX_SESSIONS> slots;

    int find(uint32_t token) const {
        if (token == 0) return -1;
        for (size_t i = 0; i < tokens.size(); i++) {
            if (tokens[i].load(std::memory_order_relaxed) == token) return static_cast<int>(i);
        }
        return -1;
    }

    int findFree() const {
        for (size_t i = 0; i < tokens.size(); i++) {
            const bool unpublished = tokens[i].load(std::memory_order_relaxed) == 0;
            if (unpublished && !slots[i].writing()) return static_cast<int>(i);
        }
        return -1;
    }

    void publish(int index, uint32_t token) {
        tokens[static_cast<size_t>(index)].store(token, std::memory_order_release);
    }

    void retire(int index) {
        tokens[static_cast<size_t>(index)].store(0, std::memory_order_release);
    }
};

class HotWrite {
  public:
    HotWrite(HotSlot& slot, const RxEpoch& rx) : slot_(slot) {
        slot_.beginWrite();
        rx.waitQuiescent();
    }
    ~HotWrite() { close(); }
    HotWrite(const HotWrite&) = delete;
    HotWrite& operator=(const HotWrite&) = delete;

    void close() {
        if (!open_) return;
        open_ = false;
        slot_.endWrite();
    }

  private:
    HotSlot& slot_;
    bool open_ = true;
};
