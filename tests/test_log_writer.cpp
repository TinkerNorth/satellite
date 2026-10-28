// SPDX-License-Identifier: LGPL-3.0-or-later
#include "../src/core/log_writer.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "test_util.h"

using satellite::ILogSink;
using satellite::LOG_LINE_CAPACITY;
using satellite::LOG_LINE_END;
using satellite::LOG_MESSAGE_CAPACITY;
using satellite::LOG_QUEUE_CAPACITY;
using satellite::LOG_RECORD_RING_ONLY;
using satellite::LOG_SOURCE_CAPACITY;
using satellite::logClockNowNs;
using satellite::LogLineFormatter;
using satellite::LogRingEntry;
using satellite::LogWriter;
using satellite::LogWriterTiming;
using satellite::makeLogRecord;

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

constexpr milliseconds PATIENCE{5000};
constexpr milliseconds PROMPTLY{2000};
constexpr LogWriterTiming DROWSY{milliseconds(5000), milliseconds(1000)};
constexpr size_t NOT_FOUND = static_cast<size_t>(-1);

enum class Call { Write, Flush, Rotate, Purge };

struct SinkCall {
    Call call = Call::Write;
    std::string batch;
    bool rotated = false;
};

using Calls = std::vector<SinkCall>;

size_t countOf(const Calls& calls, Call kind) {
    return static_cast<size_t>(std::count_if(calls.begin(), calls.end(),
                                             [kind](const SinkCall& c) { return c.call == kind; }));
}

std::string writtenBy(const Calls& calls) {
    std::string text;
    for (const SinkCall& c : calls) {
        if (c.call == Call::Write) text += c.batch;
    }
    return text;
}

size_t firstWriteHolding(const Calls& calls, const std::string& needle) {
    for (size_t i = 0; i < calls.size(); ++i) {
        if (calls[i].call == Call::Write && calls[i].batch.find(needle) != std::string::npos) {
            return i;
        }
    }
    return NOT_FOUND;
}

size_t nextOf(const Calls& calls, Call kind, size_t after) {
    for (size_t i = after + 1; i < calls.size(); ++i) {
        if (calls[i].call == kind) return i;
    }
    return NOT_FOUND;
}

std::vector<std::string> linesOf(const std::string& text) {
    std::vector<std::string> lines;
    size_t from = 0;
    for (;;) {
        const size_t end = text.find(LOG_LINE_END, from);
        if (end == std::string::npos) break;
        lines.push_back(text.substr(from, end - from));
        from = end + LOG_LINE_END.size();
    }
    return lines;
}

bool inOrder(const std::string& text, const std::vector<std::string>& needles) {
    size_t from = 0;
    for (const std::string& needle : needles) {
        const size_t at = text.find(needle, from);
        if (at == std::string::npos) return false;
        from = at + needle.size();
    }
    return true;
}

class SinkRecord {
  public:
    void note(SinkCall call) {
        std::lock_guard<std::mutex> lk(mtx_);
        calls_.push_back(std::move(call));
        changed_.notify_all();
    }

    Calls calls() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return calls_;
    }

    bool waitFor(const std::function<bool(const Calls&)>& done,
                 milliseconds timeout = PATIENCE) const {
        std::unique_lock<std::mutex> lk(mtx_);
        return changed_.wait_for(lk, timeout, [&] { return done(calls_); });
    }

    void holdWrites(bool hold) {
        std::lock_guard<std::mutex> lk(mtx_);
        hold_ = hold;
        changed_.notify_all();
    }

    void awaitRelease() {
        std::unique_lock<std::mutex> lk(mtx_);
        changed_.wait(lk, [&] { return !hold_; });
    }

    void failWrites(bool fail) {
        std::lock_guard<std::mutex> lk(mtx_);
        fail_ = fail;
    }

    bool writesFail() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return fail_;
    }

    void rotateNext() {
        std::lock_guard<std::mutex> lk(mtx_);
        rotateNext_ = true;
    }

    bool takeRotation() {
        std::lock_guard<std::mutex> lk(mtx_);
        const bool rotate = rotateNext_;
        rotateNext_ = false;
        return rotate;
    }

  private:
    mutable std::mutex mtx_;
    mutable std::condition_variable changed_;
    Calls calls_;
    bool hold_ = false;
    bool fail_ = false;
    bool rotateNext_ = false;
};

class FakeSink : public ILogSink {
  public:
    explicit FakeSink(std::shared_ptr<SinkRecord> record) : record_(std::move(record)) {}

    bool write(std::string_view batch) override {
        record_->awaitRelease();
        const bool ok = !record_->writesFail();
        record_->note(SinkCall{Call::Write, std::string(batch), false});
        return ok;
    }

    bool flush() override {
        record_->note(SinkCall{Call::Flush, std::string(), false});
        return true;
    }

    bool rotateIfDue() override {
        const bool rotate = record_->takeRotation();
        record_->note(SinkCall{Call::Rotate, std::string(), rotate});
        return rotate;
    }

    void purge() override { record_->note(SinkCall{Call::Purge, std::string(), false}); }

  private:
    std::shared_ptr<SinkRecord> record_;
};

struct Harness {
    explicit Harness(LogWriterTiming timing = LogWriterTiming{})
        : sink(std::make_shared<SinkRecord>()), writer(std::make_unique<LogWriter>(timing)) {}

    bool start() { return writer->start(std::make_unique<FakeSink>(sink)); }

    bool awaitPass() {
        const size_t before = countOf(sink->calls(), Call::Rotate);
        return sink->waitFor(
            [before](const Calls& calls) { return countOf(calls, Call::Rotate) > before; });
    }

    bool awaitWritten(const std::string& needle, milliseconds timeout = PATIENCE) {
        return sink->waitFor(
            [&needle](const Calls& calls) {
                return writtenBy(calls).find(needle) != std::string::npos;
            },
            timeout);
    }

    bool awaitFlushes(size_t count, milliseconds timeout = PATIENCE) {
        return sink->waitFor(
            [count](const Calls& calls) { return countOf(calls, Call::Flush) >= count; }, timeout);
    }

    std::vector<LogRingEntry> ringSince(uint64_t since) const {
        std::vector<LogRingEntry> entries;
        writer->recent().copySince(since, entries);
        return entries;
    }

    std::shared_ptr<SinkRecord> sink;
    std::unique_ptr<LogWriter> writer;
};

std::string localStamp(int64_t seconds) {
    const auto when = static_cast<std::time_t>(seconds);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &when);
#else
    localtime_r(&when, &tm);
#endif
    char stamp[32];
    const size_t n = std::strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &tm);
    return std::string(stamp, n);
}

void testLineFormat() {
    TEST("log line: local time to the millisecond, the padded level, the source and the message");
    const int64_t seconds = 1'700'000'000;
    const int64_t ns = seconds * 1'000'000'000 + 123'456'789;
    LogLineFormatter formatter;
    char line[LOG_LINE_CAPACITY];
    const size_t err =
        formatter.format(makeLogRecord(ns, LogLevel::ERR, "svc", "boom"), line, sizeof(line));
    EXPECT_EQ(std::string(line, err),
              localStamp(seconds) + ".123 [ERROR] [svc] boom" + std::string(LOG_LINE_END));
    const size_t warn =
        formatter.format(makeLogRecord(ns, LogLevel::WARN, "web", "careful"), line, sizeof(line));
    EXPECT_EQ(std::string(line, warn),
              localStamp(seconds) + ".123 [WARN ] [web] careful" + std::string(LOG_LINE_END));
    const size_t info =
        formatter.format(makeLogRecord(ns + 1'000'000'000, LogLevel::INFO, "app", "next second"),
                         line, sizeof(line));
    EXPECT_EQ(std::string(line, info), localStamp(seconds + 1) + ".123 [INFO ] [app] next second" +
                                           std::string(LOG_LINE_END));

    TEST("log line: the longest record still fits a line buffer and ends its line");
    const size_t longest = formatter.format(
        makeLogRecord(ns, LogLevel::WARN, std::string(LOG_SOURCE_CAPACITY + 5, 's'),
                      std::string(LOG_MESSAGE_CAPACITY + 50, 'm')),
        line, sizeof(line));
    const std::string text(line, longest);
    EXPECT(longest < LOG_LINE_CAPACITY);
    EXPECT(text.size() >= LOG_LINE_END.size() &&
           text.compare(text.size() - LOG_LINE_END.size(), LOG_LINE_END.size(), LOG_LINE_END) == 0);
    EXPECT(text.find("m...") != std::string::npos);
}

void testWarnWakesAndBatches() {
    TEST("log writer: INFO lines wait for the idle window instead of waking the writer");
    Harness h(DROWSY);
    EXPECT(h.start());
    EXPECT(h.awaitPass());
    std::vector<std::string> expected;
    for (int i = 0; i < 50; ++i) {
        const std::string line = "batched " + std::to_string(i) + ";";
        h.writer->log(LogLevel::INFO, "test", line);
        expected.push_back(line);
    }
    std::this_thread::sleep_for(milliseconds(300));
    EXPECT_EQ(countOf(h.sink->calls(), Call::Write), size_t{0});

    TEST("log writer: a WARN wakes the sleeping writer, which writes the whole batch in one call "
         "and then flushes");
    h.writer->log(LogLevel::WARN, "test", "batched warning");
    expected.push_back("batched warning");
    EXPECT(h.awaitFlushes(1, PROMPTLY));
    const Calls calls = h.sink->calls();
    EXPECT_EQ(countOf(calls, Call::Write), size_t{1});
    const std::string written = writtenBy(calls);
    EXPECT(inOrder(written, expected));
    EXPECT_EQ(linesOf(written).size(), size_t{51});
    const size_t write = firstWriteHolding(calls, "batched warning");
    EXPECT(write != NOT_FOUND && nextOf(calls, Call::Flush, write) != NOT_FOUND);
    h.writer->stop();
}

void testErrFlushesAtOnce() {
    TEST("log writer: an ERR wakes the sleeping writer and is flushed at once");
    Harness h(DROWSY);
    EXPECT(h.start());
    EXPECT(h.awaitPass());
    h.writer->log(LogLevel::ERR, "test", "an error");
    EXPECT(h.awaitFlushes(1, PROMPTLY));
    const Calls calls = h.sink->calls();
    const size_t write = firstWriteHolding(calls, "[ERROR] [test] an error");
    EXPECT(write != NOT_FOUND && nextOf(calls, Call::Flush, write) != NOT_FOUND);
    h.writer->stop();
}

void testInfoWithinTheWindow() {
    TEST("log writer: an INFO line reaches the sink within the 100 ms idle window");
    Harness h;
    EXPECT(h.start());
    EXPECT(h.awaitPass());
    const Clock::time_point loggedAt = Clock::now();
    h.writer->log(LogLevel::INFO, "test", "window probe");
    EXPECT(h.awaitWritten("window probe"));
    EXPECT(Clock::now() - loggedAt < milliseconds(1000));
    h.writer->stop();
}

void testIntervalFlush() {
    TEST("log writer: an INFO-only batch is written but not flushed before the flush interval");
    Harness h(LogWriterTiming{milliseconds(100), milliseconds(1500)});
    EXPECT(h.start());
    EXPECT(h.awaitPass());
    h.writer->log(LogLevel::INFO, "test", "interval probe");
    EXPECT(h.awaitWritten("interval probe"));
    const Clock::time_point writtenAt = Clock::now();
    EXPECT_EQ(countOf(h.sink->calls(), Call::Flush), size_t{0});

    TEST("log writer: once the interval has passed since the last flush, the writer flushes");
    EXPECT(h.awaitFlushes(1));
    EXPECT(Clock::now() - writtenAt >= milliseconds(500));
    h.writer->stop();
}

void testDropsInOrder() {
    TEST("log writer: a full queue refuses lines and counts them");
    Harness h;
    size_t refusedEarly = 0;
    for (size_t i = 0; i < LOG_QUEUE_CAPACITY; ++i) {
        const std::string line = "queued " + std::to_string(i) + ";";
        if (!h.writer->submit(makeLogRecord(logClockNowNs(), LogLevel::INFO, "test", line))) {
            ++refusedEarly;
        }
    }
    EXPECT_EQ(refusedEarly, size_t{0});
    size_t refused = 0;
    for (int i = 0; i < 10; ++i) {
        const std::string line = "refused " + std::to_string(i);
        if (!h.writer->submit(makeLogRecord(logClockNowNs(), LogLevel::INFO, "test", line))) {
            ++refused;
        }
    }
    EXPECT_EQ(refused, size_t{10});
    EXPECT_EQ(h.writer->dropped(), uint64_t{10});

    TEST("log writer: the drops become one counted WARN line after the lines queued before "
         "them and before the lines logged after");
    EXPECT(h.start());
    EXPECT(h.awaitPass());
    h.writer->log(LogLevel::INFO, "test", "after the drops");
    EXPECT(h.writer->flush(PATIENCE));
    const std::string written = writtenBy(h.sink->calls());
    const std::vector<std::string> lines = linesOf(written);
    EXPECT_EQ(lines.size(), LOG_QUEUE_CAPACITY + 2);
    if (lines.size() == LOG_QUEUE_CAPACITY + 2) {
        EXPECT(lines[0].find("queued 0;") != std::string::npos);
        EXPECT(lines[LOG_QUEUE_CAPACITY - 1].find("queued 4095;") != std::string::npos);
        EXPECT(lines[LOG_QUEUE_CAPACITY].find("[WARN ] [log] 10 log lines dropped") !=
               std::string::npos);
        EXPECT(lines[LOG_QUEUE_CAPACITY + 1].find("after the drops") != std::string::npos);
    }
    EXPECT(written.find("refused") == std::string::npos);

    TEST("log writer: the drop line takes the same place in the ring");
    const std::vector<LogRingEntry> entries = h.ringSince(LOG_QUEUE_CAPACITY);
    EXPECT(!entries.empty() && entries[0].seq == LOG_QUEUE_CAPACITY &&
           entries[0].record.messageText() == "10 log lines dropped" &&
           entries[0].record.level == LogLevel::WARN);
    h.writer->stop();
}

void testOneDropReadsAsOne() {
    TEST("log writer: a single refused line is counted in the singular");
    Harness h;
    for (size_t i = 0; i <= LOG_QUEUE_CAPACITY; ++i) {
        h.writer->submit(makeLogRecord(logClockNowNs(), LogLevel::INFO, "test", "fill"));
    }
    EXPECT_EQ(h.writer->dropped(), uint64_t{1});
    EXPECT(h.start());
    EXPECT(h.writer->flush(PATIENCE));
    EXPECT(writtenBy(h.sink->calls()).find("[log] 1 log line dropped") != std::string::npos);
    h.writer->stop();
}

void testFlushMeansFlushed() {
    TEST("log writer: with no writer running, flush promises nothing");
    auto idle = std::make_unique<LogWriter>();
    idle->log(LogLevel::INFO, "test", "nobody writes this");
    EXPECT(!idle->flush(milliseconds(10)));

    TEST("log writer: flush returns true only once the lines before it were written and flushed");
    Harness h(DROWSY);
    EXPECT(h.start());
    EXPECT(h.awaitPass());
    h.writer->log(LogLevel::INFO, "test", "flush probe");
    EXPECT(h.writer->flush(PATIENCE));
    const Calls calls = h.sink->calls();
    const size_t write = firstWriteHolding(calls, "flush probe");
    EXPECT(write != NOT_FOUND);
    EXPECT(write != NOT_FOUND && nextOf(calls, Call::Flush, write) != NOT_FOUND);

    TEST("log writer: a later flush covers what was logged since the previous one");
    h.writer->log(LogLevel::INFO, "test", "second probe");
    EXPECT(h.writer->flush(PATIENCE));
    const Calls later = h.sink->calls();
    const size_t second = firstWriteHolding(later, "second probe");
    EXPECT(second != NOT_FOUND && nextOf(later, Call::Flush, second) != NOT_FOUND);
    h.writer->stop();
}

void testFlushOnABlockedSink() {
    TEST("log writer: flush gives up at its deadline while the sink is blocked");
    Harness h(DROWSY);
    EXPECT(h.start());
    EXPECT(h.awaitPass());
    h.sink->holdWrites(true);
    h.writer->log(LogLevel::INFO, "test", "held line");
    const Clock::time_point asked = Clock::now();
    EXPECT(!h.writer->flush(milliseconds(300)));
    EXPECT(Clock::now() - asked >= milliseconds(250));

    TEST("log writer: logging never waits for a blocked sink");
    const Clock::time_point loggingFrom = Clock::now();
    for (int i = 0; i < 1000; ++i) {
        h.writer->log(LogLevel::WARN, "test", "while held " + std::to_string(i) + ";");
    }
    EXPECT(Clock::now() - loggingFrom < milliseconds(1000));

    TEST("log writer: once the sink moves again, a flush covers everything logged before it");
    h.sink->holdWrites(false);
    EXPECT(h.writer->flush(PATIENCE));
    const std::string written = writtenBy(h.sink->calls());
    EXPECT(inOrder(written, {"held line", "while held 0;", "while held 999;"}));
    h.writer->stop();
}

void testFailedWrite() {
    TEST("log writer: a failed write makes flush report the loss without waiting out its "
         "deadline");
    Harness h(DROWSY);
    EXPECT(h.start());
    EXPECT(h.awaitPass());
    h.sink->failWrites(true);
    h.writer->log(LogLevel::INFO, "test", "lost line");
    const Clock::time_point asked = Clock::now();
    EXPECT(!h.writer->flush(PATIENCE));
    EXPECT(Clock::now() - asked < milliseconds(2500));

    TEST("log writer: the lost line still reaches the ring");
    bool inRing = false;
    for (const LogRingEntry& entry : h.ringSince(0)) {
        if (entry.record.messageText() == "lost line") inRing = true;
    }
    EXPECT(inRing);
    h.writer->stop();
}

void testRotationAndRetention() {
    TEST("log writer: retention runs once when the writer starts, before anything else");
    Harness h(DROWSY);
    EXPECT(h.start());
    EXPECT(h.awaitPass());
    const Calls first = h.sink->calls();
    EXPECT(!first.empty() && first[0].call == Call::Purge);
    EXPECT_EQ(countOf(first, Call::Purge), size_t{1});

    TEST("log writer: a pass that rotated runs retention, then writes into the new file");
    h.sink->rotateNext();
    h.writer->log(LogLevel::WARN, "test", "after rotation");
    EXPECT(h.awaitFlushes(1));
    const Calls rotated = h.sink->calls();
    size_t rotation = NOT_FOUND;
    for (size_t i = 0; i < rotated.size(); ++i) {
        if (rotated[i].call == Call::Rotate && rotated[i].rotated) rotation = i;
    }
    EXPECT(rotation != NOT_FOUND && rotation + 2 < rotated.size());
    if (rotation != NOT_FOUND && rotation + 2 < rotated.size()) {
        EXPECT(rotated[rotation + 1].call == Call::Purge);
        EXPECT(rotated[rotation + 2].call == Call::Write &&
               rotated[rotation + 2].batch.find("after rotation") != std::string::npos);
    }
    EXPECT_EQ(countOf(rotated, Call::Purge), size_t{2});

    TEST("log writer: a pass that did not rotate runs no retention");
    h.writer->log(LogLevel::WARN, "test", "no rotation");
    EXPECT(h.awaitFlushes(2));
    EXPECT_EQ(countOf(h.sink->calls(), Call::Purge), size_t{2});
    h.writer->stop();
}

void testStopDrains() {
    TEST("log writer: stop writes and flushes everything still queued, then stops");
    Harness h(DROWSY);
    EXPECT(h.start());
    EXPECT(h.awaitPass());
    EXPECT(h.writer->running());
    std::vector<std::string> expected;
    for (int i = 0; i < 20; ++i) {
        const std::string line = "draining " + std::to_string(i) + ";";
        h.writer->log(LogLevel::INFO, "test", line);
        expected.push_back(line);
    }
    h.writer->stop();
    const Calls calls = h.sink->calls();
    EXPECT(inOrder(writtenBy(calls), expected));
    EXPECT(!calls.empty() && calls.back().call == Call::Flush);
    EXPECT(!h.writer->running());
    EXPECT(!h.writer->flush(milliseconds(10)));

    TEST("log writer: a stopped writer starts again and picks up where it left off");
    h.writer->log(LogLevel::INFO, "test", "after the restart");
    EXPECT(h.start());
    EXPECT(h.writer->flush(PATIENCE));
    EXPECT(writtenBy(h.sink->calls()).find("after the restart") != std::string::npos);
    h.writer->stop();
}

void testRingOnlyRecords() {
    TEST("log writer: a line the crash path already wrote reaches the ring but not the sink");
    Harness h(DROWSY);
    EXPECT(h.start());
    EXPECT(h.awaitPass());
    EXPECT(h.writer->submit(makeLogRecord(logClockNowNs(), LogLevel::ERR, "crash",
                                          "written directly", LOG_RECORD_RING_ONLY)));
    h.writer->log(LogLevel::WARN, "test", "after the crash line");
    EXPECT(h.writer->flush(PATIENCE));
    const std::string written = writtenBy(h.sink->calls());
    EXPECT(written.find("written directly") == std::string::npos);
    EXPECT(written.find("after the crash line") != std::string::npos);
    bool inRing = false;
    for (const LogRingEntry& entry : h.ringSince(0)) {
        if (entry.record.messageText() == "written directly") inRing = true;
    }
    EXPECT(inRing);
    h.writer->stop();
}

void testRingNumbering() {
    TEST("log writer: the ring numbers lines contiguously from 0 and keeps the last 500");
    Harness h(DROWSY);
    EXPECT(h.start());
    for (int i = 0; i < 600; ++i) {
        h.writer->log(LogLevel::INFO, "test", "ring " + std::to_string(i));
    }
    EXPECT(h.writer->flush(PATIENCE));
    EXPECT_EQ(h.writer->recent().published(), uint64_t{600});
    const std::vector<LogRingEntry> entries = h.ringSince(0);
    EXPECT_EQ(entries.size(), size_t{500});
    bool numbered = entries.size() == 500;
    for (size_t i = 0; numbered && i < entries.size(); ++i) {
        const uint64_t seq = 100 + i;
        numbered = entries[i].seq == seq &&
                   entries[i].record.messageText() == "ring " + std::to_string(seq);
    }
    EXPECT(numbered);
    h.writer->stop();
}

} // namespace

int main() {
    testLineFormat();
    testWarnWakesAndBatches();
    testErrFlushesAtOnce();
    testInfoWithinTheWindow();
    testIntervalFlush();
    testDropsInOrder();
    testOneDropReadsAsOne();
    testFlushMeansFlushed();
    testFlushOnABlockedSink();
    testFailedWrite();
    testRotationAndRetention();
    testStopDrains();
    testRingOnlyRecords();
    testRingNumbering();

    std::cout << "test_log_writer: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
