// SPDX-License-Identifier: LGPL-3.0-or-later
#include "core/log_writer.h"

#include "core/tracked_mutex.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <system_error>
#include <utility>

namespace satellite {

namespace {

constexpr int64_t NANOS_PER_SECOND = 1'000'000'000;
constexpr int64_t NANOS_PER_MILLI = 1'000'000;
constexpr int64_t MILLIS_PER_SECOND = 1'000;
constexpr size_t MILLI_DIGITS = 3;
constexpr std::string_view UNKNOWN_STAMP = "0000-00-00T00:00:00";
constexpr std::string_view DROP_SOURCE = "log";

thread_local const LogWriter* tl_writer = nullptr;

std::string_view levelWord(LogLevel level) {
    switch (level) {
    case LogLevel::INFO:
        return "INFO ";
    case LogLevel::WARN:
        return "WARN ";
    case LogLevel::ERR:
        return "ERROR";
    }
    return "INFO ";
}

bool localTime(std::time_t when, std::tm& out) {
#if defined(_WIN32)
    return localtime_s(&out, &when) == 0;
#else
    return localtime_r(&when, &out) != nullptr;
#endif
}

class LineBuilder {
  public:
    LineBuilder(char* out, size_t capacity) : out_(out), capacity_(capacity) {}

    void append(std::string_view text) {
        const size_t n = std::min(text.size(), capacity_ - size_);
        if (n != 0) std::memcpy(out_ + size_, text.data(), n);
        size_ += n;
    }

    void appendDigits(int64_t value, size_t width) {
        std::array<char, 20> digits{};
        const size_t count = std::min(width, digits.size());
        for (size_t i = count; i-- > 0;) {
            digits[i] = static_cast<char>('0' + value % 10);
            value /= 10;
        }
        append(std::string_view(digits.data(), count));
    }

    size_t size() const { return size_; }

  private:
    char* const out_;
    const size_t capacity_;
    size_t size_ = 0;
};

void raiseTo(std::atomic<uint64_t>& value, uint64_t target) {
    uint64_t seen = value.load(std::memory_order_seq_cst);
    while (seen < target && !value.compare_exchange_weak(seen, target, std::memory_order_seq_cst)) {
    }
}

void lowerTo(std::atomic<uint64_t>& value, uint64_t target) {
    uint64_t seen = value.load(std::memory_order_seq_cst);
    while (seen > target && !value.compare_exchange_weak(seen, target, std::memory_order_seq_cst)) {
    }
}

} // namespace

size_t LogLineFormatter::format(const LogRecord& record, char* out, size_t capacity) {
    const int64_t ns = std::max<int64_t>(record.timestampNs, 0);
    const int64_t second = ns / NANOS_PER_SECOND;
    if (second != cachedSecond_) {
        std::tm tm{};
        stampLength_ = 0;
        if (localTime(static_cast<std::time_t>(second), tm)) {
            stampLength_ = std::strftime(stamp_.data(), stamp_.size(), "%Y-%m-%dT%H:%M:%S", &tm);
        }
        if (stampLength_ == 0) {
            std::memcpy(stamp_.data(), UNKNOWN_STAMP.data(), UNKNOWN_STAMP.size());
            stampLength_ = UNKNOWN_STAMP.size();
        }
        cachedSecond_ = second;
    }
    LineBuilder line(out, capacity);
    line.append(std::string_view(stamp_.data(), stampLength_));
    line.append(".");
    line.appendDigits((ns / NANOS_PER_MILLI) % MILLIS_PER_SECOND, MILLI_DIGITS);
    line.append(" [");
    line.append(levelWord(record.level));
    line.append("] [");
    line.append(record.sourceText());
    line.append("] ");
    line.append(record.messageText());
    line.append(LOG_LINE_END);
    return line.size();
}

LogWriter::LogWriter(LogWriterTiming timing) : timing_(timing) {}

LogWriter::~LogWriter() { stop(); }

void LogWriter::log(LogLevel level, std::string_view source, std::string_view message) noexcept {
    submit(makeLogRecord(logClockNowNs(), level, source, message));
}

bool LogWriter::submit(const LogRecord& record) noexcept {
    const bool pushed = queue_.push(record);
    const bool urgent = record.level != LogLevel::INFO;
    if (urgent) urgent_.store(true, std::memory_order_seq_cst);
    if (urgent || !pushed || queue_.depth() >= LOG_WAKE_DEPTH) wakeIfSleeping();
    return pushed;
}

bool LogWriter::start(std::unique_ptr<ILogSink> sink) {
    if (!sink || thread_.joinable()) return false;
    sink_ = std::move(sink);
    stopping_.store(false, std::memory_order_seq_cst);
    running_.store(true, std::memory_order_seq_cst);
    try {
        thread_ = std::thread([this] { run(); });
    } catch (const std::system_error&) {
        running_.store(false, std::memory_order_seq_cst);
        sink_.reset();
        return false;
    }
    return true;
}

void LogWriter::stop() {
    if (!thread_.joinable()) return;
    stopping_.store(true, std::memory_order_seq_cst);
    wake_.release();
    thread_.join();
    sink_.reset();
    running_.store(false, std::memory_order_seq_cst);
    const int waiting = flushWaiters_.load(std::memory_order_seq_cst);
    if (waiting > 0) flushAck_.release(waiting);
}

bool LogWriter::flush(std::chrono::milliseconds timeout) {
    if (!running() || tl_writer == this) return false;
    const uint64_t target = queue_.enqueued();
    if (covered(target)) return true;
    if (lostFrom_.load(std::memory_order_seq_cst) < target) return false;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    raiseTo(flushTarget_, target);
    wakeIfSleeping();
    flushWaiters_.fetch_add(1, std::memory_order_seq_cst);
    bool done = covered(target);
    while (!done && running() && lostFrom_.load(std::memory_order_seq_cst) >= target &&
           flushAck_.try_acquire_until(deadline)) {
        done = covered(target);
    }
    flushWaiters_.fetch_sub(1, std::memory_order_seq_cst);
    return done || covered(target);
}

bool LogWriter::running() const noexcept { return running_.load(std::memory_order_seq_cst); }

void LogWriter::run() {
    ScopedThreadRole role(ThreadRole::Logger);
    tl_writer = this;
    batch_.reserve(LOG_BATCH_RESERVE);
    unflushed_ = false;
    lastFlush_ = std::chrono::steady_clock::now();
    sink_->purge();
    bool stopping = false;
    uint64_t finalTarget = 0;
    for (;;) {
        if (!stopping && stopping_.load(std::memory_order_seq_cst)) {
            stopping = true;
            finalTarget = queue_.enqueued();
        }
        urgent_.exchange(false, std::memory_order_seq_cst);
        const bool full = pass(stopping);
        if (stopping && queue_.consumed() >= finalTarget) break;
        if (full) continue;
        if (stopping || flushPending()) {
            std::this_thread::yield();
            continue;
        }
        sleepUntilWoken();
    }
    tl_writer = nullptr;
}

bool LogWriter::pass(bool stopping) {
    const uint64_t droppedNow = queue_.dropped();
    const uint64_t queuedAtStart = queue_.enqueued();
    const uint64_t passStart = queue_.consumed();
    bool dropLinePending = droppedNow != lastDropped_;
    bool urgentLine = false;
    size_t popped = 0;
    batch_.clear();
    LogRecord record;
    while (popped < LOG_QUEUE_CAPACITY) {
        const uint64_t position = queue_.consumed();
        if (!queue_.pop(record)) break;
        if (dropLinePending && position >= queuedAtStart) {
            takeDropLine(droppedNow - lastDropped_, urgentLine);
            lastDropped_ = droppedNow;
            dropLinePending = false;
        }
        take(record, urgentLine);
        ++popped;
    }
    if (dropLinePending) {
        takeDropLine(droppedNow - lastDropped_, urgentLine);
        lastDropped_ = droppedNow;
    }
    const uint64_t consumedNow = queue_.consumed();
    if (sink_->rotateIfDue()) sink_->purge();
    if (!batch_.empty()) {
        blockingRegion("log.write");
        if (sink_->write(batch_)) {
            unflushed_ = true;
        } else {
            lowerTo(lostFrom_, passStart);
        }
    }
    const uint64_t target = flushTarget_.load(std::memory_order_seq_cst);
    const bool flushAsked = target > handledFlushTarget_ && consumedNow >= target;
    const auto now = std::chrono::steady_clock::now();
    const bool intervalPassed = now - lastFlush_ >= timing_.flushInterval;
    if (unflushed_ && (urgentLine || flushAsked || stopping || intervalPassed)) {
        blockingRegion("log.write");
        if (sink_->flush()) unflushed_ = false;
        lastFlush_ = now;
    }
    if (flushAsked) handledFlushTarget_ = target;
    if (!unflushed_) durable_.store(consumedNow, std::memory_order_seq_cst);
    acknowledge();
    return popped >= LOG_QUEUE_CAPACITY;
}

void LogWriter::take(const LogRecord& record, bool& urgentLine) {
    ring_.publish(record);
    if (record.ringOnly()) return;
    char line[LOG_LINE_CAPACITY];
    const size_t length = formatter_.format(record, line, sizeof(line));
    batch_.append(line, length);
    if (record.level != LogLevel::INFO) urgentLine = true;
}

void LogWriter::takeDropLine(uint64_t count, bool& urgentLine) {
    const std::string message =
        std::to_string(count) + (count == 1 ? " log line dropped" : " log lines dropped");
    take(makeLogRecord(logClockNowNs(), LogLevel::WARN, DROP_SOURCE, message), urgentLine);
}

void LogWriter::sleepUntilWoken() {
    sleeping_.store(true, std::memory_order_seq_cst);
    const bool workWaiting = urgent_.load(std::memory_order_seq_cst) ||
                             queue_.depth() >= LOG_WAKE_DEPTH || flushPending() ||
                             stopping_.load(std::memory_order_seq_cst);
    if (!workWaiting) static_cast<void>(wake_.try_acquire_for(timing_.idleWait));
    sleeping_.store(false, std::memory_order_seq_cst);
}

void LogWriter::wakeIfSleeping() noexcept {
    if (sleeping_.load(std::memory_order_seq_cst) &&
        sleeping_.exchange(false, std::memory_order_seq_cst)) {
        wake_.release();
    }
}

bool LogWriter::flushPending() const noexcept {
    return flushTarget_.load(std::memory_order_seq_cst) > handledFlushTarget_;
}

bool LogWriter::covered(uint64_t target) const noexcept {
    return durable_.load(std::memory_order_seq_cst) >= target &&
           lostFrom_.load(std::memory_order_seq_cst) >= target;
}

void LogWriter::acknowledge() {
    const uint64_t durable = durable_.load(std::memory_order_seq_cst);
    const uint64_t lost = lostFrom_.load(std::memory_order_seq_cst);
    if (durable == acknowledgedDurable_ && lost == acknowledgedLost_) return;
    acknowledgedDurable_ = durable;
    acknowledgedLost_ = lost;
    const int waiting = flushWaiters_.load(std::memory_order_seq_cst);
    if (waiting > 0) flushAck_.release(waiting);
}

} // namespace satellite
