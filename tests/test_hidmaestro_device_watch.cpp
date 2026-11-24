// SPDX-License-Identifier: LGPL-3.0-or-later
#include "test_util.h"

#include "../src/core/byte_order.h"
#include "../src/core/hidmaestro_device_watch.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

namespace satellite {
namespace hidmaestro {

std::ostream& operator<<(std::ostream& os, PadWatch state) { return os << static_cast<int>(state); }

std::ostream& operator<<(std::ostream& os, PadLoss loss) { return os << static_cast<int>(loss); }

std::ostream& operator<<(std::ostream& os, DevnodeCondition condition) {
    return os << static_cast<uint32_t>(condition);
}

std::ostream& operator<<(std::ostream& os, Presentation part) {
    return os << static_cast<uint32_t>(part);
}

std::ostream& operator<<(std::ostream& os, WriteGate gate) { return os << static_cast<int>(gate); }

} // namespace hidmaestro
} // namespace satellite

using namespace satellite::hidmaestro;
using satellite::writeLE32;

namespace {

using WatchImage = std::array<uint8_t, WATCH_BLOCK_SIZE>;

// A small policy, so a test walks a grace in a few steps.
constexpr WatchPolicy SHORT{5, 3, 7, 4};

PresentationReading part(DevnodeCondition condition,
                         WatchedDevnode devnode = WatchedDevnode::Device, uint32_t problem = 0) {
    return {condition, devnode, problem};
}

const PresentationReading PRESENT = part(DevnodeCondition::Present);
const PresentationReading UNKNOWN = part(DevnodeCondition::Unknown);

PresentationReading disabled(WatchedDevnode devnode) {
    return part(DevnodeCondition::Disabled, devnode, 22);
}

// The looks of the three kinds of pad the SDK makes: a UMDF2 pad, a profile
// with an XUSB companion, and a composite with its audio function.
WatchLook sonyLook(PresentationReading hid) {
    WatchLook look;
    look.presents = WATCH_PRESENTS_HID;
    look.hid = hid;
    return look;
}

WatchLook xboxLook(PresentationReading hid, PresentationReading xinput) {
    WatchLook look = sonyLook(hid);
    look.presents |= WATCH_PRESENTS_XINPUT;
    look.xinput = xinput;
    return look;
}

WatchLook compositeLook(PresentationReading hid, PresentationReading audio) {
    WatchLook look = sonyLook(hid);
    look.presents |= WATCH_PRESENTS_AUDIO;
    look.audio = audio;
    return look;
}

void writePart(WatchImage& image, size_t offset, const PresentationReading& p) {
    writeLE32(image.data() + offset + WATCH_FIELD_CONDITION_OFFSET,
              static_cast<uint32_t>(p.condition));
    writeLE32(image.data() + offset + WATCH_FIELD_DEVNODE_OFFSET, static_cast<uint32_t>(p.devnode));
    writeLE32(image.data() + offset + WATCH_FIELD_PROBLEM_OFFSET, p.problem);
}

// What the helper's watcher leaves in a block after a write it finished.
WatchImage blockWith(const WatchLook& look, uint32_t heartbeat, uint32_t seq = 2) {
    WatchImage image{};
    writeLE32(image.data() + WATCH_LAYOUT_OFFSET, static_cast<uint32_t>(WATCH_LAYOUT));
    writeLE32(image.data() + WATCH_SEQ_OFFSET, seq);
    writeLE32(image.data() + WATCH_HEARTBEAT_OFFSET, heartbeat);
    writeLE32(image.data() + WATCH_PRESENTS_OFFSET, look.presents);
    writePart(image, WATCH_HID_OFFSET, look.hid);
    writePart(image, WATCH_XINPUT_OFFSET, look.xinput);
    writePart(image, WATCH_AUDIO_OFFSET, look.audio);
    return image;
}

// The helper as a test drives it: each step its watcher may look (the
// sequence moves) and its heartbeat may beat.
struct Helper {
    uint32_t heartbeat = 0;
    uint32_t seq = 0;
    bool beats = true;
    bool looks = true;

    WatchReading next(const WatchLook& look) {
        if (beats) heartbeat++;
        if (looks) seq += 2;
        WatchReading r;
        r.known = true;
        r.stable = true;
        r.seq = seq;
        r.heartbeat = heartbeat;
        r.look = look;
        return r;
    }
};

WatchTracker steps(WatchTracker t, Helper& helper, const WatchLook& look, int n,
                   const WatchPolicy& policy = SHORT) {
    for (int i = 0; i < n; ++i) t = advanceWatch(t, helper.next(look), policy);
    return t;
}

// A pad the watch has armed: seen taking input once, its heartbeat moving.
WatchTracker armed(Helper& helper, const WatchLook& look) {
    return steps(WatchTracker{}, helper, look, 1);
}

bool says(const WatchNote& note, LogLevel level, const std::string& needle) {
    return note.level == level && note.text.find(needle) != std::string::npos;
}

int count(const std::vector<WatchNote>& notes, LogLevel level, const std::string& needle) {
    int n = 0;
    for (const WatchNote& note : notes) n += says(note, level, needle) ? 1 : 0;
    return n;
}

void test_the_block_reads_back_what_the_watcher_wrote() {
    TEST("block: a finished write reads back presentation for presentation, little-endian at its "
         "offsets");
    WatchLook look = xboxLook(part(DevnodeCondition::Failed, WatchedDevnode::HidCollection, 43),
                              disabled(WatchedDevnode::Companion));
    look.presents |= WATCH_PRESENTS_AUDIO;
    look.audio = part(DevnodeCondition::Missing, WatchedDevnode::AudioFunction);
    const WatchImage image = blockWith(look, 0x01020304, 6);
    const WatchReading r = readWatchBlock(image.data());
    EXPECT(r.known);
    EXPECT(r.stable);
    EXPECT_EQ(r.seq, 6u);
    EXPECT_EQ(r.heartbeat, 0x01020304u);
    EXPECT_EQ(r.look.presents, WATCH_PRESENTS_HID | WATCH_PRESENTS_XINPUT | WATCH_PRESENTS_AUDIO);
    EXPECT_EQ(r.look.hid.condition, DevnodeCondition::Failed);
    EXPECT(r.look.hid.devnode == WatchedDevnode::HidCollection);
    EXPECT_EQ(r.look.hid.problem, 43u);
    EXPECT_EQ(r.look.xinput.condition, DevnodeCondition::Disabled);
    EXPECT(r.look.xinput.devnode == WatchedDevnode::Companion);
    EXPECT_EQ(r.look.xinput.problem, 22u);
    EXPECT_EQ(r.look.audio.condition, DevnodeCondition::Missing);
    EXPECT(r.look.audio.devnode == WatchedDevnode::AudioFunction);

    TEST("block: the layout constants are the C# writer's (DeviceWatchBlock, held by its test)");
    EXPECT_EQ(WATCH_BLOCK_SIZE, size_t{64});
    EXPECT_EQ(WATCH_LAYOUT, 1);
    EXPECT_EQ(WATCH_HEARTBEAT_OFFSET, size_t{8});
    EXPECT_EQ(WATCH_AUDIO_OFFSET + WATCH_PRESENTATION_SIZE, size_t{52});
}

void test_a_block_of_another_layout_is_not_known() {
    TEST("block: a block whose layout is not 1, or no block at all, says nothing");
    WatchImage image = blockWith(sonyLook(PRESENT), 9);
    writeLE32(image.data() + WATCH_LAYOUT_OFFSET, 2);
    EXPECT(!readWatchBlock(image.data()).known);
    writeLE32(image.data() + WATCH_LAYOUT_OFFSET, 0);
    EXPECT(!readWatchBlock(image.data()).known);
    EXPECT(!readWatchBlock(nullptr).known);
}

void test_a_write_in_progress_is_not_stable() {
    TEST("block: a write in progress (odd sequence) is no look, and its heartbeat still reads");
    const WatchImage image = blockWith(sonyLook(part(DevnodeCondition::Missing)), 77, 3);
    const WatchReading r = readWatchBlock(image.data());
    EXPECT(r.known);
    EXPECT(!r.stable);
    EXPECT_EQ(r.heartbeat, 77u);
}

void test_codes_this_side_does_not_know_read_as_unknown() {
    TEST("block: a condition or devnode code this side does not know reads as unknown, a "
         "presentation bit it does not know is dropped, and every pad has its HID one");
    WatchImage image = blockWith(sonyLook(PRESENT), 1);
    writeLE32(image.data() + WATCH_HID_OFFSET + WATCH_FIELD_CONDITION_OFFSET, 99);
    writeLE32(image.data() + WATCH_HID_OFFSET + WATCH_FIELD_DEVNODE_OFFSET, 42);
    writeLE32(image.data() + WATCH_PRESENTS_OFFSET, 0x80);
    const WatchReading r = readWatchBlock(image.data());
    EXPECT_EQ(r.look.hid.condition, DevnodeCondition::Unknown);
    EXPECT(r.look.hid.devnode == WatchedDevnode::Device);
    EXPECT_EQ(r.look.presents, WATCH_PRESENTS_HID);
}

void test_a_pad_with_one_presentation_is_what_it_shows() {
    TEST("verdict: a UMDF2 pad's one presentation decides it: present, disabled or away");
    EXPECT_EQ(inputVerdict(sonyLook(PRESENT)).condition, DevnodeCondition::Present);
    EXPECT_EQ(inputVerdict(sonyLook(disabled(WatchedDevnode::HidCollection))).condition,
              DevnodeCondition::Disabled);
    EXPECT_EQ(inputVerdict(sonyLook(part(DevnodeCondition::Missing))).condition,
              DevnodeCondition::Missing);
    EXPECT_EQ(inputVerdict(sonyLook(UNKNOWN)).condition, DevnodeCondition::Unknown);
}

void test_an_xbox_pad_takes_input_while_either_presentation_does() {
    TEST("verdict: an Xbox pad whose XInput device is disabled still takes input through its HID "
         "game controller, and the other way round");
    const InputVerdict companionOff =
        inputVerdict(xboxLook(PRESENT, disabled(WatchedDevnode::Companion)));
    EXPECT_EQ(companionOff.condition, DevnodeCondition::Present);
    EXPECT_EQ(companionOff.part, Presentation::Hid);
    const InputVerdict hidOff =
        inputVerdict(xboxLook(disabled(WatchedDevnode::HidCollection),
                              part(DevnodeCondition::Present, WatchedDevnode::Companion)));
    EXPECT_EQ(hidOff.condition, DevnodeCondition::Present);
    EXPECT_EQ(hidOff.part, Presentation::Xinput);

    TEST("verdict: only with both disabled is the pad disabled");
    EXPECT_EQ(inputVerdict(
                  xboxLook(disabled(WatchedDevnode::Device), disabled(WatchedDevnode::Companion)))
                  .condition,
              DevnodeCondition::Disabled);

    TEST("verdict: one gone or failed with the other disabled is away, a loss in the making: a "
         "disabled companion no longer outranks a failed parent");
    const InputVerdict failedParent =
        inputVerdict(xboxLook(part(DevnodeCondition::Failed, WatchedDevnode::Device, 43),
                              disabled(WatchedDevnode::Companion)));
    EXPECT_EQ(failedParent.condition, DevnodeCondition::Failed);
    EXPECT_EQ(failedParent.part, Presentation::Hid);
    EXPECT_EQ(failedParent.named.problem, 43u);
    const InputVerdict missingCompanion =
        inputVerdict(xboxLook(disabled(WatchedDevnode::Device),
                              part(DevnodeCondition::Missing, WatchedDevnode::Companion)));
    EXPECT_EQ(missingCompanion.condition, DevnodeCondition::Missing);
    EXPECT_EQ(missingCompanion.part, Presentation::Xinput);

    TEST("verdict: a presentation gone or failed while the other takes input is away too, a loss "
         "in the making: XInput and WGI games lose a pad whose companion failed (Emir's "
         "decision)");
    const InputVerdict companionFailed = inputVerdict(
        xboxLook(PRESENT, part(DevnodeCondition::Failed, WatchedDevnode::Companion, 10)));
    EXPECT_EQ(companionFailed.condition, DevnodeCondition::Failed);
    EXPECT_EQ(companionFailed.part, Presentation::Xinput);
    EXPECT_EQ(inputVerdict(xboxLook(part(DevnodeCondition::Missing),
                                    part(DevnodeCondition::Present, WatchedDevnode::Companion)))
                  .condition,
              DevnodeCondition::Missing);

    TEST("verdict: a presentation not looked at yet leaves the verdict unknown");
    EXPECT_EQ(inputVerdict(xboxLook(disabled(WatchedDevnode::Device), UNKNOWN)).condition,
              DevnodeCondition::Unknown);
    EXPECT_EQ(inputVerdict(xboxLook(PRESENT, UNKNOWN)).condition, DevnodeCondition::Unknown);
}

void test_a_composites_audio_takes_no_input() {
    TEST("verdict: a composite whose HID function is disabled is disabled, whatever its audio "
         "function shows, and one whose audio function is disabled still takes input");
    EXPECT_EQ(inputVerdict(compositeLook(disabled(WatchedDevnode::HidFunction), PRESENT)).condition,
              DevnodeCondition::Disabled);
    EXPECT_EQ(
        inputVerdict(compositeLook(PRESENT, disabled(WatchedDevnode::AudioFunction))).condition,
        DevnodeCondition::Present);
    EXPECT_EQ(
        inputVerdict(
            compositeLook(part(DevnodeCondition::Failed, WatchedDevnode::HidFunction, 10), PRESENT))
            .condition,
        DevnodeCondition::Failed);
    TEST("verdict: a composite whose audio function failed is away, though its input works");
    const InputVerdict audioFailed = inputVerdict(
        compositeLook(PRESENT, part(DevnodeCondition::Failed, WatchedDevnode::AudioFunction, 10)));
    EXPECT_EQ(audioFailed.condition, DevnodeCondition::Failed);
    EXPECT_EQ(audioFailed.part, Presentation::Audio);
}

void test_the_disabled_parts_are_the_presented_ones() {
    TEST("verdict: the disabled parts name each disabled presentation the pad has, and none it "
         "lacks");
    EXPECT_EQ(disabledParts(xboxLook(PRESENT, disabled(WatchedDevnode::Companion))),
              WATCH_PRESENTS_XINPUT);
    EXPECT_EQ(disabledParts(compositeLook(disabled(WatchedDevnode::Device),
                                          disabled(WatchedDevnode::Device))),
              WATCH_PRESENTS_HID | WATCH_PRESENTS_AUDIO);
    WatchLook sony = sonyLook(PRESENT);
    sony.xinput = disabled(WatchedDevnode::Companion);
    sony.audio = disabled(WatchedDevnode::AudioFunction);
    EXPECT_EQ(disabledParts(sony), 0u);
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
    TEST("policy: a helper is gone after ten of its beats' worth of silence, 5 s, and a pad "
         "never seen whole is noted after 30 s");
    EXPECT_EQ(watchPolicyFor(ProfileBackend::Umdf2, 1.0).helperSilenceTicks, 50u);
    EXPECT_EQ(watchPolicyFor(ProfileBackend::Usbip, 1.0).helperSilenceTicks, 50u);
    EXPECT_EQ(watchPolicyFor(ProfileBackend::Umdf2, 1.0).unarmedNoticeTicks, 300u);
}

void test_a_look_is_stale_after_three_of_its_periods() {
    TEST("policy: a look that has not moved for three of the watcher's periods, 1.5 s, is stale");
    EXPECT_EQ(watchPolicyFor(ProfileBackend::Umdf2, 1.0).staleTicks, 15u);
    EXPECT_EQ(watchPolicyFor(ProfileBackend::Usbip, 4.0).staleTicks, 15u);
    EXPECT_EQ(watchTicks(std::chrono::milliseconds(1)), 1u);
    EXPECT_EQ(watchTicks(std::chrono::milliseconds(101)), 2u);
}

void test_a_pad_is_unwatched_until_it_takes_input() {
    TEST("watch: a pad is unwatched while the helper has not looked, or has not found an input "
         "presentation taking input");
    Helper helper;
    WatchTracker t = steps(WatchTracker{}, helper, sonyLook(UNKNOWN), 3);
    EXPECT_EQ(t.state, PadWatch::Unwatched);
    t = steps(t, helper, sonyLook(part(DevnodeCondition::Missing)), 20);
    EXPECT_EQ(t.state, PadWatch::Unwatched);
    EXPECT(!watchLost(t));
    t = steps(t, helper, sonyLook(PRESENT), 1);
    EXPECT_EQ(t.state, PadWatch::Present);

    TEST("watch: an Xbox pad seen with its HID game controller missing is not armed, though its "
         "XInput device takes input, and is once it is whole");
    Helper other;
    const WatchLook halfThere =
        xboxLook(part(DevnodeCondition::Missing),
                 part(DevnodeCondition::Present, WatchedDevnode::Companion));
    WatchTracker xbox = steps(WatchTracker{}, other, halfThere, SHORT.graceTicks * 3);
    EXPECT_EQ(xbox.state, PadWatch::Unwatched);
    xbox = steps(xbox, other, xboxLook(PRESENT, PRESENT), 1);
    EXPECT_EQ(xbox.state, PadWatch::Present);
}

void test_a_pad_absent_within_its_grace_comes_back() {
    TEST("watch: a pad away for less than its grace is absent, not lost, and present again once "
         "it is back");
    Helper helper;
    WatchTracker t = armed(helper, sonyLook(PRESENT));
    t = steps(t, helper, sonyLook(part(DevnodeCondition::Missing)), SHORT.graceTicks - 1);
    EXPECT_EQ(t.state, PadWatch::Absent);
    EXPECT_EQ(t.absentTicks, SHORT.graceTicks - 1);
    t = steps(t, helper, sonyLook(PRESENT), 1);
    EXPECT_EQ(t.state, PadWatch::Present);
    EXPECT_EQ(t.absentTicks, 0u);

    TEST("watch: the grace counts ticks, not looks: a look every few ticks ages the absence on "
         "every tick between them");
    Helper slow;
    WatchTracker u = armed(slow, sonyLook(PRESENT));
    for (uint32_t i = 0; i < SHORT.graceTicks; ++i) {
        slow.looks = i % 3 == 0;
        u = advanceWatch(u, slow.next(sonyLook(part(DevnodeCondition::Missing))), SHORT);
    }
    EXPECT_EQ(u.state, PadWatch::Lost);
}

void test_a_pad_absent_for_its_whole_grace_is_lost() {
    TEST("watch: a pad away for its whole grace is lost to its device");
    Helper helper;
    WatchTracker t = armed(helper, sonyLook(PRESENT));
    t = steps(t, helper,
              sonyLook(part(DevnodeCondition::Failed, WatchedDevnode::HidCollection, 43)),
              SHORT.graceTicks);
    EXPECT_EQ(t.state, PadWatch::Lost);
    EXPECT_EQ(t.loss, PadLoss::Device);
}

void test_every_kind_of_absence_counts_toward_the_grace() {
    TEST("watch: missing, failed, leaving and not started all count toward the grace");
    for (const DevnodeCondition c : {DevnodeCondition::Missing, DevnodeCondition::Failed,
                                     DevnodeCondition::Leaving, DevnodeCondition::Starting}) {
        Helper helper;
        WatchTracker t = armed(helper, sonyLook(PRESENT));
        t = steps(t, helper, sonyLook(part(c)), SHORT.graceTicks);
        EXPECT_EQ(t.state, PadWatch::Lost);
    }
}

void test_a_disabled_pad_is_held_and_never_lost_to_it() {
    TEST("watch: a pad whose every input presentation is disabled is held however long it stays "
         "so, and present again once enabled");
    Helper helper;
    WatchTracker t = armed(helper, sonyLook(PRESENT));
    t = steps(t, helper, sonyLook(disabled(WatchedDevnode::HidCollection)), 10 * SHORT.graceTicks);
    EXPECT_EQ(t.state, PadWatch::Disabled);
    EXPECT(watchHoldsInput(t));
    EXPECT(!watchLost(t));
    t = steps(t, helper, sonyLook(PRESENT), 1);
    EXPECT_EQ(t.state, PadWatch::Present);
    EXPECT(!watchHoldsInput(t));
}

void test_a_pad_disabled_before_it_was_armed_is_disabled() {
    TEST("watch: a pad first seen disabled is held at once");
    Helper helper;
    const WatchTracker t =
        steps(WatchTracker{}, helper, sonyLook(disabled(WatchedDevnode::Device)), 1);
    EXPECT_EQ(t.state, PadWatch::Disabled);
}

void test_a_disabled_pad_that_then_goes_missing_starts_its_grace() {
    TEST("watch: a disabled pad that then goes missing starts its grace there");
    Helper helper;
    WatchTracker t = armed(helper, sonyLook(PRESENT));
    t = steps(t, helper, sonyLook(disabled(WatchedDevnode::Device)), 3);
    t = steps(t, helper, sonyLook(part(DevnodeCondition::Missing)), SHORT.graceTicks - 1);
    EXPECT_EQ(t.state, PadWatch::Absent);
    t = steps(t, helper, sonyLook(part(DevnodeCondition::Missing)), 1);
    EXPECT_EQ(t.state, PadWatch::Lost);
}

void test_an_xbox_pad_with_one_presentation_disabled_is_written_on() {
    TEST("watch: an Xbox pad whose XInput device is disabled stays present and is never held or "
         "given up for it");
    Helper helper;
    WatchTracker t = armed(helper, xboxLook(PRESENT, PRESENT));
    t = steps(t, helper, xboxLook(PRESENT, disabled(WatchedDevnode::Companion)),
              10 * SHORT.graceTicks);
    EXPECT_EQ(t.state, PadWatch::Present);
    EXPECT(!watchHoldsInput(t));
    EXPECT(!watchLost(t));
    EXPECT_EQ(disabledParts(t.look), WATCH_PRESENTS_XINPUT);

    TEST("watch: its HID game controller then failing, with the XInput device still disabled, is "
         "a loss after the grace");
    t = steps(t, helper,
              xboxLook(part(DevnodeCondition::Failed, WatchedDevnode::Device, 43),
                       disabled(WatchedDevnode::Companion)),
              SHORT.graceTicks - 1);
    EXPECT_EQ(t.state, PadWatch::Absent);
    t = steps(t, helper,
              xboxLook(part(DevnodeCondition::Failed, WatchedDevnode::Device, 43),
                       disabled(WatchedDevnode::Companion)),
              1);
    EXPECT_EQ(t.state, PadWatch::Lost);
    EXPECT_EQ(t.loss, PadLoss::Device);

    TEST("watch: an Xbox pad whose XUSB companion is gone while its HID game controller takes "
         "input is away, and lost after the grace, so the whole pad is plugged again");
    Helper other;
    WatchTracker u = armed(other, xboxLook(PRESENT, PRESENT));
    const WatchLook companionGone =
        xboxLook(PRESENT, part(DevnodeCondition::Missing, WatchedDevnode::Companion));
    u = steps(u, other, companionGone, SHORT.graceTicks - 1);
    EXPECT_EQ(u.state, PadWatch::Absent);
    u = steps(u, other, companionGone, 1);
    EXPECT_EQ(u.state, PadWatch::Lost);
    EXPECT_EQ(u.loss, PadLoss::Device);

    TEST("watch: a composite whose audio function failed is lost after its grace too");
    Helper composite;
    WatchTracker c = armed(composite, compositeLook(PRESENT, PRESENT));
    c = steps(
        c, composite,
        compositeLook(PRESENT, part(DevnodeCondition::Failed, WatchedDevnode::AudioFunction, 10)),
        SHORT.graceTicks);
    EXPECT_EQ(c.state, PadWatch::Lost);
}

void test_a_helper_that_stops_beating_is_given_up() {
    TEST("watch: a helper whose heartbeat moved and then stops is gone after its silence");
    Helper helper;
    WatchTracker t = armed(helper, sonyLook(PRESENT));
    helper.beats = false;
    t = steps(t, helper, sonyLook(PRESENT), SHORT.helperSilenceTicks - 1);
    EXPECT_EQ(t.state, PadWatch::Present);
    t = steps(t, helper, sonyLook(PRESENT), 1);
    EXPECT_EQ(t.state, PadWatch::Lost);
    EXPECT_EQ(t.loss, PadLoss::Helper);
}

void test_a_heartbeat_that_never_moved_never_gives_the_helper_up() {
    TEST("watch: a heartbeat that never moved since the plug never gives the helper up");
    Helper helper;
    helper.beats = false;
    const WatchTracker t = steps(WatchTracker{}, helper, sonyLook(PRESENT), 100);
    EXPECT_EQ(t.state, PadWatch::Present);
}

void test_a_gone_channel_gives_the_pad_up_whatever_its_heartbeat_did() {
    TEST("watch: a pad whose helper connection is gone is given up at once, though its heartbeat "
         "never moved after the plug, and so is one still unwatched or held");
    for (const WatchLook& look :
         {sonyLook(PRESENT), sonyLook(UNKNOWN), sonyLook(disabled(WatchedDevnode::Device))}) {
        Helper helper;
        helper.beats = false;
        WatchTracker t = steps(WatchTracker{}, helper, look, 1);
        EXPECT(t.state != PadWatch::Lost);
        WatchReading gone = helper.next(look);
        gone.channelGone = true;
        t = advanceWatch(t, gone, SHORT);
        EXPECT_EQ(t.state, PadWatch::Lost);
        EXPECT_EQ(t.loss, PadLoss::Channel);
    }

    TEST("note: a gone connection says so");
    const WatchPolicy umdf2 = watchPolicyFor(ProfileBackend::Umdf2, 1.0);
    Helper helper;
    const WatchTracker was = armed(helper, sonyLook(PRESENT));
    WatchReading gone = helper.next(sonyLook(PRESENT));
    gone.channelGone = true;
    const WatchTracker now = advanceWatch(was, gone, umdf2);
    EXPECT_EQ(count(noteWatchStep(7, was, now, umdf2), LogLevel::WARN,
                    "Serial 7's HIDMaestro helper connection is gone; Satellite gives the "
                    "controller up"),
              1);
}

void test_a_pad_without_a_block_goes_only_with_its_connection() {
    TEST("watch: a pad with no block to read stays as it was through any number of steps, says "
         "nothing, and is lost to its connection alone");
    WatchTracker t;
    int notes = 0;
    for (int i = 0; i < 100; ++i) {
        const WatchTracker next = advanceUnwatched(t, false);
        notes += static_cast<int>(noteWatchStep(3, t, next, SHORT).size());
        t = next;
    }
    EXPECT_EQ(t.state, PadWatch::Unwatched);
    EXPECT_EQ(notes, 0);
    const WatchTracker lost = advanceUnwatched(t, true);
    EXPECT_EQ(lost.state, PadWatch::Lost);
    EXPECT_EQ(lost.loss, PadLoss::Channel);
    EXPECT_EQ(count(noteWatchStep(3, t, lost, SHORT), LogLevel::WARN,
                    "Serial 3's HIDMaestro helper connection is gone"),
              1);
    const WatchTracker still = advanceUnwatched(lost, false);
    EXPECT_EQ(still.state, PadWatch::Lost);
    EXPECT_EQ(still.loss, PadLoss::Channel);
    EXPECT(noteWatchStep(3, lost, still, SHORT).empty());
}

void test_a_helper_gone_takes_unwatched_and_disabled_pads_too() {
    TEST("watch: a helper gone takes a pad still unwatched, and one held disabled (Emir's "
         "decision: nothing could tell Satellite when Windows enabled it again)");
    Helper helper;
    const WatchReading first = helper.next(sonyLook(UNKNOWN));
    WatchReading firstHeld = first;
    firstHeld.look = sonyLook(disabled(WatchedDevnode::Device));
    WatchTracker unwatched = advanceWatch(WatchTracker{}, first, SHORT);
    WatchTracker held = advanceWatch(WatchTracker{}, firstHeld, SHORT);
    EXPECT_EQ(unwatched.state, PadWatch::Unwatched);
    EXPECT_EQ(held.state, PadWatch::Disabled);
    helper.beats = false;
    for (uint32_t i = 0; i < SHORT.helperSilenceTicks; ++i) {
        const WatchReading r = helper.next(sonyLook(UNKNOWN));
        unwatched = advanceWatch(unwatched, r, SHORT);
        WatchReading d = r;
        d.look = sonyLook(disabled(WatchedDevnode::Device));
        held = advanceWatch(held, d, SHORT);
    }
    EXPECT_EQ(unwatched.state, PadWatch::Lost);
    EXPECT_EQ(unwatched.loss, PadLoss::Helper);
    EXPECT_EQ(held.state, PadWatch::Lost);
    EXPECT_EQ(held.loss, PadLoss::Helper);
}

void test_a_lost_pad_stays_lost() {
    TEST("watch: lost is final, whatever the block says after");
    Helper helper;
    WatchTracker t = armed(helper, sonyLook(PRESENT));
    t = steps(t, helper, sonyLook(part(DevnodeCondition::Missing)), SHORT.graceTicks);
    EXPECT_EQ(t.state, PadWatch::Lost);
    t = steps(t, helper, sonyLook(PRESENT), 5);
    EXPECT_EQ(t.state, PadWatch::Lost);
}

void test_an_unstable_reading_leaves_the_pad_as_it_was() {
    TEST("watch: a reading taken mid-write keeps the last look, so it neither starts nor ends an "
         "absence by itself");
    Helper helper;
    WatchTracker t = armed(helper, sonyLook(PRESENT));
    WatchReading r = helper.next(sonyLook(part(DevnodeCondition::Missing)));
    r.stable = false;
    t = advanceWatch(t, r, SHORT);
    EXPECT_EQ(t.state, PadWatch::Present);
    EXPECT_EQ(t.look.hid.condition, DevnodeCondition::Present);
}

void test_a_block_of_another_layout_leaves_the_pad_unwatched() {
    TEST("watch: a block of another layout is no look and no heartbeat, so the pad stays "
         "unwatched and is never given up");
    WatchTracker t;
    WatchReading r;
    r.known = false;
    for (int i = 0; i < 40; ++i) t = advanceWatch(t, r, SHORT);
    EXPECT_EQ(t.state, PadWatch::Unwatched);
}

void test_a_stale_look_decides_nothing() {
    TEST("stale: a pad away whose watcher stops looking is not aged into a loss however long the "
         "look stays stale");
    Helper helper;
    WatchTracker t = armed(helper, sonyLook(PRESENT));
    t = steps(t, helper, sonyLook(part(DevnodeCondition::Missing)), 1);
    helper.looks = false;
    t = steps(t, helper, sonyLook(part(DevnodeCondition::Missing)), 10 * SHORT.graceTicks);
    EXPECT(t.stale);
    EXPECT_EQ(t.state, PadWatch::Absent);
    EXPECT(t.absentTicks < SHORT.graceTicks);

    TEST("stale: a look within the stale bound still ages the absence");
    Helper slow;
    WatchTracker u = armed(slow, sonyLook(PRESENT));
    u = steps(u, slow, sonyLook(part(DevnodeCondition::Missing)), 1);
    slow.looks = false;
    u = steps(u, slow, sonyLook(part(DevnodeCondition::Missing)), SHORT.staleTicks - 1);
    EXPECT(!u.stale);
    EXPECT_EQ(u.absentTicks, SHORT.staleTicks);

    TEST("stale: a look that comes again decides again, and a pad back is present");
    helper.looks = true;
    t = steps(t, helper, sonyLook(PRESENT), 1);
    EXPECT(!t.stale);
    EXPECT_EQ(t.state, PadWatch::Present);

    TEST("stale: a held pad stays held while its look is stale, and a present one present");
    Helper held;
    WatchTracker h = armed(held, sonyLook(disabled(WatchedDevnode::Device)));
    held.looks = false;
    h = steps(h, held, sonyLook(PRESENT), 3 * SHORT.staleTicks);
    EXPECT_EQ(h.state, PadWatch::Disabled);
    Helper calm;
    WatchTracker p = armed(calm, sonyLook(PRESENT));
    calm.looks = false;
    p = steps(p, calm, sonyLook(part(DevnodeCondition::Missing)), 3 * SHORT.staleTicks);
    EXPECT_EQ(p.state, PadWatch::Present);

    TEST("stale: a helper that stops beating is still given up while its look is stale");
    Helper dying;
    WatchTracker d = armed(dying, sonyLook(PRESENT));
    dying.looks = false;
    dying.beats = false;
    d = steps(d, dying, sonyLook(PRESENT), SHORT.helperSilenceTicks);
    EXPECT_EQ(d.state, PadWatch::Lost);
    EXPECT_EQ(d.loss, PadLoss::Helper);
}

void test_only_every_input_presentation_disabled_holds_input() {
    TEST("gate: only a pad whose every input presentation is disabled holds its input");
    Helper helper;
    WatchTracker t = armed(helper, xboxLook(PRESENT, PRESENT));
    EXPECT(!watchHoldsInput(t));
    t = steps(t, helper, xboxLook(disabled(WatchedDevnode::HidCollection), PRESENT), 1);
    EXPECT(!watchHoldsInput(t));
    t = steps(
        t, helper,
        xboxLook(disabled(WatchedDevnode::HidCollection), disabled(WatchedDevnode::Companion)), 1);
    EXPECT(watchHoldsInput(t));
    t = steps(t, helper, xboxLook(part(DevnodeCondition::Missing), PRESENT), 1);
    EXPECT(!watchHoldsInput(t));

    TEST("gate: the microphone is held by the audio function alone");
    Helper composite;
    WatchTracker c = armed(composite, compositeLook(PRESENT, PRESENT));
    EXPECT(!watchHoldsMic(c));
    c = steps(c, composite, compositeLook(disabled(WatchedDevnode::HidFunction), PRESENT), 1);
    EXPECT(watchHoldsInput(c));
    EXPECT(!watchHoldsMic(c));
    c = steps(c, composite, compositeLook(PRESENT, disabled(WatchedDevnode::AudioFunction)), 1);
    EXPECT(!watchHoldsInput(c));
    EXPECT(watchHoldsMic(c));
    c = steps(c, composite,
              compositeLook(disabled(WatchedDevnode::Device), disabled(WatchedDevnode::Device)), 1);
    EXPECT(watchHoldsInput(c));
    EXPECT(watchHoldsMic(c));
}

void test_each_step_says_what_changed() {
    TEST("note: arming, going away, coming back, disabled, enabled and lost each say so once");
    const WatchPolicy umdf2 = watchPolicyFor(ProfileBackend::Umdf2, 1.0);
    Helper helper;
    WatchTracker was;
    WatchTracker now = steps(was, helper, sonyLook(PRESENT), 1, umdf2);
    EXPECT_EQ(count(noteWatchStep(3, was, now, umdf2), LogLevel::INFO,
                    "Satellite watches Serial 3's devices in Windows"),
              1);
    was = now;
    now = steps(was, helper, sonyLook(part(DevnodeCondition::Missing)), 1, umdf2);
    EXPECT_EQ(count(noteWatchStep(3, was, now, umdf2), LogLevel::WARN,
                    "Serial 3's device is not in Windows' device tree; Satellite waits up to 15 s"),
              1);
    was = now;
    now = steps(was, helper, sonyLook(part(DevnodeCondition::Missing)), 1, umdf2);
    EXPECT(noteWatchStep(3, was, now, umdf2).empty());
    was = now;
    now = steps(was, helper, sonyLook(PRESENT), 1, umdf2);
    EXPECT_EQ(count(noteWatchStep(3, was, now, umdf2), LogLevel::INFO,
                    "Serial 3's device is back in Windows after 0.2 s"),
              1);
    was = now;
    now = steps(was, helper, sonyLook(disabled(WatchedDevnode::HidCollection)), 1, umdf2);
    EXPECT_EQ(count(noteWatchStep(3, was, now, umdf2), LogLevel::WARN,
                    "Serial 3's HID collection is disabled in Windows (Code 22); Satellite writes "
                    "nothing"),
              1);
    was = now;
    now = steps(was, helper, sonyLook(PRESENT), 1, umdf2);
    EXPECT_EQ(count(noteWatchStep(3, was, now, umdf2), LogLevel::INFO,
                    "Serial 3's HID collection is enabled in Windows again"),
              1);
    was = now;
    now = steps(was, helper, sonyLook(part(DevnodeCondition::Failed, WatchedDevnode::Device, 43)),
                umdf2.graceTicks, umdf2);
    const WatchTracker before = steps(was, helper, sonyLook(PRESENT), 0, umdf2);
    EXPECT_EQ(now.state, PadWatch::Lost);
    WatchTracker last = now;
    last.state = PadWatch::Absent;
    EXPECT_EQ(count(noteWatchStep(3, last, now, umdf2), LogLevel::WARN,
                    "Serial 3's device is still failed with problem code 43 after 15 s; Satellite "
                    "gives the controller up"),
              1);
    EXPECT_EQ(before.state, PadWatch::Present);

    TEST("note: a helper gone says how long it has been silent");
    WatchTracker gone = now;
    gone.loss = PadLoss::Helper;
    EXPECT_EQ(count(noteWatchStep(3, last, gone, umdf2), LogLevel::WARN,
                    "Serial 3's HIDMaestro helper has not beaten for 5 s"),
              1);

    TEST("note: both of an Xbox pad's input presentations disabled are named together");
    Helper xbox;
    WatchTracker x = armed(xbox, xboxLook(PRESENT, PRESENT));
    const WatchTracker off = steps(
        x, xbox,
        xboxLook(disabled(WatchedDevnode::HidCollection), disabled(WatchedDevnode::Companion)), 1);
    EXPECT_EQ(count(noteWatchStep(4, x, off, umdf2), LogLevel::WARN,
                    "Serial 4's HID collection and XUSB companion are disabled in Windows"),
              1);

    TEST("note: a pad still unwatched is noted once, when its notice comes due");
    WatchTracker due;
    due.unwatchedTicks = umdf2.unarmedNoticeTicks;
    WatchTracker beforeDue;
    beforeDue.unwatchedTicks = umdf2.unarmedNoticeTicks - 1;
    EXPECT_EQ(count(noteWatchStep(3, beforeDue, due, umdf2), LogLevel::WARN,
                    "Satellite has not found Serial 3's game controller taking input"),
              1);
    WatchTracker past = due;
    past.unwatchedTicks++;
    EXPECT(noteWatchStep(3, due, past, umdf2).empty());
}

void test_a_presentation_that_changes_while_another_takes_input_is_noted() {
    TEST("note: an Xbox pad's XInput device disabled while its HID game controller takes input "
         "says so, and that Satellite keeps writing the controller");
    const WatchPolicy umdf2 = watchPolicyFor(ProfileBackend::Umdf2, 1.0);
    Helper helper;
    WatchTracker was = armed(helper, xboxLook(PRESENT, PRESENT));
    WatchTracker now =
        steps(was, helper, xboxLook(PRESENT, disabled(WatchedDevnode::Companion)), 1, umdf2);
    EXPECT_EQ(count(noteWatchStep(3, was, now, umdf2), LogLevel::WARN,
                    "Serial 3's XUSB companion is disabled in Windows (Code 22); its HID game "
                    "controller still takes input, so Satellite keeps writing the controller"),
              1);
    was = now;
    now = steps(was, helper, xboxLook(PRESENT, disabled(WatchedDevnode::Companion)), 1, umdf2);
    EXPECT(noteWatchStep(3, was, now, umdf2).empty());

    TEST("note: enabled again, it says so");
    was = now;
    now = steps(was, helper, xboxLook(PRESENT, PRESENT), 1, umdf2);
    EXPECT_EQ(count(noteWatchStep(3, was, now, umdf2), LogLevel::INFO,
                    "Serial 3's XUSB companion is enabled in Windows again"),
              1);

    TEST("note: one gone while the other takes input starts the grace and names it, and back "
         "says so");
    was = now;
    now = steps(was, helper,
                xboxLook(PRESENT, part(DevnodeCondition::Missing, WatchedDevnode::Companion)), 1,
                umdf2);
    const std::vector<WatchNote> gone = noteWatchStep(3, was, now, umdf2);
    EXPECT_EQ(count(gone, LogLevel::WARN,
                    "Serial 3's XUSB companion is not in Windows' device tree; Satellite waits up "
                    "to 15 s for it to come back"),
              1);
    EXPECT_EQ(gone.size(), size_t{1});
    was = now;
    now = steps(was, helper, xboxLook(PRESENT, PRESENT), 1, umdf2);
    EXPECT_EQ(count(noteWatchStep(3, was, now, umdf2), LogLevel::INFO,
                    "Serial 3's XUSB companion is back in Windows"),
              1);

    TEST("note: the HID game controller disabled says the XInput device still takes input");
    was = now;
    now = steps(was, helper, xboxLook(disabled(WatchedDevnode::HidCollection), PRESENT), 1, umdf2);
    EXPECT_EQ(count(noteWatchStep(3, was, now, umdf2), LogLevel::WARN,
                    "Serial 3's HID collection is disabled in Windows (Code 22); its XInput device "
                    "still takes input"),
              1);

    TEST("note: a presentation already disabled when the pad is armed is noted then");
    Helper late;
    const WatchTracker start;
    const WatchTracker armedOff =
        steps(start, late, xboxLook(PRESENT, disabled(WatchedDevnode::Companion)), 1, umdf2);
    const std::vector<WatchNote> notes = noteWatchStep(5, start, armedOff, umdf2);
    EXPECT_EQ(count(notes, LogLevel::INFO, "Satellite watches Serial 5's devices"), 1);
    EXPECT_EQ(count(notes, LogLevel::WARN, "Serial 5's XUSB companion is disabled in Windows"), 1);
    EXPECT_EQ(notes.size(), size_t{2});
}

void test_the_audio_function_says_what_it_does_to_the_microphone() {
    TEST("note: a composite's audio function disabled holds its microphone, and says so");
    const WatchPolicy usbip = watchPolicyFor(ProfileBackend::Usbip, 1.0);
    Helper helper;
    WatchTracker was = armed(helper, compositeLook(PRESENT, PRESENT));
    WatchTracker now = steps(
        was, helper, compositeLook(PRESENT, disabled(WatchedDevnode::AudioFunction)), 1, usbip);
    EXPECT_EQ(count(noteWatchStep(6, was, now, usbip), LogLevel::WARN,
                    "Serial 6's audio function is disabled in Windows (Code 22); Satellite holds "
                    "its microphone until it is enabled again"),
              1);
    was = now;
    now = steps(was, helper, compositeLook(PRESENT, PRESENT), 1, usbip);
    EXPECT_EQ(count(noteWatchStep(6, was, now, usbip), LogLevel::INFO,
                    "Serial 6's audio function is enabled in Windows again; its microphone "
                    "carries on"),
              1);
    was = now;
    now = steps(
        was, helper,
        compositeLook(PRESENT, part(DevnodeCondition::Missing, WatchedDevnode::AudioFunction)), 1,
        usbip);
    const std::vector<WatchNote> gone = noteWatchStep(6, was, now, usbip);
    EXPECT_EQ(count(gone, LogLevel::WARN,
                    "Serial 6's audio function is not in Windows' device tree; Satellite waits "
                    "up to 45 s"),
              1);
    EXPECT_EQ(gone.size(), size_t{1});
    was = now;
    now = steps(was, helper, compositeLook(PRESENT, PRESENT), 1, usbip);
    EXPECT_EQ(count(noteWatchStep(6, was, now, usbip), LogLevel::INFO,
                    "Serial 6's audio function is back in Windows"),
              1);

    TEST("note: a pad without an audio function never speaks of one");
    Helper sony;
    WatchLook look = sonyLook(PRESENT);
    look.audio = disabled(WatchedDevnode::AudioFunction);
    const WatchTracker s = armed(sony, sonyLook(PRESENT));
    const WatchTracker s2 = steps(s, sony, look, 1, usbip);
    EXPECT_EQ(count(noteWatchStep(6, s, s2, usbip), LogLevel::WARN, "audio function"), 0);
}

void test_a_stale_look_is_noted_and_its_end_too() {
    TEST("note: a look grown stale says the watch decides nothing, once, and a fresh one says "
         "the watch looks again");
    const WatchPolicy umdf2 = watchPolicyFor(ProfileBackend::Umdf2, 1.0);
    Helper helper;
    WatchTracker t = armed(helper, sonyLook(PRESENT));
    helper.looks = false;
    WatchTracker was = steps(t, helper, sonyLook(PRESENT), umdf2.staleTicks - 1, umdf2);
    WatchTracker now = steps(was, helper, sonyLook(PRESENT), 1, umdf2);
    EXPECT_EQ(count(noteWatchStep(2, was, now, umdf2), LogLevel::WARN,
                    "The device watch has not finished a look at Serial 2's devices for 1.5 s; "
                    "Satellite decides nothing about the controller until it does"),
              1);
    was = now;
    now = steps(was, helper, sonyLook(PRESENT), 1, umdf2);
    EXPECT(noteWatchStep(2, was, now, umdf2).empty());
    helper.looks = true;
    was = now;
    now = steps(was, helper, sonyLook(PRESENT), 1, umdf2);
    EXPECT_EQ(count(noteWatchStep(2, was, now, umdf2), LogLevel::INFO,
                    "The device watch looks at Serial 2's devices again"),
              1);

    TEST("note: an unwatched pad's stale look says nothing, its unarmed notice covers it");
    Helper quiet;
    quiet.looks = false;
    const WatchTracker u0;
    const WatchTracker u1 = steps(u0, quiet, sonyLook(UNKNOWN), umdf2.staleTicks, umdf2);
    const WatchTracker u2 = steps(u1, quiet, sonyLook(UNKNOWN), 1, umdf2);
    EXPECT_EQ(count(noteWatchStep(2, u1, u2, umdf2), LogLevel::WARN, "The device watch"), 0);
}

void test_a_look_that_keeps_going_stale_is_logged_less_and_less() {
    TEST("note: a look that goes stale and fresh again and again is logged on its 1st, 2nd, 4th "
         "and 8th time, with the count, so PnP contention cannot flood the log");
    const WatchPolicy umdf2 = watchPolicyFor(ProfileBackend::Umdf2, 1.0);
    Helper helper;
    WatchTracker t = armed(helper, sonyLook(PRESENT));
    std::vector<WatchNote> all;
    for (int round = 0; round < 10; ++round) {
        helper.looks = false;
        for (uint32_t i = 0; i < umdf2.staleTicks; ++i) {
            const WatchTracker was = t;
            t = advanceWatch(t, helper.next(sonyLook(PRESENT)), umdf2);
            for (const WatchNote& n : noteWatchStep(2, was, t, umdf2)) all.push_back(n);
        }
        helper.looks = true;
        const WatchTracker was = t;
        t = advanceWatch(t, helper.next(sonyLook(PRESENT)), umdf2);
        for (const WatchNote& n : noteWatchStep(2, was, t, umdf2)) all.push_back(n);
    }
    EXPECT_EQ(count(all, LogLevel::WARN, "has not finished a look"), 4);
    EXPECT_EQ(count(all, LogLevel::INFO, "looks at Serial 2's devices again"), 4);
    EXPECT_EQ(count(all, LogLevel::WARN, "8 times since the plug"), 1);
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
    return satellite::test::runMain("hidmaestro_device_watch", [] {
        test_the_block_reads_back_what_the_watcher_wrote();
        test_a_block_of_another_layout_is_not_known();
        test_a_write_in_progress_is_not_stable();
        test_codes_this_side_does_not_know_read_as_unknown();
        test_a_pad_with_one_presentation_is_what_it_shows();
        test_an_xbox_pad_takes_input_while_either_presentation_does();
        test_a_composites_audio_takes_no_input();
        test_the_disabled_parts_are_the_presented_ones();
        test_a_umdf2_pad_waits_the_sdks_start_budget();
        test_a_composite_waits_for_usbip_win2s_first_reattach();
        test_a_silent_helper_is_given_up_after_ten_missed_beats();
        test_a_look_is_stale_after_three_of_its_periods();
        test_a_pad_is_unwatched_until_it_takes_input();
        test_a_pad_absent_within_its_grace_comes_back();
        test_a_pad_absent_for_its_whole_grace_is_lost();
        test_every_kind_of_absence_counts_toward_the_grace();
        test_a_disabled_pad_is_held_and_never_lost_to_it();
        test_a_pad_disabled_before_it_was_armed_is_disabled();
        test_a_disabled_pad_that_then_goes_missing_starts_its_grace();
        test_an_xbox_pad_with_one_presentation_disabled_is_written_on();
        test_a_helper_that_stops_beating_is_given_up();
        test_a_heartbeat_that_never_moved_never_gives_the_helper_up();
        test_a_helper_gone_takes_unwatched_and_disabled_pads_too();
        test_a_gone_channel_gives_the_pad_up_whatever_its_heartbeat_did();
        test_a_pad_without_a_block_goes_only_with_its_connection();
        test_a_lost_pad_stays_lost();
        test_an_unstable_reading_leaves_the_pad_as_it_was();
        test_a_block_of_another_layout_leaves_the_pad_unwatched();
        test_a_stale_look_decides_nothing();
        test_only_every_input_presentation_disabled_holds_input();
        test_each_step_says_what_changed();
        test_a_presentation_that_changes_while_another_takes_input_is_noted();
        test_the_audio_function_says_what_it_does_to_the_microphone();
        test_a_stale_look_is_noted_and_its_end_too();
        test_a_look_that_keeps_going_stale_is_logged_less_and_less();
        test_the_version_gate_and_the_watch_combine();
        test_the_hello_settles_the_protocol();
    });
}
