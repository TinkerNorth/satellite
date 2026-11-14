// SPDX-License-Identifier: LGPL-3.0-or-later
#include "test_util.h"

#include "../src/core/byte_order.h"
#include "../src/core/hidmaestro_device_watch.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <ostream>
#include <string>

namespace satellite {
namespace hidmaestro {

std::ostream& operator<<(std::ostream& os, PadWatch state) { return os << static_cast<int>(state); }

std::ostream& operator<<(std::ostream& os, PadLoss loss) { return os << static_cast<int>(loss); }

std::ostream& operator<<(std::ostream& os, DevnodeCondition condition) {
    return os << static_cast<uint32_t>(condition);
}

std::ostream& operator<<(std::ostream& os, WriteGate gate) { return os << static_cast<int>(gate); }

} // namespace hidmaestro
} // namespace satellite

using namespace satellite::hidmaestro;
using satellite::writeLE32;

namespace {

using WatchImage = std::array<uint8_t, WATCH_BLOCK_SIZE>;

// A small policy, so a test walks a grace in a few steps.
constexpr WatchPolicy SHORT{5, 3, 7};

// What the helper's watcher leaves in a block after a write it finished.
WatchImage blockWith(uint32_t condition, uint32_t devnode, uint32_t problem, uint32_t heartbeat) {
    WatchImage image{};
    writeLE32(image.data() + WATCH_LAYOUT_OFFSET, static_cast<uint32_t>(WATCH_LAYOUT));
    writeLE32(image.data() + WATCH_SEQ_OFFSET, 2);
    writeLE32(image.data() + WATCH_HEARTBEAT_OFFSET, heartbeat);
    writeLE32(image.data() + WATCH_CONDITION_OFFSET, condition);
    writeLE32(image.data() + WATCH_DEVNODE_OFFSET, devnode);
    writeLE32(image.data() + WATCH_PROBLEM_OFFSET, problem);
    return image;
}

WatchReading reading(DevnodeCondition condition, uint32_t heartbeat,
                     WatchedDevnode devnode = WatchedDevnode::Device, uint32_t problem = 0) {
    WatchReading r;
    r.known = true;
    r.stable = true;
    r.heartbeat = heartbeat;
    r.condition = condition;
    r.devnode = devnode;
    r.problem = problem;
    return r;
}

// One tracker stepped through `steps` readings of the same thing, the heartbeat
// moving on each when `beating`.
WatchTracker stepped(WatchTracker t, DevnodeCondition condition, int steps, uint32_t& heartbeat,
                     bool beating) {
    for (int i = 0; i < steps; ++i) {
        if (beating) heartbeat++;
        t = advanceWatch(t, reading(condition, heartbeat), SHORT);
    }
    return t;
}

// A pad the watch has armed: seen whole once, its heartbeat moving.
WatchTracker armed(uint32_t& heartbeat) {
    const WatchTracker start = startWatch(reading(DevnodeCondition::Unknown, heartbeat));
    return stepped(start, DevnodeCondition::Present, 1, heartbeat, true);
}

bool says(const WatchNote& note, LogLevel level, const std::string& needle) {
    return note.level == level && note.text.find(needle) != std::string::npos;
}

void test_the_block_reads_back_what_the_watcher_wrote() {
    TEST("block: a finished write reads back field for field, little-endian at its offsets");
    const WatchImage image =
        blockWith(WATCH_CONDITION_FAILED, WATCH_DEVNODE_HID_COLLECTION, 43, 0x01020304);
    const WatchReading r = readWatchBlock(image.data());
    EXPECT(r.known);
    EXPECT(r.stable);
    EXPECT_EQ(r.heartbeat, 0x01020304u);
    EXPECT_EQ(r.condition, DevnodeCondition::Failed);
    EXPECT(r.devnode == WatchedDevnode::HidCollection);
    EXPECT_EQ(r.problem, 43u);

    TEST("block: the layout constants are the C# writer's (DeviceWatchBlock, held by its test)");
    EXPECT_EQ(WATCH_BLOCK_SIZE, size_t{64});
    EXPECT_EQ(WATCH_LAYOUT, 1);
    EXPECT_EQ(WATCH_HEARTBEAT_OFFSET, size_t{8});
    EXPECT_EQ(WATCH_PROBLEM_OFFSET, size_t{20});
}

void test_a_block_of_another_layout_is_not_known() {
    TEST("block: a block whose layout is not 1, or no block at all, says nothing");
    WatchImage image = blockWith(WATCH_CONDITION_PRESENT, WATCH_DEVNODE_DEVICE, 0, 9);
    writeLE32(image.data() + WATCH_LAYOUT_OFFSET, 2);
    EXPECT(!readWatchBlock(image.data()).known);
    writeLE32(image.data() + WATCH_LAYOUT_OFFSET, 0);
    EXPECT(!readWatchBlock(image.data()).known);
    EXPECT(!readWatchBlock(nullptr).known);
}

void test_a_write_in_progress_is_not_stable() {
    TEST("block: a write in progress (odd sequence) is no condition, and its heartbeat still "
         "reads");
    WatchImage image = blockWith(WATCH_CONDITION_MISSING, WATCH_DEVNODE_DEVICE, 0, 77);
    writeLE32(image.data() + WATCH_SEQ_OFFSET, 3);
    const WatchReading r = readWatchBlock(image.data());
    EXPECT(r.known);
    EXPECT(!r.stable);
    EXPECT_EQ(r.heartbeat, 77u);
}

void test_an_unknown_condition_or_devnode_reads_as_unknown() {
    TEST("block: a condition or devnode code this side does not know reads as unknown");
    const WatchImage image = blockWith(99, 42, 0, 1);
    const WatchReading r = readWatchBlock(image.data());
    EXPECT_EQ(r.condition, DevnodeCondition::Unknown);
    EXPECT(r.devnode == WatchedDevnode::Device);
}

void test_a_umdf2_pad_waits_the_sdks_start_budget() {
    TEST("policy: a UMDF2 pad waits the SDK's own start budget, 15 s, lengthened as the SDK "
         "lengthens it");
    EXPECT_EQ(watchPolicyFor(ProfileBackend::Umdf2, 1.0).graceTicks, 150u);
    EXPECT_EQ(watchPolicyFor(ProfileBackend::Umdf2, 2.0).graceTicks, 300u);
    EXPECT_EQ(watchPolicyFor(ProfileBackend::Umdf2, 500.0).graceTicks, 150u);
    TEST("policy: a timeout scale below 1 never shortens a UMDF2 pad's grace: Windows restarting "
         "a failed host does not run on the SDK's clock");
    EXPECT_EQ(watchPolicyFor(ProfileBackend::Umdf2, 0.1).graceTicks, 150u);
    EXPECT_EQ(watchPolicyFor(ProfileBackend::Umdf2, 0.5).graceTicks, 150u);
    EXPECT_EQ(watchPolicyFor(ProfileBackend::Umdf2, 0.99).graceTicks, 150u);
}

void test_a_composite_waits_for_usbip_win2s_first_reattach() {
    TEST("policy: a composite waits 45 s, usbip-win2's first reattach at 30 s and the import "
         "after it, whatever the timeout scale");
    EXPECT_EQ(watchPolicyFor(ProfileBackend::Usbip, 1.0).graceTicks, 450u);
    EXPECT_EQ(watchPolicyFor(ProfileBackend::Usbip, 0.1).graceTicks, 450u);
    EXPECT_EQ(watchPolicyFor(ProfileBackend::Usbip, 3.0).graceTicks, 450u);
}

void test_a_silent_helper_is_given_up_after_ten_missed_beats() {
    TEST("policy: a helper is given up after ten of its beats go missing, 5 s");
    EXPECT_EQ(watchPolicyFor(ProfileBackend::Umdf2, 1.0).helperSilenceTicks, 50u);
    EXPECT_EQ(watchPolicyFor(ProfileBackend::Usbip, 1.0).helperSilenceTicks, 50u);
    TEST("policy: a pad not armed 30 s after its plug is named once");
    EXPECT_EQ(watchPolicyFor(ProfileBackend::Umdf2, 1.0).unarmedNoticeTicks, 300u);
    TEST("policy: a span that is not whole ticks rounds up, and none is under one tick");
    EXPECT_EQ(watchTicks(std::chrono::milliseconds(150)), 2u);
    EXPECT_EQ(watchTicks(std::chrono::milliseconds(0)), 1u);
}

void test_a_pad_is_unwatched_until_it_is_seen_whole() {
    TEST("watch: a pad is unwatched while the helper has not looked, or has not yet found every "
         "devnode present");
    uint32_t heartbeat = 1;
    WatchTracker t = startWatch(reading(DevnodeCondition::Unknown, heartbeat));
    EXPECT_EQ(t.state, PadWatch::Unwatched);
    t = stepped(t, DevnodeCondition::Unknown, 2, heartbeat, true);
    EXPECT_EQ(t.state, PadWatch::Unwatched);
    t = stepped(t, DevnodeCondition::Missing, SHORT.graceTicks * 2, heartbeat, true);
    EXPECT_EQ(t.state, PadWatch::Unwatched);
    EXPECT_EQ(t.loss, PadLoss::None);

    TEST("watch: the first reading of every devnode present arms the watch");
    t = stepped(t, DevnodeCondition::Present, 1, heartbeat, true);
    EXPECT_EQ(t.state, PadWatch::Present);
}

void test_a_pad_absent_within_its_grace_comes_back() {
    TEST("watch: a present pad whose devnode goes missing is absent, counting its grace");
    uint32_t heartbeat = 1;
    WatchTracker t = armed(heartbeat);
    t = stepped(t, DevnodeCondition::Missing, 1, heartbeat, true);
    EXPECT_EQ(t.state, PadWatch::Absent);
    EXPECT_EQ(t.absentTicks, 1u);
    t = stepped(t, DevnodeCondition::Missing, SHORT.graceTicks - 2, heartbeat, true);
    EXPECT_EQ(t.state, PadWatch::Absent);
    EXPECT_EQ(t.absentTicks, SHORT.graceTicks - 1);

    TEST("watch: a pad back within its grace is present again, and its count starts over");
    t = stepped(t, DevnodeCondition::Present, 1, heartbeat, true);
    EXPECT_EQ(t.state, PadWatch::Present);
    EXPECT_EQ(t.absentTicks, 0u);
    t = stepped(t, DevnodeCondition::Missing, SHORT.graceTicks - 1, heartbeat, true);
    EXPECT_EQ(t.state, PadWatch::Absent);
}

void test_a_pad_absent_for_its_whole_grace_is_lost() {
    TEST("watch: a pad absent for its whole grace is lost to its device, and not a step before");
    uint32_t heartbeat = 1;
    WatchTracker t = armed(heartbeat);
    t = stepped(t, DevnodeCondition::Missing, SHORT.graceTicks - 1, heartbeat, true);
    EXPECT_EQ(t.state, PadWatch::Absent);
    t = stepped(t, DevnodeCondition::Missing, 1, heartbeat, true);
    EXPECT_EQ(t.state, PadWatch::Lost);
    EXPECT_EQ(t.loss, PadLoss::Device);
    EXPECT(watchLost(t));
}

void test_every_kind_of_absence_counts_toward_the_grace() {
    TEST("watch: a failed, leaving or unstarted devnode counts toward the same grace as a "
         "missing one");
    for (const DevnodeCondition condition :
         {DevnodeCondition::Failed, DevnodeCondition::Leaving, DevnodeCondition::Starting}) {
        uint32_t heartbeat = 1;
        WatchTracker t = armed(heartbeat);
        t = stepped(t, condition, SHORT.graceTicks - 1, heartbeat, true);
        EXPECT_EQ(t.state, PadWatch::Absent);
        t = stepped(t, condition, 1, heartbeat, true);
        EXPECT_EQ(t.state, PadWatch::Lost);
        EXPECT_EQ(t.loss, PadLoss::Device);
    }
}

void test_a_disabled_pad_is_held_and_never_lost_to_it() {
    TEST("watch: a pad disabled in Windows is disabled at once, holds its input, and is never "
         "lost however long it stays disabled");
    uint32_t heartbeat = 1;
    WatchTracker t = armed(heartbeat);
    t = stepped(t, DevnodeCondition::Disabled, 1, heartbeat, true);
    EXPECT_EQ(t.state, PadWatch::Disabled);
    EXPECT(watchHoldsInput(t));
    t = stepped(t, DevnodeCondition::Disabled, SHORT.graceTicks * 4, heartbeat, true);
    EXPECT_EQ(t.state, PadWatch::Disabled);
    EXPECT(!watchLost(t));

    TEST("watch: a disabled pad that is enabled and started again is present, and writes again");
    t = stepped(t, DevnodeCondition::Present, 1, heartbeat, true);
    EXPECT_EQ(t.state, PadWatch::Present);
    EXPECT(!watchHoldsInput(t));
}

void test_a_pad_disabled_before_it_was_armed_is_disabled() {
    TEST("watch: a pad whose devnode is disabled before the watch armed is disabled all the same");
    uint32_t heartbeat = 1;
    WatchTracker t = startWatch(reading(DevnodeCondition::Unknown, heartbeat));
    t = stepped(t, DevnodeCondition::Disabled, 1, heartbeat, true);
    EXPECT_EQ(t.state, PadWatch::Disabled);
}

void test_a_disabled_pad_that_then_goes_missing_starts_its_grace() {
    TEST("watch: a disabled pad whose devnode then goes missing is absent from that step on");
    uint32_t heartbeat = 1;
    WatchTracker t = armed(heartbeat);
    t = stepped(t, DevnodeCondition::Disabled, 3, heartbeat, true);
    t = stepped(t, DevnodeCondition::Missing, 1, heartbeat, true);
    EXPECT_EQ(t.state, PadWatch::Absent);
    EXPECT_EQ(t.absentTicks, 1u);
    EXPECT(!watchHoldsInput(t));
}

void test_a_helper_that_stops_beating_is_given_up() {
    TEST("watch: a helper whose heartbeat moved and then stopped is gone after the silence, and "
         "not a step before");
    uint32_t heartbeat = 1;
    WatchTracker t = armed(heartbeat);
    t = stepped(t, DevnodeCondition::Present, SHORT.helperSilenceTicks - 1, heartbeat, false);
    EXPECT_EQ(t.state, PadWatch::Present);
    t = stepped(t, DevnodeCondition::Present, 1, heartbeat, false);
    EXPECT_EQ(t.state, PadWatch::Lost);
    EXPECT_EQ(t.loss, PadLoss::Helper);

    TEST("watch: a beat resets the silence");
    uint32_t again = 1;
    WatchTracker u = armed(again);
    u = stepped(u, DevnodeCondition::Present, SHORT.helperSilenceTicks - 1, again, false);
    u = stepped(u, DevnodeCondition::Present, 1, again, true);
    u = stepped(u, DevnodeCondition::Present, SHORT.helperSilenceTicks - 1, again, false);
    EXPECT_EQ(u.state, PadWatch::Present);
}

void test_a_heartbeat_that_never_moved_never_gives_the_helper_up() {
    TEST("watch: a heartbeat that has not moved since the plug never gives the helper up");
    uint32_t heartbeat = 5;
    WatchTracker t = startWatch(reading(DevnodeCondition::Present, heartbeat));
    t = stepped(t, DevnodeCondition::Present, SHORT.helperSilenceTicks * 4, heartbeat, false);
    EXPECT_EQ(t.state, PadWatch::Present);
    EXPECT(!t.beating);
}

void test_a_helper_gone_takes_unwatched_and_disabled_pads_too() {
    TEST("watch: a helper gone takes a pad the watch never armed");
    uint32_t heartbeat = 1;
    WatchTracker t = startWatch(reading(DevnodeCondition::Unknown, heartbeat));
    t = stepped(t, DevnodeCondition::Missing, 2, heartbeat, true);
    t = stepped(t, DevnodeCondition::Missing, SHORT.helperSilenceTicks, heartbeat, false);
    EXPECT_EQ(t.state, PadWatch::Lost);
    EXPECT_EQ(t.loss, PadLoss::Helper);

    TEST("watch: a helper gone takes a disabled pad too, since nothing can enable it again");
    uint32_t beat = 1;
    WatchTracker d = armed(beat);
    d = stepped(d, DevnodeCondition::Disabled, 1, beat, true);
    d = stepped(d, DevnodeCondition::Disabled, SHORT.helperSilenceTicks, beat, false);
    EXPECT_EQ(d.state, PadWatch::Lost);
    EXPECT_EQ(d.loss, PadLoss::Helper);
}

void test_a_lost_pad_stays_lost() {
    TEST("watch: a lost pad stays lost whatever the helper reports after");
    uint32_t heartbeat = 1;
    WatchTracker t = armed(heartbeat);
    t = stepped(t, DevnodeCondition::Missing, SHORT.graceTicks, heartbeat, true);
    EXPECT_EQ(t.state, PadWatch::Lost);
    t = stepped(t, DevnodeCondition::Present, 3, heartbeat, true);
    EXPECT_EQ(t.state, PadWatch::Lost);
    EXPECT_EQ(t.loss, PadLoss::Device);
}

void test_an_unstable_reading_leaves_the_pad_as_it_was() {
    TEST("watch: a reading taken mid-write leaves the pad as it was, and its heartbeat still "
         "counts");
    uint32_t heartbeat = 1;
    WatchTracker t = armed(heartbeat);
    t = stepped(t, DevnodeCondition::Missing, 1, heartbeat, true);
    WatchReading torn = reading(DevnodeCondition::Present, ++heartbeat);
    torn.stable = false;
    t = advanceWatch(t, torn, SHORT);
    EXPECT_EQ(t.state, PadWatch::Absent);
    EXPECT_EQ(t.absentTicks, 2u);
    EXPECT_EQ(t.silentTicks, 0u);
}

void test_a_block_of_another_layout_leaves_the_pad_unwatched() {
    TEST("watch: a block of another layout leaves the pad unwatched, its heartbeat unheard");
    WatchReading other = reading(DevnodeCondition::Present, 1);
    other.known = false;
    WatchTracker t = startWatch(other);
    for (uint32_t i = 2; i < 2 + SHORT.helperSilenceTicks * 2; ++i) {
        other.heartbeat = i;
        t = advanceWatch(t, other, SHORT);
    }
    EXPECT_EQ(t.state, PadWatch::Unwatched);
    EXPECT(!t.beating);
}

void test_only_a_disabled_pad_holds_its_input() {
    TEST("watch: only a disabled pad holds its input; an absent or lost one is still written");
    WatchTracker t;
    const struct {
        PadWatch state;
        bool holds;
        bool lost;
    } cases[] = {{PadWatch::Unwatched, false, false},
                 {PadWatch::Present, false, false},
                 {PadWatch::Absent, false, false},
                 {PadWatch::Disabled, true, false},
                 {PadWatch::Lost, false, true}};
    for (const auto& c : cases) {
        t.state = c.state;
        EXPECT_EQ(watchHoldsInput(t), c.holds);
        EXPECT_EQ(watchLost(t), c.lost);
    }
}

void test_each_step_says_what_changed() {
    uint32_t heartbeat = 1;
    const WatchTracker start = startWatch(reading(DevnodeCondition::Unknown, heartbeat));
    const WatchTracker present = stepped(start, DevnodeCondition::Present, 1, heartbeat, true);
    const WatchPolicy umdf2 = watchPolicyFor(ProfileBackend::Umdf2, 1.0);

    TEST("note: arming says the watch has the pad's devnodes");
    EXPECT(says(noteWatchStep(3, start, present, umdf2), LogLevel::INFO, "Serial 3"));
    EXPECT(says(noteWatchStep(3, start, present, umdf2), LogLevel::INFO, "watches"));

    TEST("note: a step that changes nothing says nothing");
    EXPECT(noteWatchStep(3, present, present, umdf2).text.empty());

    TEST("note: an absence names the devnode, what Windows says of it and the grace");
    WatchTracker absent = present;
    absent.state = PadWatch::Absent;
    absent.absentTicks = 1;
    absent.condition = DevnodeCondition::Failed;
    absent.devnode = WatchedDevnode::HidCollection;
    absent.problem = 43;
    const WatchNote gone = noteWatchStep(3, present, absent, umdf2);
    EXPECT(says(gone, LogLevel::WARN, "HID collection"));
    EXPECT(says(gone, LogLevel::WARN, "problem code 43"));
    EXPECT(says(gone, LogLevel::WARN, "15 s"));

    TEST("note: a return says how long the devnode was away, and that the pad carries on");
    WatchTracker away = absent;
    away.absentTicks = 12;
    const WatchNote back = noteWatchStep(3, away, present, umdf2);
    EXPECT(says(back, LogLevel::INFO, "1.2 s"));
    EXPECT(says(back, LogLevel::INFO, "carries on"));

    TEST("note: a loss to the device says the controller is given up for its client to plug "
         "again");
    WatchTracker lost = absent;
    lost.state = PadWatch::Lost;
    lost.loss = PadLoss::Device;
    const WatchNote device = noteWatchStep(3, absent, lost, umdf2);
    EXPECT(says(device, LogLevel::WARN, "gives the controller up"));
    EXPECT(says(device, LogLevel::WARN, "plugs it again"));

    TEST("note: a loss to the helper names the helper");
    WatchTracker silent = present;
    silent.state = PadWatch::Lost;
    silent.loss = PadLoss::Helper;
    EXPECT(says(noteWatchStep(3, present, silent, umdf2), LogLevel::WARN, "helper"));

    TEST("note: a disable names Device Manager's code, and says the slot is kept");
    WatchTracker disabled = present;
    disabled.state = PadWatch::Disabled;
    disabled.condition = DevnodeCondition::Disabled;
    disabled.problem = 22;
    const WatchNote off = noteWatchStep(3, present, disabled, umdf2);
    EXPECT(says(off, LogLevel::WARN, "disabled in Windows"));
    EXPECT(says(off, LogLevel::WARN, "Code 22"));
    EXPECT(says(off, LogLevel::WARN, "keeps its slot"));

    TEST("note: an enable says Satellite writes the pad again");
    EXPECT(says(noteWatchStep(3, disabled, present, umdf2), LogLevel::INFO, "enabled"));

    TEST("note: a pad not armed is named once, when its notice comes due");
    WatchTracker waiting = start;
    waiting.unwatchedTicks = umdf2.unarmedNoticeTicks - 1;
    WatchTracker due = waiting;
    due.unwatchedTicks = umdf2.unarmedNoticeTicks;
    WatchTracker past = due;
    past.unwatchedTicks = umdf2.unarmedNoticeTicks + 1;
    EXPECT(says(noteWatchStep(3, waiting, due, umdf2), LogLevel::WARN, "does not watch"));
    EXPECT(noteWatchStep(3, due, past, umdf2).text.empty());
}

void test_the_version_gate_and_the_watch_combine() {
    TEST("gate: a refused version wins, a held one holds, and an open one holds only while the "
         "watch holds the pad");
    EXPECT_EQ(combineGates(WriteGate::Refused, false), WriteGate::Refused);
    EXPECT_EQ(combineGates(WriteGate::Refused, true), WriteGate::Refused);
    EXPECT_EQ(combineGates(WriteGate::Held, false), WriteGate::Held);
    EXPECT_EQ(combineGates(WriteGate::Held, true), WriteGate::Held);
    EXPECT_EQ(combineGates(WriteGate::Open, false), WriteGate::Open);
    EXPECT_EQ(combineGates(WriteGate::Open, true), WriteGate::Held);
}

void test_the_hello_settles_the_protocol() {
    TEST("hello: satellite offers the device watch's protocol");
    EXPECT_EQ(HELPER_PROTOCOL_OFFERED, HELPER_PROTOCOL_DEVICE_WATCH);
    EXPECT_EQ(HELPER_PROTOCOL_DEVICE_WATCH, 2);
    TEST("hello: a helper that names no protocol predates it, and the session stays on 1");
    EXPECT_EQ(settledHelperProtocol(false, 0), HELPER_PROTOCOL_BASE);
    EXPECT_EQ(settledHelperProtocol(false, 2), HELPER_PROTOCOL_BASE);
    TEST("hello: a helper that names 2 watches; a newer one still serves 2; 1 or less is 1");
    EXPECT_EQ(settledHelperProtocol(true, 2), HELPER_PROTOCOL_DEVICE_WATCH);
    EXPECT_EQ(settledHelperProtocol(true, 7), HELPER_PROTOCOL_DEVICE_WATCH);
    EXPECT_EQ(settledHelperProtocol(true, 1), HELPER_PROTOCOL_BASE);
    EXPECT_EQ(settledHelperProtocol(true, 0), HELPER_PROTOCOL_BASE);
    EXPECT_EQ(settledHelperProtocol(true, -3), HELPER_PROTOCOL_BASE);
}

} // namespace

int main() {
    test_the_block_reads_back_what_the_watcher_wrote();
    test_a_block_of_another_layout_is_not_known();
    test_a_write_in_progress_is_not_stable();
    test_an_unknown_condition_or_devnode_reads_as_unknown();
    test_a_umdf2_pad_waits_the_sdks_start_budget();
    test_a_composite_waits_for_usbip_win2s_first_reattach();
    test_a_silent_helper_is_given_up_after_ten_missed_beats();
    test_a_pad_is_unwatched_until_it_is_seen_whole();
    test_a_pad_absent_within_its_grace_comes_back();
    test_a_pad_absent_for_its_whole_grace_is_lost();
    test_every_kind_of_absence_counts_toward_the_grace();
    test_a_disabled_pad_is_held_and_never_lost_to_it();
    test_a_pad_disabled_before_it_was_armed_is_disabled();
    test_a_disabled_pad_that_then_goes_missing_starts_its_grace();
    test_a_helper_that_stops_beating_is_given_up();
    test_a_heartbeat_that_never_moved_never_gives_the_helper_up();
    test_a_helper_gone_takes_unwatched_and_disabled_pads_too();
    test_a_lost_pad_stays_lost();
    test_an_unstable_reading_leaves_the_pad_as_it_was();
    test_a_block_of_another_layout_leaves_the_pad_unwatched();
    test_only_a_disabled_pad_holds_its_input();
    test_each_step_says_what_changed();
    test_the_version_gate_and_the_watch_combine();
    test_the_hello_settles_the_protocol();

    std::cout << "hidmaestro_device_watch: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
