// SPDX-License-Identifier: LGPL-3.0-or-later
#include "../src/core/log_queue.h"
#include "../src/core/log_record.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "test_util.h"

using satellite::BasicLogQueue;
using satellite::LOG_MESSAGE_CAPACITY;
using satellite::LOG_QUEUE_CAPACITY;
using satellite::LOG_RECORD_BYTES;
using satellite::LOG_RECORD_RING_ONLY;
using satellite::LOG_SOURCE_CAPACITY;
using satellite::LOG_TRUNCATION_MARK;
using satellite::LogQueue;
using satellite::LogRecord;
using satellite::makeLogRecord;

namespace {

constexpr int PRODUCERS = 8;
constexpr uint64_t RECORDS_PER_PRODUCER = 100'000;
constexpr size_t SMALL_CAPACITY = 8;
constexpr size_t WRAP_CAPACITY = 64;
constexpr auto GIVE_UP_AFTER = std::chrono::seconds(30);

LogRecord numbered(int producer, uint64_t index) {
    const std::string source(1, static_cast<char>('A' + producer));
    return makeLogRecord(static_cast<int64_t>(index), LogLevel::INFO, source,
                         "record " + std::to_string(index));
}

struct Delivery {
    std::array<uint64_t, PRODUCERS> next{};
    uint64_t received = 0;
    uint64_t misplaced = 0;
    uint64_t abandoned = 0;

    void take(const LogRecord& record) {
        ++received;
        const std::string_view source = record.sourceText();
        const int producer = source.size() == 1 ? source[0] - 'A' : -1;
        if (producer < 0 || producer >= PRODUCERS) {
            ++misplaced;
            return;
        }
        const auto index = static_cast<uint64_t>(record.timestampNs);
        const size_t slot = static_cast<size_t>(producer);
        if (index != next[slot] || record.messageText() != "record " + std::to_string(index)) {
            ++misplaced;
        }
        next[slot] = index + 1;
    }

    bool complete() const {
        for (const uint64_t n : next) {
            if (n != RECORDS_PER_PRODUCER) return false;
        }
        return misplaced == 0 && abandoned == 0 && received == PRODUCERS * RECORDS_PER_PRODUCER;
    }
};

template <typename Queue> Delivery deliverConcurrently(Queue& queue) {
    const auto deadline = std::chrono::steady_clock::now() + GIVE_UP_AFTER;
    std::atomic<int> finished{0};
    std::atomic<uint64_t> abandoned{0};
    std::vector<std::thread> producers;
    for (int p = 0; p < PRODUCERS; ++p) {
        producers.emplace_back([&queue, &finished, &abandoned, deadline, p] {
            for (uint64_t i = 0; i < RECORDS_PER_PRODUCER; ++i) {
                const LogRecord record = numbered(p, i);
                bool pushed = queue.push(record);
                while (!pushed && std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::yield();
                    pushed = queue.push(record);
                }
                if (!pushed) {
                    abandoned.fetch_add(RECORDS_PER_PRODUCER - i);
                    break;
                }
            }
            finished.fetch_add(1, std::memory_order_release);
        });
    }
    Delivery delivery;
    LogRecord record;
    for (;;) {
        if (queue.pop(record)) {
            delivery.take(record);
            continue;
        }
        if (finished.load(std::memory_order_acquire) == PRODUCERS) {
            while (queue.pop(record)) delivery.take(record);
            break;
        }
        std::this_thread::yield();
    }
    for (std::thread& producer : producers) producer.join();
    delivery.abandoned = abandoned.load();
    return delivery;
}

void testRecordRoundTrip() {
    TEST("log record: 512 bytes, whatever it holds");
    EXPECT_EQ(sizeof(LogRecord), LOG_RECORD_BYTES);
    EXPECT_EQ(LOG_SOURCE_CAPACITY, size_t{31});

    TEST("log record: a record round-trips its timestamp, level, source, message and flags");
    const LogRecord record = makeLogRecord(1'234'567'890'123'456'789, LogLevel::WARN, "pairing",
                                           "Pair success", LOG_RECORD_RING_ONLY);
    EXPECT_EQ(record.timestampNs, int64_t{1'234'567'890'123'456'789});
    EXPECT(record.level == LogLevel::WARN);
    EXPECT_EQ(std::string(record.sourceText()), std::string("pairing"));
    EXPECT_EQ(std::string(record.messageText()), std::string("Pair success"));
    EXPECT(record.ringOnly());
    const LogRecord copy = record;
    EXPECT_EQ(copy.timestampNs, record.timestampNs);
    EXPECT_EQ(std::string(copy.messageText()), std::string("Pair success"));

    TEST("log record: a plain record is not ring-only, and every level survives");
    EXPECT(!makeLogRecord(0, LogLevel::ERR, "a", "b").ringOnly());
    EXPECT(makeLogRecord(0, LogLevel::ERR, "a", "b").level == LogLevel::ERR);
    EXPECT(makeLogRecord(0, LogLevel::INFO, "a", "b").level == LogLevel::INFO);

    TEST("log record: an empty source and message stay empty");
    const LogRecord empty = makeLogRecord(7, LogLevel::INFO, "", "");
    EXPECT(empty.sourceText().empty());
    EXPECT(empty.messageText().empty());

    TEST("log record: the queue hands a record back field for field");
    auto queue = std::make_unique<BasicLogQueue<SMALL_CAPACITY>>();
    EXPECT(queue->push(record));
    LogRecord popped;
    EXPECT(queue->pop(popped));
    EXPECT_EQ(popped.timestampNs, record.timestampNs);
    EXPECT(popped.level == record.level);
    EXPECT_EQ(std::string(popped.sourceText()), std::string("pairing"));
    EXPECT_EQ(std::string(popped.messageText()), std::string("Pair success"));
    EXPECT(popped.ringOnly());
}

void testTruncation() {
    TEST("log record: a message that fits exactly is kept whole and unmarked");
    const std::string exact(LOG_MESSAGE_CAPACITY, 'x');
    const LogRecord fits = makeLogRecord(0, LogLevel::INFO, "web", exact);
    EXPECT_EQ(std::string(fits.messageText()), exact);

    TEST("log record: a message that does not fit is cut and ends in ...");
    const std::string longer(LOG_MESSAGE_CAPACITY + 200, 'y');
    const LogRecord cut = makeLogRecord(0, LogLevel::INFO, "web", longer);
    const std::string kept(cut.messageText());
    EXPECT_EQ(kept.size(), LOG_MESSAGE_CAPACITY);
    EXPECT(kept.size() >= LOG_TRUNCATION_MARK.size() &&
           kept.compare(kept.size() - LOG_TRUNCATION_MARK.size(), LOG_TRUNCATION_MARK.size(),
                        LOG_TRUNCATION_MARK) == 0);
    EXPECT_EQ(kept.substr(0, LOG_MESSAGE_CAPACITY - LOG_TRUNCATION_MARK.size()),
              std::string(LOG_MESSAGE_CAPACITY - LOG_TRUNCATION_MARK.size(), 'y'));

    TEST("log record: the cut never splits a UTF-8 character");
    const size_t room = LOG_MESSAGE_CAPACITY - LOG_TRUNCATION_MARK.size();
    const std::string accented = std::string(room - 1, 'a') + "\xC3\xA9" + std::string(50, 'b');
    const LogRecord utf8 = makeLogRecord(0, LogLevel::INFO, "web", accented);
    EXPECT_EQ(std::string(utf8.messageText()),
              std::string(room - 1, 'a') + std::string(LOG_TRUNCATION_MARK));

    TEST("log record: a source longer than 31 bytes keeps its first 31");
    const std::string source(40, 's');
    EXPECT_EQ(std::string(makeLogRecord(0, LogLevel::INFO, source, "m").sourceText()),
              std::string(LOG_SOURCE_CAPACITY, 's'));
}

void testFullQueue() {
    TEST("log queue: capacity 4096, a power of two");
    EXPECT_EQ(LOG_QUEUE_CAPACITY, size_t{4096});
    EXPECT_EQ(LogQueue::CAPACITY, LOG_QUEUE_CAPACITY);

    TEST("log queue: a full queue refuses the record and counts it");
    auto queue = std::make_unique<BasicLogQueue<SMALL_CAPACITY>>();
    int accepted = 0;
    for (size_t i = 0; i < SMALL_CAPACITY; ++i) {
        if (queue->push(numbered(0, i))) ++accepted;
    }
    EXPECT_EQ(accepted, static_cast<int>(SMALL_CAPACITY));
    EXPECT(!queue->push(numbered(0, SMALL_CAPACITY)));
    EXPECT(!queue->push(numbered(0, SMALL_CAPACITY + 1)));
    EXPECT_EQ(queue->dropped(), uint64_t{2});
    EXPECT_EQ(queue->depth(), uint64_t{SMALL_CAPACITY});
    EXPECT_EQ(queue->enqueued(), uint64_t{SMALL_CAPACITY});

    TEST("log queue: a pop frees a cell for the next push");
    LogRecord record;
    EXPECT(queue->pop(record));
    EXPECT_EQ(record.timestampNs, int64_t{0});
    EXPECT_EQ(queue->consumed(), uint64_t{1});
    EXPECT(queue->push(numbered(0, SMALL_CAPACITY + 2)));
    EXPECT_EQ(queue->dropped(), uint64_t{2});

    TEST("log queue: records come out in the order they went in, then nothing");
    std::vector<int64_t> order;
    while (queue->pop(record)) order.push_back(record.timestampNs);
    const std::vector<int64_t> want{1, 2, 3, 4, 5, 6, 7, static_cast<int64_t>(SMALL_CAPACITY + 2)};
    EXPECT(order == want);
    EXPECT(!queue->pop(record));
    EXPECT_EQ(queue->depth(), uint64_t{0});
}

void testConcurrentDelivery() {
    TEST("log queue: eight producers, 100,000 records each, drained concurrently: every record "
         "exactly once, each producer's in order");
    auto queue = std::make_unique<LogQueue>();
    const Delivery delivery = deliverConcurrently(*queue);
    EXPECT_EQ(delivery.received, PRODUCERS * RECORDS_PER_PRODUCER);
    EXPECT_EQ(delivery.misplaced, uint64_t{0});
    EXPECT_EQ(delivery.abandoned, uint64_t{0});
    EXPECT(delivery.complete());

    TEST("log queue: the same through a 64-cell queue that wraps on every few records");
    auto small = std::make_unique<BasicLogQueue<WRAP_CAPACITY>>();
    const Delivery wrapped = deliverConcurrently(*small);
    EXPECT_EQ(wrapped.received, PRODUCERS * RECORDS_PER_PRODUCER);
    EXPECT_EQ(wrapped.misplaced, uint64_t{0});
    EXPECT_EQ(wrapped.abandoned, uint64_t{0});
    EXPECT(wrapped.complete());
}

} // namespace

int main() {
    testRecordRoundTrip();
    testTruncation();
    testFullQueue();
    testConcurrentDelivery();

    std::cout << "test_log_queue: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
