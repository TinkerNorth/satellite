// SPDX-License-Identifier: LGPL-3.0-or-later
#include "../src/core/log_ring.h"

#include <atomic>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "test_util.h"

using satellite::jsonDump;
using satellite::LOG_MESSAGE_CAPACITY;
using satellite::LOG_RING_SLOTS;
using satellite::logEntryJson;
using satellite::logLevelLabel;
using satellite::LogRecord;
using satellite::LogRing;
using satellite::LogRingEntry;
using satellite::makeLogRecord;

namespace {

constexpr uint64_t RACE_WRITES = 200'000;
constexpr int RACE_READERS = 2;

LogRecord entry(uint64_t n) {
    return makeLogRecord(static_cast<int64_t>(n), LogLevel::INFO, "ring",
                         "entry " + std::to_string(n));
}

std::unique_ptr<LogRing> ringWith(uint64_t count) {
    auto ring = std::make_unique<LogRing>();
    for (uint64_t n = 0; n < count; ++n) ring->publish(entry(n));
    return ring;
}

std::vector<uint64_t> seqsSince(const LogRing& ring, uint64_t since) {
    std::vector<LogRingEntry> entries;
    ring.copySince(since, entries);
    std::vector<uint64_t> seqs;
    for (const LogRingEntry& e : entries) seqs.push_back(e.seq);
    return seqs;
}

std::vector<uint64_t> range(uint64_t first, uint64_t end) {
    std::vector<uint64_t> seqs;
    for (uint64_t n = first; n < end; ++n) seqs.push_back(n);
    return seqs;
}

bool entriesMatchTheirSeq(const LogRing& ring, uint64_t since) {
    std::vector<LogRingEntry> entries;
    ring.copySince(since, entries);
    for (const LogRingEntry& e : entries) {
        if (e.record.timestampNs != static_cast<int64_t>(e.seq)) return false;
        if (e.record.messageText() != "entry " + std::to_string(e.seq)) return false;
    }
    return true;
}

std::string patternFor(uint64_t n) {
    const std::string unit = std::to_string(n) + "|";
    std::string text;
    while (text.size() + unit.size() <= LOG_MESSAGE_CAPACITY) text += unit;
    return text;
}

LogRecord patterned(uint64_t n) {
    return makeLogRecord(static_cast<int64_t>(n), LogLevel::WARN, std::to_string(n), patternFor(n));
}

bool whole(const LogRingEntry& e) {
    return e.record.timestampNs == static_cast<int64_t>(e.seq) &&
           e.record.level == LogLevel::WARN && e.record.sourceText() == std::to_string(e.seq) &&
           e.record.messageText() == patternFor(e.seq);
}

void testSlotsSince() {
    TEST("an empty ring lists nothing");
    auto empty = ringWith(0);
    EXPECT(seqsSince(*empty, 0).empty());
    std::vector<LogRingEntry> none;
    EXPECT_EQ(empty->copySince(0, none), uint64_t{0});
    EXPECT_EQ(empty->published(), uint64_t{0});

    TEST("before the ring wraps, entries come oldest first with their sequence numbers");
    auto three = ringWith(3);
    EXPECT(seqsSince(*three, 0) == range(0, 3));
    EXPECT(entriesMatchTheirSeq(*three, 0));

    TEST("the first poll (since 0) sees the first entry ever written");
    auto one = ringWith(1);
    EXPECT(seqsSince(*one, 0) == range(0, 1));

    TEST("once wrapped, the oldest entry is the one LOG_RING_SLOTS back");
    auto wrapped = ringWith(LOG_RING_SLOTS + 3);
    EXPECT(seqsSince(*wrapped, 0) == range(3, LOG_RING_SLOTS + 3));
    EXPECT(entriesMatchTheirSeq(*wrapped, 0));

    TEST("since is the seq the reader got last time, so the reply starts at that entry");
    auto nine = ringWith(9);
    EXPECT(seqsSince(*nine, 7) == range(7, 9));
    EXPECT(seqsSince(*nine, 8) == range(8, 9));

    TEST("a reader that is current, or ahead, gets nothing and the seq to send next");
    std::vector<LogRingEntry> current;
    EXPECT_EQ(nine->copySince(9, current), uint64_t{9});
    EXPECT(current.empty());
    std::vector<LogRingEntry> ahead;
    EXPECT_EQ(nine->copySince(50, ahead), uint64_t{9});
    EXPECT(ahead.empty());

    TEST("never more than the ring holds, however many were written");
    auto thousand = ringWith(1000);
    EXPECT_EQ(thousand->published(), uint64_t{1000});
    EXPECT(seqsSince(*thousand, 0) == range(1000 - LOG_RING_SLOTS, 1000));

    TEST("a reader who fell behind the ring gets the oldest it still holds");
    EXPECT(seqsSince(*thousand, 300) == range(1000 - LOG_RING_SLOTS, 1000));
    EXPECT(entriesMatchTheirSeq(*thousand, 300));
}

void testJson() {
    TEST("logLevelLabel: the three words the viewer styles by");
    EXPECT_EQ(std::string(logLevelLabel(LogLevel::ERR)), std::string("error"));
    EXPECT_EQ(std::string(logLevelLabel(LogLevel::WARN)), std::string("warn"));
    EXPECT_EQ(std::string(logLevelLabel(LogLevel::INFO)), std::string("info"));

    TEST("logEntryJson: seq, epoch milliseconds, level word, source, message, in that order");
    const LogRecord record = makeLogRecord(1'234'567'891, LogLevel::WARN, "web", "hi");
    EXPECT_EQ(jsonDump(logEntryJson(7, record)),
              std::string(R"({"seq":7,"ts":1234,"level":"warn","source":"web","message":"hi"})"));
}

void testRace() {
    TEST("a reader racing the writer never returns a torn record and sees sequence numbers in "
         "order");
    auto ring = std::make_unique<LogRing>();
    std::atomic<bool> done{false};
    std::atomic<uint64_t> torn{0};
    std::atomic<uint64_t> disorder{0};
    std::atomic<uint64_t> seen{0};
    std::atomic<uint64_t> overlapping{0};
    std::vector<std::thread> readers;
    for (int r = 0; r < RACE_READERS; ++r) {
        readers.emplace_back([&] {
            uint64_t since = 0;
            std::vector<LogRingEntry> entries;
            for (bool last = false; !last;) {
                last = done.load(std::memory_order_acquire);
                const uint64_t next = ring->copySince(since, entries);
                if (!last && !entries.empty()) overlapping.fetch_add(1);
                uint64_t previous = 0;
                bool first = true;
                for (const LogRingEntry& e : entries) {
                    if (e.seq < since || e.seq >= next || (!first && e.seq <= previous)) {
                        disorder.fetch_add(1);
                    }
                    if (!whole(e)) torn.fetch_add(1);
                    previous = e.seq;
                    first = false;
                }
                seen.fetch_add(entries.size());
                if (next < since) disorder.fetch_add(1);
                since = next;
            }
        });
    }
    for (uint64_t n = 0; n < RACE_WRITES; ++n) ring->publish(patterned(n));
    done.store(true, std::memory_order_release);
    for (std::thread& reader : readers) reader.join();
    EXPECT_EQ(torn.load(), uint64_t{0});
    EXPECT_EQ(disorder.load(), uint64_t{0});
    EXPECT(seen.load() > 0);
    EXPECT(overlapping.load() > 0);
    EXPECT_EQ(ring->published(), RACE_WRITES);
}

} // namespace

int main() {
    testSlotsSince();
    testJson();
    testRace();

    std::cout << "test_log_ring: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
