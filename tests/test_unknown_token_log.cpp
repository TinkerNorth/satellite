// SPDX-License-Identifier: LGPL-3.0-or-later
#include "../src/core/unknown_token_log.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>

#include "test_util.h"

using satellite::parseIPv4Nbo;
using satellite::UNKNOWN_TOKEN_QUIET_PERIOD;
using satellite::UnknownTokenLog;
using satellite::UnknownTokenLogLine;

namespace {

const uint32_t kDish = parseIPv4Nbo("192.168.1.23");
const std::chrono::steady_clock::time_point kT0 = std::chrono::steady_clock::now();

std::chrono::steady_clock::time_point at(int seconds) {
    return kT0 + std::chrono::seconds(seconds);
}

bool mentions(const UnknownTokenLogLine& line, const std::string& needle) {
    return line.message.find(needle) != std::string::npos;
}

UnknownTokenLog logAfterOneStretch(int quietSeconds, uint64_t dropped) {
    UnknownTokenLog log;
    log.observe(dropped, kDish, at(0));
    log.observe(dropped, kDish, at(quietSeconds));
    return log;
}

} // namespace

int main() {
    const int quiet = static_cast<int>(UNKNOWN_TOKEN_QUIET_PERIOD.count());
    {
        TEST("nothing to say while no datagram was dropped");
        UnknownTokenLog log;
        EXPECT(log.observe(0, 0, at(0)).message.empty());
        EXPECT(log.observe(0, 0, at(1)).message.empty());
    }
    {
        TEST("the first dropped datagrams open a stretch with one warning naming the sender");
        UnknownTokenLog log;
        const UnknownTokenLogLine line = log.observe(3, kDish, at(0));
        EXPECT(line.level == LogLevel::WARN);
        EXPECT(mentions(line, "Dropping datagrams from 192.168.1.23"));
        EXPECT(mentions(line, "session this Satellite does not have"));
        EXPECT(mentions(line, "opens a new session on its own"));
    }
    {
        TEST("more drops inside an open stretch stay quiet");
        UnknownTokenLog log;
        log.observe(3, kDish, at(0));
        EXPECT(log.observe(60, kDish, at(1)).message.empty());
        EXPECT(log.observe(120, kDish, at(2)).message.empty());
    }
    {
        TEST("a stretch that merely pauses for less than the quiet period stays open");
        UnknownTokenLog log;
        log.observe(3, kDish, at(0));
        EXPECT(log.observe(3, kDish, at(quiet - 1)).message.empty());
        EXPECT(log.observe(9, kDish, at(quiet)).message.empty());
    }
    {
        TEST("a stretch ends after the quiet period, reporting how many were dropped in it");
        UnknownTokenLog log;
        log.observe(3, kDish, at(0));
        log.observe(120, kDish, at(1));
        const UnknownTokenLogLine line = log.observe(120, kDish, at(1 + quiet));
        EXPECT(line.level == LogLevel::INFO);
        EXPECT(mentions(line, "Datagrams from 192.168.1.23"));
        EXPECT(mentions(line, "have stopped after 120 dropped datagrams"));
    }
    {
        TEST("a single dropped datagram is reported in the singular");
        UnknownTokenLog log;
        log.observe(1, kDish, at(0));
        EXPECT(mentions(log.observe(1, kDish, at(quiet)), "after 1 dropped datagram"));
    }
    {
        TEST("the ended line counts this stretch only, not the drops before it");
        UnknownTokenLog log = logAfterOneStretch(quiet, 50);
        log.observe(57, kDish, at(2 * quiet));
        const UnknownTokenLogLine line = log.observe(57, kDish, at(3 * quiet));
        EXPECT(mentions(line, "after 7 dropped datagrams"));
    }
    {
        TEST("an ended stretch says nothing more until datagrams are dropped again");
        UnknownTokenLog log = logAfterOneStretch(quiet, 50);
        EXPECT(log.observe(50, kDish, at(2 * quiet)).message.empty());
        EXPECT(log.observe(50, kDish, at(3 * quiet)).message.empty());
    }
    {
        TEST("drops after an ended stretch open a new one with a fresh warning");
        UnknownTokenLog log = logAfterOneStretch(quiet, 50);
        const UnknownTokenLogLine line = log.observe(51, kDish, at(2 * quiet));
        EXPECT(line.level == LogLevel::WARN);
        EXPECT(mentions(line, "Dropping datagrams from 192.168.1.23"));
    }
    {
        TEST("an unknown sender address leaves the address out of both lines");
        UnknownTokenLog log;
        const UnknownTokenLogLine opened = log.observe(2, 0, at(0));
        EXPECT(mentions(opened, "Dropping datagrams for a session"));
        EXPECT(!mentions(opened, " from "));
        const UnknownTokenLogLine ended = log.observe(2, 0, at(quiet));
        EXPECT(mentions(ended, "Datagrams for a session"));
        EXPECT(!mentions(ended, " from "));
    }
    {
        TEST("a counter that went backwards (receiver rebind) forgets the stretch silently");
        UnknownTokenLog log;
        log.observe(40, kDish, at(0));
        EXPECT(log.observe(0, 0, at(1)).message.empty());
        EXPECT(log.observe(0, 0, at(1 + quiet)).message.empty());
        const UnknownTokenLogLine line = log.observe(5, kDish, at(2 + quiet));
        EXPECT(line.level == LogLevel::WARN);
    }
    {
        TEST("the quiet period is the reaper's horizon");
        EXPECT_EQ(quiet, HEARTBEAT_INTERVAL_SEC * HEARTBEAT_MISS_MAX);
    }

    std::cout << "test_unknown_token_log: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
