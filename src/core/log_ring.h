// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include "core/json.h"
#include "core/log_record.h"
#include "core/types.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace satellite {

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

} // namespace satellite
