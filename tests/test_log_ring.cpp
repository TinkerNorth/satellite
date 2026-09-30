// SPDX-License-Identifier: LGPL-3.0-or-later
// The log ring as /api/logs reads it: which slots a reader with a given
// `since` is owed, in order, and the JSON one entry becomes.
#include "../src/core/log_ring.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "test_util.h"

using satellite::jsonDump;
using satellite::logEntryJson;
using satellite::logLevelLabel;
using satellite::LogRingSlot;
using satellite::logRingSlotsSince;

namespace {

bool slotsAre(const std::vector<LogRingSlot>& got, const std::vector<LogRingSlot>& want) {
    if (got.size() != want.size()) return false;
    for (size_t i = 0; i < got.size(); i++) {
        if (got[i].seq != want[i].seq || got[i].index != want[i].index) return false;
    }
    return true;
}

} // namespace

int main() {
    {
        TEST("an empty ring lists nothing");
        EXPECT(logRingSlotsSince(5, 0, 0, 0).empty());
    }
    {
        TEST("before the ring wraps, entries come oldest first with their sequence numbers");
        EXPECT(slotsAre(logRingSlotsSince(5, 3, 3, 0), {{0, 0}, {1, 1}, {2, 2}}));
    }
    {
        TEST("the first poll (since 0) sees the first entry ever written");
        const auto slots = logRingSlotsSince(5, 1, 1, 0);
        EXPECT_EQ(slots.size(), static_cast<size_t>(1));
        EXPECT(slotsAre(slots, {{0, 0}}));
    }
    {
        TEST("once wrapped, the oldest entry is the one after head");
        // Nine writes into four slots: the last four (5..8) sit at 1, 2, 3, 0, and head is 1.
        EXPECT(slotsAre(logRingSlotsSince(4, 1, 9, 0), {{5, 1}, {6, 2}, {7, 3}, {8, 0}}));
    }
    {
        TEST("since is the seq the reader got last time, so the reply starts at that entry");
        // A reader that saw seq=7 (seven entries, 0..6) is owed 7 and 8, not 8 alone.
        EXPECT(slotsAre(logRingSlotsSince(4, 1, 9, 7), {{7, 3}, {8, 0}}));
        EXPECT(slotsAre(logRingSlotsSince(4, 1, 9, 8), {{8, 0}}));
    }
    {
        TEST("a reader that is current, or ahead, gets nothing");
        EXPECT(logRingSlotsSince(4, 1, 9, 9).empty());
        EXPECT(logRingSlotsSince(4, 1, 9, 50).empty());
    }
    {
        TEST("never more than the ring holds, however many were written");
        EXPECT(
            slotsAre(logRingSlotsSince(4, 0, 1000, 0), {{996, 0}, {997, 1}, {998, 2}, {999, 3}}));
    }
    {
        TEST("a reader who fell behind the ring gets the oldest it still holds");
        EXPECT(
            slotsAre(logRingSlotsSince(4, 0, 1000, 500), {{996, 0}, {997, 1}, {998, 2}, {999, 3}}));
    }
    {
        TEST("logLevelLabel: the three words the viewer styles by");
        EXPECT_EQ(std::string(logLevelLabel(LogLevel::ERR)), std::string("error"));
        EXPECT_EQ(std::string(logLevelLabel(LogLevel::WARN)), std::string("warn"));
        EXPECT_EQ(std::string(logLevelLabel(LogLevel::INFO)), std::string("info"));
    }
    {
        TEST("logEntryJson: seq, epoch milliseconds, level word, source, message, in that order");
        LogEntry e;
        e.timestamp = std::chrono::system_clock::time_point(std::chrono::milliseconds(1234));
        e.level = LogLevel::WARN;
        e.source = "web";
        e.message = "hi";
        EXPECT_EQ(
            jsonDump(logEntryJson(7, e)),
            std::string(R"({"seq":7,"ts":1234,"level":"warn","source":"web","message":"hi"})"));
    }

    std::cout << "test_log_ring: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
