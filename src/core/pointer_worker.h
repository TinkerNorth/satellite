// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include "core/hot_table.h"
#include "core/mpsc_queue.h"
#include "core/ports.h"
#include "core/spsc_ring.h"
#include "core/worker_wake.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>

inline constexpr size_t POINTER_FRAME_CAPACITY = 256;
inline constexpr size_t POINTER_COMMAND_CAPACITY = 256;

struct PointerFrame {
    uint32_t serial = 0;
    uint32_t stamp = 0;
    int slot = -1;
    uint32_t row = 0;
    TouchpadReport report{};
};

struct PointerCommand {
    enum class Kind : uint8_t { Release, Retry, BackdateRefusalLog };
    Kind kind = Kind::Retry;
    uint32_t serial = 0;
    uint32_t stamp = 0;
    int slot = -1;
    uint32_t row = 0;
    int seconds = 0;
};

struct PointerMotion {
    int dx = 0;
    int dy = 0;
    int wheel = 0;
    MouseButtons buttons;
    bool buttonsChanged = false;
};

PointerMotion touchpadPointerMotion(const TouchpadReport& prev, bool prevValid,
                                    const TouchpadReport& report, float& remX, float& remY);

class PointerWorker {
  public:
    PointerWorker(IGamepadPort& backend, ILogPort& log, const HotTable& hot);
    ~PointerWorker();
    PointerWorker(const PointerWorker&) = delete;
    PointerWorker& operator=(const PointerWorker&) = delete;

    bool pushFrame(const PointerFrame& frame);
    void post(const PointerCommand& command);
    bool retryPending() const;
    PointerCounts counts() const;

#ifdef SATELLITE_BUILD_TESTS
    bool drainForTest();
    std::thread::id threadIdForTest() const;
#endif

  private:
    struct Pad {
        MouseButtons buttons;
        uint32_t releaseStamp = 0;
        uint32_t stamp = 0;
        TouchpadReport prev{};
        bool prevValid = false;
        float remX = 0.0f;
        float remY = 0.0f;
    };
    struct Session {
        uint32_t row = 0;
        uint32_t refusedRun = 0;
        bool refusalLogged = false;
        std::chrono::steady_clock::time_point refusalLoggedAt{};
    };

    void run();
    bool hasWork() const;
    bool drainCommands();
    void process(const PointerFrame& frame);
    void apply(const PointerCommand& command);
    bool submit(int dx, int dy, int wheel);
    void record(int slot, uint32_t row, bool ok);
    Session& session(int slot, uint32_t row);
    bool deviceName(int slot, uint32_t row, std::string& out) const;

    IGamepadPort& backend_;
    ILogPort& log_;
    const HotTable& hot_;
    satellite::SpscRing<PointerFrame, POINTER_FRAME_CAPACITY> frames_;
    satellite::MpscQueue<PointerCommand, POINTER_COMMAND_CAPACITY> commands_;
    satellite::WorkerWake wake_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> retryPending_{false};
    std::atomic<uint64_t> injected_{0};
    std::atomic<uint64_t> refused_{0};
    std::atomic<uint64_t> framesDone_{0};
    std::atomic<uint64_t> commandsDone_{0};
    std::atomic<bool> lastFrameOk_{false};
#ifdef SATELLITE_BUILD_TESTS
    uint64_t framesAtLastDrain_ = 0;
#endif
    std::array<Pad, MAX_BACKEND_CONTROLLERS + 1> pads_{};
    std::array<Session, MAX_SESSIONS> sessions_{};
    std::thread thread_;
};
