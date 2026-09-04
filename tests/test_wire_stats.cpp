// SPDX-License-Identifier: LGPL-3.0-or-later

#include "../src/app/wire_stats.h"

#include <iostream>
#include <string>

#include "test_util.h"

using satellite::WireCounts;
using satellite::WireStats;

static uint64_t totalOf(WireStats& w) {
    uint64_t sum = 0;
    w.forEachCounter([&sum](std::atomic<uint64_t>& c) { sum += c.load(); });
    return sum;
}

static int counterCount(WireStats& w) {
    int n = 0;
    w.forEachCounter([&n](std::atomic<uint64_t>&) { n++; });
    return n;
}

static void test_resetCoversEveryCounter() {
    TEST("reset() zeroes every counter the struct declares");
    WireStats w;
    w.forEachCounter([](std::atomic<uint64_t>& c) { c.store(7); });
    const int fields = counterCount(w);
    EXPECT(fields > 0);
    EXPECT_EQ(totalOf(w), (uint64_t)(7 * fields));
    w.reset();
    EXPECT_EQ(totalOf(w), (uint64_t)0);
}

struct TypeSlot {
    uint16_t type;
    const char* name;
    std::atomic<uint64_t> WireStats::* slot;
};

static void test_inboundTypeMapping() {
    const TypeSlot cases[] = {
        {MSG_HEARTBEAT_PING, "heartbeat", &WireStats::rxHeartbeat},
        {MSG_MOTION, "motion", &WireStats::rxMotion},
        {MSG_BATTERY, "battery", &WireStats::rxBattery},
        {MSG_TOUCHPAD, "pointer", &WireStats::rxPointer},
        {MSG_MIC_AUDIO, "micAudio", &WireStats::rxMicAudio},
    };
    for (const auto& c : cases) {
        TEST(std::string("recordInbound: an accepted ") + c.name + " lands in exactly one slot");
        WireStats w;
        w.recordInbound(c.type, true);
        EXPECT_EQ((w.*(c.slot)).load(), (uint64_t)1);
        EXPECT_EQ(totalOf(w), (uint64_t)1);
    }
}

static void test_inboundGamepadIsLeftToTheHotPath() {
    TEST("recordInbound: accepted input frames move nothing (submitOk/submitFail own them)");
    WireStats w;
    w.recordInbound(MSG_GAMEPAD_DATA, true);
    EXPECT_EQ(totalOf(w), (uint64_t)0);
}

static void test_inboundRejectionSplit() {
    TEST("recordInbound: a known opcode that failed its length guard is malformed");
    const uint16_t known[] = {MSG_GAMEPAD_DATA, MSG_HEARTBEAT_PING, MSG_MOTION,
                              MSG_BATTERY,      MSG_TOUCHPAD,       MSG_MIC_AUDIO};
    for (uint16_t t : known) {
        WireStats w;
        w.recordInbound(t, false);
        EXPECT_EQ(w.rxMalformed.load(), (uint64_t)1);
        EXPECT_EQ(totalOf(w), (uint64_t)1);
    }

    TEST("recordInbound: an unrecognised opcode is not malformed");
    const uint16_t unknown[] = {0x0000, 0x0004, 0x0005, 0x0008, 0x000E, 0x0099, 0x7FFF, 0xFFFF};
    for (uint16_t t : unknown) {
        WireStats w;
        w.recordInbound(t, false);
        EXPECT_EQ(w.rxUnknownType.load(), (uint64_t)1);
        EXPECT_EQ(w.rxMalformed.load(), (uint64_t)0);
        EXPECT_EQ(totalOf(w), (uint64_t)1);
    }

    TEST("recordInbound: the deleted registration opcodes stay unknown, never malformed");
    EXPECT(!WireStats::isDispatchedInboundType(0x0004));
    EXPECT(!WireStats::isDispatchedInboundType(0x0005));
    EXPECT(!WireStats::isDispatchedInboundType(0x0008));
    EXPECT(!WireStats::isDispatchedInboundType(0x000E));
}

static void test_outboundTypeMapping() {
    const TypeSlot cases[] = {
        {MSG_HEARTBEAT_ACK, "heartbeatAck", &WireStats::txHeartbeatAck},
        {MSG_RUMBLE, "rumble", &WireStats::txRumble},
        {MSG_LIGHTBAR, "lightbar", &WireStats::txLightbar},
        {MSG_TRIGGER_EFFECTS, "triggerEffects", &WireStats::txTriggerEffects},
        {MSG_PLAYER_LEDS, "playerLeds", &WireStats::txPlayerLeds},
        {MSG_SPEAKER_AUDIO, "speakerAudio", &WireStats::txSpeakerAudio},
        {MSG_MIC_LED, "micLed", &WireStats::txMicLed},
        {MSG_SESSION_CLOSE, "sessionClose", &WireStats::txSessionClose},
    };
    for (const auto& c : cases) {
        TEST(std::string("recordOutbound: ") + c.name + " counts once, with its datagram bytes");
        WireStats w;
        w.recordOutbound(c.type, 40);
        EXPECT_EQ((w.*(c.slot)).load(), (uint64_t)1);
        EXPECT_EQ(w.txPackets.load(), (uint64_t)1);
        EXPECT_EQ(w.txBytes.load(), (uint64_t)40);
        EXPECT_EQ(totalOf(w), (uint64_t)(1 + 1 + 40));
    }

    TEST("recordOutbound: every message the client adapter sends has its own slot");
    WireStats w;
    const size_t n = sizeof(cases) / sizeof(cases[0]);
    for (const auto& c : cases) w.recordOutbound(c.type, 0);
    EXPECT_EQ(w.txPackets.load(), (uint64_t)n);
    EXPECT_EQ(totalOf(w), (uint64_t)(2 * n));
}

static void test_peakLoopSurvivesTheWindowedRead() {
    TEST("observePeakLoopUs: holds the peak across the maxLoopUs exchange that feeds it");
    WireStats w;
    EXPECT_EQ(w.observePeakLoopUs(120), (uint64_t)120);
    EXPECT_EQ(w.observePeakLoopUs(0), (uint64_t)120);
    EXPECT_EQ(w.observePeakLoopUs(90), (uint64_t)120);
    EXPECT_EQ(w.observePeakLoopUs(400), (uint64_t)400);
    EXPECT_EQ(w.observePeakLoopUs(0), (uint64_t)400);

    TEST("observePeakLoopUs: a rebind's reset clears the peak with everything else");
    w.reset();
    EXPECT_EQ(w.observePeakLoopUs(0), (uint64_t)0);
}

static void test_snapshotMirrorsTheCounters() {
    TEST("snapshot(): every field carried across, none crossed over");
    WireStats w;
    w.recordInbound(MSG_MOTION, true);
    w.recordInbound(0x7FFF, false);
    w.recordOutbound(MSG_RUMBLE, 33);
    w.rxRunt.store(4);
    w.rxUnknownToken.store(5);
    w.txSendFailed.store(6);
    w.txUnroutable.store(7);
    w.authNotPaired.store(8);
    w.authBadProof.store(9);
    w.sessionsReaped.store(10);

    const WireCounts c = w.snapshot();
    EXPECT_EQ(c.rx.motion, (uint64_t)1);
    EXPECT_EQ(c.rx.unknownType, (uint64_t)1);
    EXPECT_EQ(c.rx.heartbeat, (uint64_t)0);
    EXPECT_EQ(c.rx.runt, (uint64_t)4);
    EXPECT_EQ(c.rx.unknownToken, (uint64_t)5);
    EXPECT_EQ(c.tx.rumble, (uint64_t)1);
    EXPECT_EQ(c.tx.packets, (uint64_t)1);
    EXPECT_EQ(c.tx.bytes, (uint64_t)33);
    EXPECT_EQ(c.tx.sendFailed, (uint64_t)6);
    EXPECT_EQ(c.tx.unroutable, (uint64_t)7);
    EXPECT_EQ(c.authNotPaired, (uint64_t)8);
    EXPECT_EQ(c.authBadProof, (uint64_t)9);
    EXPECT_EQ(c.sessionsReaped, (uint64_t)10);
    EXPECT_EQ(c.rx.input, (uint64_t)0);
}

int main() {
    std::cout << "Running wire stats tests...\n\n";
    test_resetCoversEveryCounter();
    test_inboundTypeMapping();
    test_inboundGamepadIsLeftToTheHotPath();
    test_inboundRejectionSplit();
    test_outboundTypeMapping();
    test_peakLoopSurvivesTheWindowedRead();
    test_snapshotMirrorsTheCounters();

    std::cout << "\n=== Test Results ===\n";
    std::cout << "  Passed: " << g_pass << "\n";
    std::cout << "  Failed: " << g_fail << "\n";
    if (g_fail > 0) {
        std::cout << "  STATUS: FAIL\n";
        return 1;
    }
    std::cout << "  STATUS: ALL PASSED\n";
    return 0;
}
