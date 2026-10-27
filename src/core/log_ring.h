// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include "core/json.h"
#include "core/log_record.h"
#include "core/types.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace satellite {

// One entry's place in the log ring: its sequence number and the slot it
// occupies.
struct LogRingSlot {
    uint64_t seq;
    int index;
};

// The entries a reader has not seen, oldest first. Entries are numbered from
// 0 in the order written; `seq` is the count ever written, which is also the
// number the reader gets back and sends as `since` next time, so the reply
// starts at entry `since` itself. `head` is the next write position; the ring
// keeps the last `ringSize` entries, so a reader who fell further behind than
// that sees the oldest the ring still holds.
inline std::vector<LogRingSlot> logRingSlotsSince(int ringSize, int head, uint64_t seq,
                                                  uint64_t since) {
    std::vector<LogRingSlot> out;
    const int count = static_cast<int>(std::min(seq, static_cast<uint64_t>(ringSize)));
    const uint64_t oldestSeq = seq - static_cast<uint64_t>(count);
    for (int i = 0; i < count; i++) {
        const uint64_t entrySeq = oldestSeq + static_cast<uint64_t>(i);
        if (entrySeq < since) continue;
        const int index = (head - count + i + ringSize) % ringSize;
        out.push_back({entrySeq, index});
    }
    return out;
}

inline constexpr size_t LOG_RING_SLOTS = 500;
inline constexpr size_t LOG_RECORD_WORDS = LOG_RECORD_BYTES / sizeof(uint64_t);

static_assert(LOG_RECORD_WORDS * sizeof(uint64_t) == LOG_RECORD_BYTES);

struct LogRingEntry {
    uint64_t seq = 0;
    LogRecord record{};
};

class LogRing {
  public:
    void publish(const LogRecord& record) noexcept {
        const uint64_t seq = published_.load(std::memory_order_relaxed);
        Slot& slot = slots_[static_cast<size_t>(seq % LOG_RING_SLOTS)];
        const auto words = std::bit_cast<std::array<uint64_t, LOG_RECORD_WORDS>>(record);
        slot.version.fetch_add(1, std::memory_order_acq_rel);
        std::atomic_thread_fence(std::memory_order_release);
        slot.seq.store(seq, std::memory_order_relaxed);
        for (size_t i = 0; i < LOG_RECORD_WORDS; ++i) {
            slot.words[i].store(words[i], std::memory_order_relaxed);
        }
        slot.version.fetch_add(1, std::memory_order_release);
        published_.store(seq + 1, std::memory_order_release);
    }

    uint64_t published() const noexcept { return published_.load(std::memory_order_acquire); }

    uint64_t copySince(uint64_t since, std::vector<LogRingEntry>& out) const {
        out.clear();
        const uint64_t end = published();
        const uint64_t oldest = end > LOG_RING_SLOTS ? end - LOG_RING_SLOTS : 0;
        const uint64_t first = std::max(since, oldest);
        if (first >= end) return end;
        out.reserve(static_cast<size_t>(end - first));
        LogRingEntry entry;
        for (uint64_t seq = first; seq < end; ++seq) {
            if (read(seq, entry)) out.push_back(entry);
        }
        return end;
    }

  private:
    struct Slot {
        std::atomic<uint64_t> version{0};
        std::atomic<uint64_t> seq{0};
        std::array<std::atomic<uint64_t>, LOG_RECORD_WORDS> words{};
    };

    bool read(uint64_t seq, LogRingEntry& out) const noexcept {
        const Slot& slot = slots_[static_cast<size_t>(seq % LOG_RING_SLOTS)];
        const uint64_t before = slot.version.load(std::memory_order_acquire);
        if ((before & 1u) != 0) return false;
        const uint64_t stored = slot.seq.load(std::memory_order_relaxed);
        std::array<uint64_t, LOG_RECORD_WORDS> words{};
        for (size_t i = 0; i < LOG_RECORD_WORDS; ++i) {
            words[i] = slot.words[i].load(std::memory_order_relaxed);
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        if (slot.version.load(std::memory_order_relaxed) != before || stored != seq) return false;
        out.record = std::bit_cast<LogRecord>(words);
        out.seq = seq;
        return true;
    }

    std::array<Slot, LOG_RING_SLOTS> slots_{};
    std::atomic<uint64_t> published_{0};
};

inline const char* logLevelLabel(LogLevel level) {
    switch (level) {
    case LogLevel::ERR:
        return "error";
    case LogLevel::WARN:
        return "warn";
    case LogLevel::INFO:
        return "info";
    }
    return "info";
}

inline JsonOut logEntryJson(uint64_t seq, const LogRecord& record) {
    JsonOut o;
    o["seq"] = seq;
    o["ts"] = record.timestampNs / 1'000'000;
    o["level"] = logLevelLabel(record.level);
    o["source"] = std::string(record.sourceText());
    o["message"] = std::string(record.messageText());
    return o;
}

// `ts` is Unix epoch milliseconds, which is what the viewer formats.
inline JsonOut logEntryJson(uint64_t seq, const LogEntry& e) {
    const auto epochMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(e.timestamp.time_since_epoch())
            .count();
    JsonOut o;
    o["seq"] = seq;
    o["ts"] = static_cast<int64_t>(epochMs);
    o["level"] = logLevelLabel(e.level);
    o["source"] = e.source;
    o["message"] = e.message;
    return o;
}

} // namespace satellite
