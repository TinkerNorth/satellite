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
    return static_cast<uint32_t>(caps) | (static_cast<uint32_t>(type) << 16U) |
           (static_cast<uint32_t>(touchpadMode) << 24U);
}

inline constexpr uint16_t hotPadCaps(uint32_t info) { return static_cast<uint16_t>(info); }

inline constexpr uint8_t hotPadType(uint32_t info) { return static_cast<uint8_t>(info >> 16U); }

inline constexpr uint8_t hotPadMode(uint32_t info) { return static_cast<uint8_t>(info >> 24U); }

inline constexpr uint32_t hotSessionWord(uint16_t epoch, bool mouseGranted) {
    return static_cast<uint32_t>(epoch) | (mouseGranted ? HOT_SESSION_MOUSE : 0u);
}

inline constexpr uint16_t hotSessionEpoch(uint32_t word) { return static_cast<uint16_t>(word); }

inline constexpr bool hotSessionMouse(uint32_t word) { return (word & HOT_SESSION_MOUSE) != 0; }

inline constexpr uint32_t hotPadOwner(int slot, uint8_t ctrlIdx) {
    return (static_cast<uint32_t>(slot + 1) << 8U) | ctrlIdx;
}

inline constexpr int hotOwnerSlot(uint32_t owner) { return static_cast<int>(owner >> 8U) - 1; }

inline constexpr uint8_t hotOwnerIndex(uint32_t owner) { return static_cast<uint8_t>(owner); }

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
    return (static_cast<uint64_t>(ipv4NetworkOrder) << 16U) | port;
}

inline constexpr uint32_t clientAddrIPv4(uint64_t addr) {
    return static_cast<uint32_t>(addr >> 16U);
}

inline constexpr uint16_t clientAddrPort(uint64_t addr) { return static_cast<uint16_t>(addr); }

inline int64_t hotTicks(std::chrono::steady_clock::time_point at) {
    return static_cast<int64_t>(at.time_since_epoch().count());
}

inline std::chrono::steady_clock::time_point hotTime(int64_t ticks) {
    return std::chrono::steady_clock::time_point(
        std::chrono::steady_clock::duration(static_cast<std::chrono::steady_clock::rep>(ticks)));
}

// One cache line. The structs below that keep writers on separate lines
// spell their padding out as members, so the compiler adds none of its own:
// the layout is the one the alignment asks for, and MSVC has no padding to
// warn about (C4324). The static_asserts after each struct pin it.
inline constexpr size_t HOT_CACHE_LINE = 64;

struct alignas(HOT_CACHE_LINE) HotSlot {
    std::atomic<uint32_t> gen{0};
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

    // The identity block above fills seven lines exactly; the replay block
    // starts the eighth.
    std::atomic<uint32_t> replayKeyEpoch{0};
    std::atomic<uint32_t> lastCounter{0};
    std::atomic<int64_t> lastPacketTicks{0};
    std::atomic<uint64_t> clientAddr{0};
    std::atomic<uint32_t> txCounter{0};
    std::atomic<bool> feedbackPending{false};
    std::byte replayLinePad_[35]{};

    std::atomic<uint32_t> txGate{0};
    std::atomic<uint64_t> txDeferredAck{0};
    std::byte txLinePad_[48]{};

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
        for (size_t i = 0; i < HOT_NAME_WORDS; i++) {
            name[i].store(raw[i], std::memory_order_relaxed);
        }
        nameLength.store(static_cast<uint32_t>(length), std::memory_order_relaxed);
    }

    std::string loadName() const {
        uint64_t raw[HOT_NAME_WORDS] = {};
        for (size_t i = 0; i < HOT_NAME_WORDS; i++) {
            raw[i] = name[i].load(std::memory_order_relaxed);
        }
        const size_t length =
            std::min<size_t>(nameLength.load(std::memory_order_relaxed), HOT_NAME_BYTES);
        return {reinterpret_cast<const char*>(raw), length};
    }
};

inline constexpr uint32_t RX_PAD_MOTION = 1u << 0U;
inline constexpr uint32_t RX_PAD_MOTION_SINK = 1u << 1U;
inline constexpr uint32_t RX_PAD_BATTERY = 1u << 2U;
inline constexpr uint32_t RX_PAD_TOUCHPAD = 1u << 3U;
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

struct alignas(HOT_CACHE_LINE) RxPad {
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
    std::byte linePad_[46]{};

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
        out.battery.status = static_cast<uint8_t>(level >> 8U);
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

inline constexpr size_t RX_RECEIVE_PARTICIPANT = 0;
inline constexpr size_t RX_MIC_PARTICIPANT = 1;
inline constexpr size_t RX_AUDIO_LANES = 2;
inline constexpr size_t RX_PARTICIPANTS =
    2 + RX_AUDIO_LANES * static_cast<size_t>(MAX_BACKEND_CONTROLLERS);

inline constexpr size_t rxAudioParticipant(uint32_t serial, size_t lane) {
    return 2 + (static_cast<size_t>(serial) - 1) * RX_AUDIO_LANES + lane;
}

class alignas(HOT_CACHE_LINE) RxEpoch {
  public:
    explicit RxEpoch(ProcessBarrierFn barrier = nullptr) : own_(barrier) {}

    void enter() {
        own_.epoch.store(own_.epoch.load(std::memory_order_relaxed) + 1, std::memory_order_release);
        std::atomic_signal_fence(std::memory_order_seq_cst);
        if (own_.needFence) std::atomic_thread_fence(std::memory_order_seq_cst);
    }

    void exit() {
        std::atomic_signal_fence(std::memory_order_seq_cst);
        own_.epoch.store(own_.epoch.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    }

    void enter(size_t participant) {
        std::atomic<uint64_t>& epoch = epochOf(participant);
        epoch.store(epoch.load(std::memory_order_relaxed) + 1, std::memory_order_release);
        std::atomic_signal_fence(std::memory_order_seq_cst);
        if (own_.needFence) std::atomic_thread_fence(std::memory_order_seq_cst);
    }

    void exit(size_t participant) {
        std::atomic_signal_fence(std::memory_order_seq_cst);
        std::atomic<uint64_t>& epoch = epochOf(participant);
        epoch.store(epoch.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    }

    void waitQuiescent() const {
        if (own_.barrier != nullptr) {
            own_.barrier();
        } else {
            std::atomic_thread_fence(std::memory_order_seq_cst);
        }
        waitOut(own_.epoch);
        for (const Participant& participant : others_) waitOut(participant.epoch);
    }

    uint64_t value() const { return own_.epoch.load(std::memory_order_acquire); }

    uint64_t value(size_t participant) const {
        return participant == RX_RECEIVE_PARTICIPANT
                   ? own_.epoch.load(std::memory_order_acquire)
                   : others_[participant - 1].epoch.load(std::memory_order_acquire);
    }

  private:
    struct alignas(HOT_CACHE_LINE) Participant {
        std::atomic<uint64_t> epoch{0};
        std::byte linePad_[56]{};
    };
    static_assert(sizeof(Participant) == HOT_CACHE_LINE);

    static void waitOut(const std::atomic<uint64_t>& epoch) {
        const uint64_t seen = epoch.load(std::memory_order_acquire);
        if ((seen & 1u) == 0) return;
        while (epoch.load(std::memory_order_acquire) == seen) std::this_thread::yield();
    }

    std::atomic<uint64_t>& epochOf(size_t participant) {
        return participant == RX_RECEIVE_PARTICIPANT ? own_.epoch : others_[participant - 1].epoch;
    }

    // The receive thread's own line: its epoch and the settings beside it.
    struct alignas(HOT_CACHE_LINE) Own {
        explicit Own(ProcessBarrierFn processBarrier)
            : barrier(processBarrier), needFence(processBarrier == nullptr) {}
        std::atomic<uint64_t> epoch{0};
        const ProcessBarrierFn barrier;
        const bool needFence;
        std::byte linePad_[47]{};
    };
    static_assert(sizeof(Own) == HOT_CACHE_LINE);

    Own own_;
    std::array<Participant, RX_PARTICIPANTS - 1> others_{};
};

static_assert(std::is_standard_layout_v<HotSlot>);
static_assert(offsetof(HotSlot, replayKeyEpoch) == 7 * HOT_CACHE_LINE);
static_assert(offsetof(HotSlot, txGate) == 8 * HOT_CACHE_LINE);
static_assert(sizeof(HotSlot) == 9 * HOT_CACHE_LINE);
static_assert(sizeof(RxPad) == 2 * HOT_CACHE_LINE);
static_assert(sizeof(RxEpoch) == (1 + (RX_PARTICIPANTS - 1)) * HOT_CACHE_LINE);

class HotTable {
  public:
    std::array<std::atomic<uint32_t>, MAX_SESSIONS> tokens{};
    std::array<HotSlot, MAX_SESSIONS> slots;
    std::array<std::atomic<uint32_t>, MAX_BACKEND_CONTROLLERS + 1> padOwners{};

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
