// SPDX-License-Identifier: LGPL-3.0-or-later
#include "pause_detector.h"

#include <cstdint>
#include <iostream>
#include <vector>

#include "test_util.h"

using satellite::bench::Pause;
using satellite::bench::PauseDetector;
using satellite::bench::SlotChange;
using satellite::bench::SlotObservation;
using satellite::bench::SlotSummary;

namespace {

constexpr size_t SLOT_COUNT = 4;
constexpr double GAP_MS = 40.0;
constexpr size_t SLOT_A = 0;
constexpr size_t SLOT_B = 1;
constexpr size_t FIRST_UNUSED_SLOT = 2;
constexpr uint32_t FIRST_PACKET = 100;
constexpr double STEADY_PERIOD_MS = 8.0;
constexpr int STEADY_SAMPLES = 100;

SlotObservation connectedReading(PauseDetector& detector, size_t slot, uint32_t packet,
                                 double nowMs) {
    return detector.observe(slot, true, packet, nowMs);
}

SlotObservation disconnectedReading(PauseDetector& detector, size_t slot, double nowMs) {
    return detector.observe(slot, false, 0, nowMs);
}

} // namespace

static void test_steady_updates_never_pause() {
    TEST("pause detector: updates every 8 ms are counted and never reported");
    PauseDetector detector(SLOT_COUNT, GAP_MS);
    int reported = 0;
    for (int i = 0; i < STEADY_SAMPLES; ++i) {
        const double nowMs = i * STEADY_PERIOD_MS;
        const SlotObservation seen = connectedReading(detector, SLOT_A, FIRST_PACKET + i, nowMs);
        if (seen.pause) ++reported;
    }
    const SlotSummary summary = detector.summary(SLOT_A);
    EXPECT_EQ(reported, 0);
    EXPECT_EQ(summary.updates, uint64_t{STEADY_SAMPLES - 1});
    EXPECT_EQ(summary.pausesOverThreshold, uint64_t{0});
    EXPECT_EQ(summary.longestPauseMs, STEADY_PERIOD_MS);
}

static void test_gap_at_threshold_is_reported() {
    TEST("pause detector: a gap of exactly --gap-ms is reported with its start and end");
    PauseDetector detector(SLOT_COUNT, GAP_MS);
    connectedReading(detector, SLOT_A, FIRST_PACKET, 0.0);
    const SlotObservation before = connectedReading(detector, SLOT_A, FIRST_PACKET + 1, 10.0);
    const SlotObservation at = connectedReading(detector, SLOT_A, FIRST_PACKET + 2, 50.0);
    EXPECT(!before.pause);
    EXPECT(at.pause.has_value());
    const Pause pause = at.pause.value_or(Pause{});
    EXPECT_EQ(pause.startMs, 10.0);
    EXPECT_EQ(pause.endMs, 50.0);
    EXPECT_EQ(pause.lengthMs(), 40.0);
    EXPECT_EQ(detector.summary(SLOT_A).pausesOverThreshold, uint64_t{1});
}

static void test_gap_under_threshold_is_only_the_longest() {
    TEST("pause detector: a gap under --gap-ms is not reported but is the longest pause");
    PauseDetector detector(SLOT_COUNT, GAP_MS);
    connectedReading(detector, SLOT_A, FIRST_PACKET, 0.0);
    const SlotObservation seen = connectedReading(detector, SLOT_A, FIRST_PACKET + 1, 39.5);
    const SlotSummary summary = detector.summary(SLOT_A);
    EXPECT(!seen.pause);
    EXPECT_EQ(summary.pausesOverThreshold, uint64_t{0});
    EXPECT_EQ(summary.longestPauseMs, 39.5);
    EXPECT_EQ(summary.longestPauseEndMs, 39.5);
}

static void test_unchanged_packet_number_extends_the_pause() {
    TEST("pause detector: readings with the same packet number are not updates");
    PauseDetector detector(SLOT_COUNT, GAP_MS);
    connectedReading(detector, SLOT_A, FIRST_PACKET, 0.0);
    const SlotObservation first = connectedReading(detector, SLOT_A, FIRST_PACKET, 20.0);
    const SlotObservation second = connectedReading(detector, SLOT_A, FIRST_PACKET, 40.0);
    const SlotObservation ended = connectedReading(detector, SLOT_A, FIRST_PACKET + 1, 60.0);
    EXPECT(!first.pause);
    EXPECT(!second.pause);
    EXPECT(ended.pause.has_value());
    const Pause pause = ended.pause.value_or(Pause{});
    EXPECT_EQ(pause.startMs, 0.0);
    EXPECT_EQ(pause.endMs, 60.0);
    EXPECT_EQ(detector.summary(SLOT_A).updates, uint64_t{1});
}

static void test_first_reading_only_sets_the_baseline() {
    TEST("pause detector: the first reading after a connect is a baseline, not an update");
    PauseDetector detector(SLOT_COUNT, GAP_MS);
    const SlotObservation connected = connectedReading(detector, SLOT_A, FIRST_PACKET, 500.0);
    EXPECT(connected.change == SlotChange::Connected);
    EXPECT(!connected.pause);
    EXPECT_EQ(detector.summary(SLOT_A).updates, uint64_t{0});
    EXPECT_EQ(detector.summary(SLOT_A).connects, uint64_t{1});
    const SlotObservation next = connectedReading(detector, SLOT_A, FIRST_PACKET + 1, 508.0);
    EXPECT(next.change == SlotChange::None);
    EXPECT_EQ(detector.summary(SLOT_A).updates, uint64_t{1});
    EXPECT_EQ(detector.summary(SLOT_A).longestPauseMs, 8.0);
}

static void test_connection_changes_are_reported_once() {
    TEST("pause detector: a connect and a disconnect are each reported on the reading that "
         "shows them");
    PauseDetector detector(SLOT_COUNT, GAP_MS);
    const SlotObservation absent = disconnectedReading(detector, SLOT_A, 0.0);
    const SlotObservation joined = connectedReading(detector, SLOT_A, FIRST_PACKET, 1.0);
    const SlotObservation still = connectedReading(detector, SLOT_A, FIRST_PACKET + 1, 2.0);
    const SlotObservation left = disconnectedReading(detector, SLOT_A, 3.0);
    const SlotObservation gone = disconnectedReading(detector, SLOT_A, 4.0);
    EXPECT(absent.change == SlotChange::None);
    EXPECT(joined.change == SlotChange::Connected);
    EXPECT(still.change == SlotChange::None);
    EXPECT(left.change == SlotChange::Disconnected);
    EXPECT(gone.change == SlotChange::None);
}

static void test_reconnect_is_not_a_pause() {
    TEST("pause detector: the time a slot spends disconnected is not a pause");
    PauseDetector detector(SLOT_COUNT, GAP_MS);
    connectedReading(detector, SLOT_A, FIRST_PACKET, 0.0);
    connectedReading(detector, SLOT_A, FIRST_PACKET + 1, 10.0);
    disconnectedReading(detector, SLOT_A, 20.0);
    const SlotObservation rejoined = connectedReading(detector, SLOT_A, FIRST_PACKET + 7, 5000.0);
    const SlotObservation next = connectedReading(detector, SLOT_A, FIRST_PACKET + 8, 5010.0);
    const SlotSummary summary = detector.summary(SLOT_A);
    EXPECT(rejoined.change == SlotChange::Connected);
    EXPECT(!rejoined.pause);
    EXPECT(!next.pause);
    EXPECT_EQ(summary.connects, uint64_t{2});
    EXPECT_EQ(summary.updates, uint64_t{2});
    EXPECT_EQ(summary.pausesOverThreshold, uint64_t{0});
    EXPECT_EQ(summary.longestPauseMs, 10.0);
}

static void test_disconnected_slot_ignores_the_packet_number() {
    TEST("pause detector: a disconnected slot yields nothing whatever packet number it carries");
    PauseDetector detector(SLOT_COUNT, GAP_MS);
    for (int i = 0; i < 10; ++i) {
        const SlotObservation seen = detector.observe(SLOT_B, false, 1000u + i, i * 100.0);
        EXPECT(seen.change == SlotChange::None);
        EXPECT(!seen.pause);
    }
    EXPECT_EQ(detector.summary(SLOT_B).updates, uint64_t{0});
    EXPECT_EQ(detector.summary(SLOT_B).connects, uint64_t{0});
}

static void test_slots_are_independent() {
    TEST("pause detector: a pause on one slot does not touch the others");
    PauseDetector detector(SLOT_COUNT, GAP_MS);
    connectedReading(detector, SLOT_A, FIRST_PACKET, 0.0);
    connectedReading(detector, SLOT_B, FIRST_PACKET, 0.0);
    connectedReading(detector, SLOT_A, FIRST_PACKET + 1, 8.0);
    const SlotObservation stalled = connectedReading(detector, SLOT_B, FIRST_PACKET + 1, 226.0);
    connectedReading(detector, SLOT_A, FIRST_PACKET + 2, 16.0);
    EXPECT(stalled.pause.has_value());
    EXPECT_EQ(detector.summary(SLOT_B).longestPauseMs, 226.0);
    EXPECT_EQ(detector.summary(SLOT_A).longestPauseMs, 8.0);
    EXPECT_EQ(detector.summary(SLOT_A).pausesOverThreshold, uint64_t{0});
    EXPECT_EQ(detector.summary(SLOT_A).updates, uint64_t{2});
    EXPECT_EQ(detector.summary(FIRST_UNUSED_SLOT).updates, uint64_t{0});
}

static void test_several_pauses_keep_the_longest() {
    TEST("pause detector: the summary counts every pause and keeps the longest and where it "
         "ended");
    PauseDetector detector(SLOT_COUNT, GAP_MS);
    std::vector<Pause> reported;
    const double times[] = {0.0, 100.0, 108.0, 400.0, 410.0, 470.0};
    uint32_t packet = FIRST_PACKET;
    for (const double nowMs : times) {
        const SlotObservation seen = connectedReading(detector, SLOT_A, packet++, nowMs);
        if (seen.pause) reported.push_back(*seen.pause);
    }
    const SlotSummary summary = detector.summary(SLOT_A);
    EXPECT_EQ(reported.size(), size_t{3});
    EXPECT_EQ(summary.pausesOverThreshold, uint64_t{3});
    EXPECT_EQ(summary.updates, uint64_t{5});
    EXPECT_EQ(summary.longestPauseMs, 292.0);
    EXPECT_EQ(summary.longestPauseEndMs, 400.0);
}

static void test_packet_number_wraparound_is_an_update() {
    TEST("pause detector: the packet number wrapping to zero counts as a change");
    PauseDetector detector(SLOT_COUNT, GAP_MS);
    connectedReading(detector, SLOT_A, UINT32_MAX, 0.0);
    connectedReading(detector, SLOT_A, 0u, 8.0);
    EXPECT_EQ(detector.summary(SLOT_A).updates, uint64_t{1});
}

static void test_unknown_slot_is_ignored() {
    TEST("pause detector: a slot past the end is ignored and summarises as empty");
    PauseDetector detector(SLOT_COUNT, GAP_MS);
    const SlotObservation seen = connectedReading(detector, SLOT_COUNT, FIRST_PACKET, 0.0);
    EXPECT(seen.change == SlotChange::None);
    EXPECT(!seen.pause);
    EXPECT_EQ(detector.summary(SLOT_COUNT).updates, uint64_t{0});
    EXPECT_EQ(detector.slotCount(), SLOT_COUNT);
    EXPECT_EQ(detector.gapMs(), GAP_MS);
}

int main() {
    std::cout << "Running pause detector tests...\n\n";
    test_steady_updates_never_pause();
    test_gap_at_threshold_is_reported();
    test_gap_under_threshold_is_only_the_longest();
    test_unchanged_packet_number_extends_the_pause();
    test_first_reading_only_sets_the_baseline();
    test_connection_changes_are_reported_once();
    test_reconnect_is_not_a_pause();
    test_disconnected_slot_ignores_the_packet_number();
    test_slots_are_independent();
    test_several_pauses_keep_the_longest();
    test_packet_number_wraparound_is_an_update();
    test_unknown_slot_is_ignored();

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
