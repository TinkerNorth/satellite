// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include "core/json.h"
#include "core/types.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
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

// The word the dashboard's log viewer keys its styling off.
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
