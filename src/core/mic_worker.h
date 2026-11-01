// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include "core/audio/audio_codec.h"
#include "core/audio/audio_jitter.h"
#include "core/hot_table.h"
#include "core/ports.h"
#include "core/spsc_ring.h"
#include "core/worker_wake.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

inline constexpr size_t MIC_PACKET_CAPACITY = 64;
inline constexpr size_t MIC_PACKET_MAX_BYTES = static_cast<size_t>(AUDIO_JITTER_MAX_PACKET_BYTES);

struct MicPacket {
    uint32_t serial = 0;
    uint32_t gen = 0;
    int slot = -1;
    uint8_t ctrlIdx = 0;
    uint32_t keyEpoch = 0;
    uint16_t seq = 0;
    uint16_t length = 0;
    uint8_t bytes[MIC_PACKET_MAX_BYTES] = {};
};

struct MicCounts {
    uint64_t accepted = 0;
    uint64_t late = 0;
    uint64_t decoded = 0;
    uint64_t fecRecovered = 0;
    uint64_t concealed = 0;
};

class MicWorker {
  public:
    MicWorker(IGamepadPort& backend, ILogPort& log, const HotTable& hot, RxEpoch& rx,
              IAudioCodecFactory* codecs);
    ~MicWorker();
    MicWorker(const MicWorker&) = delete;
    MicWorker& operator=(const MicWorker&) = delete;

    MicPacket* claim();
    void publish();
    void noteRejected();
    MicCounts counts() const;

#ifdef SATELLITE_BUILD_TESTS
    bool drainForTest();
    std::thread::id threadIdForTest() const;
#endif

  private:
    struct Pad {
        uint32_t gen = 0;
        int slot = -1;
        uint8_t ctrlIdx = 0;
        AudioJitterWindow window;
        std::unique_ptr<IAudioDecoder> decoder;
    };
    struct Session {
        uint32_t keyEpoch = 0;
        bool noSinkLogged = false;
    };
    enum class Binding { Bound, Unbound, Changing };
    enum class Delivery { Submitted, NoSink, Unbound };

    void run();
    bool hasWork() const;
    void process(const MicPacket& packet);
    Delivery submit(const MicPacket& packet, const int16_t* pcm, size_t frames);
    Binding binding(int slot, uint8_t ctrlIdx, uint32_t serial, uint32_t gen) const;
    void sweep();
    void noteNoSink(const MicPacket& packet);
    bool deviceName(int slot, uint32_t keyEpoch, std::string& out) const;

    IGamepadPort& backend_;
    ILogPort& log_;
    const HotTable& hot_;
    RxEpoch& rx_;
    IAudioCodecFactory* codecs_;
    std::unique_ptr<satellite::SpscRing<MicPacket, MIC_PACKET_CAPACITY>> packets_ =
        std::make_unique<satellite::SpscRing<MicPacket, MIC_PACKET_CAPACITY>>();
    satellite::WorkerWake wake_;
    std::atomic<bool> stopping_{false};
    std::atomic<uint64_t> accepted_{0};
    std::atomic<uint64_t> late_{0};
    std::atomic<uint64_t> decoded_{0};
    std::atomic<uint64_t> fecRecovered_{0};
    std::atomic<uint64_t> concealed_{0};
    std::atomic<uint64_t> packetsDone_{0};
    std::atomic<bool> lastAccepted_{false};
    std::atomic<uint64_t> sweepsAsked_{0};
    std::atomic<uint64_t> sweepsDone_{0};
#ifdef SATELLITE_BUILD_TESTS
    uint64_t packetsAtLastDrain_ = 0;
#endif
    std::array<Pad, MAX_BACKEND_CONTROLLERS + 1> pads_{};
    std::array<Session, MAX_SESSIONS> sessions_{};
    std::thread thread_;
};
