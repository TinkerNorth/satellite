// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include "core/log_queue.h"
#include "core/log_record.h"
#include "core/log_ring.h"
#include "core/types.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <semaphore>
#include <string>
#include <string_view>
#include <thread>

namespace satellite {

#if defined(_WIN32)
inline constexpr std::string_view LOG_LINE_END = "\r\n";
#else
inline constexpr std::string_view LOG_LINE_END = "\n";
#endif

inline constexpr size_t LOG_LINE_CAPACITY = 640;
inline constexpr uint64_t LOG_WAKE_DEPTH = LOG_QUEUE_CAPACITY / 4;
inline constexpr size_t LOG_BATCH_RESERVE = 64 * 1024;

class ILogSink {
  public:
    virtual ~ILogSink() = default;

    virtual bool write(std::string_view batch) = 0;
    virtual bool flush() = 0;
    virtual bool rotateIfDue() = 0;
    virtual void purge() = 0;
};

class LogLineFormatter {
  public:
    size_t format(const LogRecord& record, char* out, size_t capacity);

  private:
    int64_t cachedSecond_ = std::numeric_limits<int64_t>::min();
    std::array<char, 24> stamp_{};
    size_t stampLength_ = 0;
};

struct LogWriterTiming {
    std::chrono::milliseconds idleWait{100};
    std::chrono::milliseconds flushInterval{1000};
};

class LogWriter {
  public:
    explicit LogWriter(LogWriterTiming timing = LogWriterTiming{});
    ~LogWriter();

    LogWriter(const LogWriter&) = delete;
    LogWriter& operator=(const LogWriter&) = delete;

    void log(LogLevel level, std::string_view source, std::string_view message) noexcept;
    bool submit(const LogRecord& record) noexcept;

    bool start(std::unique_ptr<ILogSink> sink);
    void stop();
    bool flush(std::chrono::milliseconds timeout);
    bool running() const noexcept;

    const LogRing& recent() const noexcept { return ring_; }
    uint64_t dropped() const noexcept { return queue_.dropped(); }

  private:
    void run();
    bool pass(bool stopping);
    void take(const LogRecord& record, bool& urgentLine);
    void takeDropLine(uint64_t count, bool& urgentLine);
    void sleepUntilWoken();
    void wakeIfSleeping() noexcept;
    bool flushPending() const noexcept;
    bool covered(uint64_t target) const noexcept;
    void acknowledge();

    const LogWriterTiming timing_;
    LogQueue queue_;
    LogRing ring_;
    std::unique_ptr<ILogSink> sink_;
    std::thread thread_;
    std::counting_semaphore<> wake_{0};
    std::counting_semaphore<> flushAck_{0};
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> sleeping_{false};
    std::atomic<bool> urgent_{false};
    std::atomic<uint64_t> flushTarget_{0};
    std::atomic<uint64_t> durable_{0};
    std::atomic<uint64_t> lostFrom_{std::numeric_limits<uint64_t>::max()};
    std::atomic<int> flushWaiters_{0};

    LogLineFormatter formatter_;
    std::string batch_;
    uint64_t lastDropped_ = 0;
    uint64_t handledFlushTarget_ = 0;
    uint64_t acknowledgedDurable_ = 0;
    uint64_t acknowledgedLost_ = std::numeric_limits<uint64_t>::max();
    bool unflushed_ = false;
    std::chrono::steady_clock::time_point lastFlush_{};
};

} // namespace satellite
