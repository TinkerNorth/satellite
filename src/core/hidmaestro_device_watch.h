// SPDX-License-Identifier: LGPL-3.0-or-later

// The device watch: what Windows has of a HIDMaestro pad's devnodes, which
// nothing in the SDK reports, and what Satellite does about it. The helper
// (helper/hidmaestro-core DeviceWatch) finds each pad's devnodes, looks at them at
// least every WATCH_BEAT_MS and writes what each of the pad's presentations
// shows into one 64-byte block per pad, which satellite.exe maps read-only
// beside the version section. A pad is written while any of its input
// presentations takes input, and one that another still serves is only noted
// when Windows disables it; a pad whose every input presentation is disabled is
// held until one is enabled again. One whose input presentations are gone or
// failed, the rest disabled, past its grace, or whose helper stops beating, is
// given up, so the core unplugs it and its client plugs it again. A composite's
// audio function holds its microphone alone. The C# writer (helper/hidmaestro-core
// DeviceWatchBlock) is held to these constants by its own test, which reads
// them from this file.
#pragma once

#include "core/byte_order.h"
#include "core/hidmaestro_version.h"
#include "core/types.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace satellite {
namespace hidmaestro {

// The hello's protocol. 1 is the pipe as it was before the watch, and a helper
// that predates it names none; a session that settles on 2 gets a watch block
// with every plug, and its unplugs are answered before the controller is gone.
inline constexpr int32_t HELPER_PROTOCOL_BASE = 1;
inline constexpr int32_t HELPER_PROTOCOL_DEVICE_WATCH = 2;
inline constexpr int32_t HELPER_PROTOCOL_OFFERED = HELPER_PROTOCOL_DEVICE_WATCH;

// One block per pad, little-endian, every field a naturally aligned word. The
// watcher writes everything but the heartbeat under the sequence (odd while it
// writes), once per look, so a sequence that stops moving is a watcher that
// stopped looking; the heartbeat has a writer of its own, a thread that makes
// no Plug and Play call, so a watcher stuck in one cannot pass for a dead
// helper. Each presentation is three words: its condition, the devnode the
// condition is about, and that devnode's problem code.
inline constexpr size_t WATCH_BLOCK_SIZE = 64;
inline constexpr size_t WATCH_LAYOUT_OFFSET = 0;
inline constexpr size_t WATCH_SEQ_OFFSET = 4;
inline constexpr size_t WATCH_HEARTBEAT_OFFSET = 8;
inline constexpr size_t WATCH_PRESENTS_OFFSET = 12;
inline constexpr size_t WATCH_HID_OFFSET = 16;
inline constexpr size_t WATCH_XINPUT_OFFSET = 28;
inline constexpr size_t WATCH_AUDIO_OFFSET = 40;
inline constexpr size_t WATCH_PRESENTATION_SIZE = 12;
inline constexpr size_t WATCH_FIELD_CONDITION_OFFSET = 0;
inline constexpr size_t WATCH_FIELD_DEVNODE_OFFSET = 4;
inline constexpr size_t WATCH_FIELD_PROBLEM_OFFSET = 8;
inline constexpr int32_t WATCH_LAYOUT = 1;
inline constexpr int WATCH_READ_ATTEMPTS = 4;
// The helper's heartbeat moves at least this often while it lives, and its
// watcher looks at every pad this often.
inline constexpr int WATCH_BEAT_MS = 500;

// The ways a pad presents itself to Windows, from the SDK's create plan
// (PresentationPlan): every pad has a HID game controller, a profile with an
// XUSB companion an XInput device beside it, and a composite a USB-audio
// function for its microphone and speaker. The first two take input.
inline constexpr uint32_t WATCH_PRESENTS_HID = 1;
inline constexpr uint32_t WATCH_PRESENTS_XINPUT = 2;
inline constexpr uint32_t WATCH_PRESENTS_AUDIO = 4;

// What the watcher found of a presentation: every devnode present and
// started, or the first that is not, with a disabled one ahead of any other.
inline constexpr uint32_t WATCH_CONDITION_UNKNOWN = 0;  // not looked at yet
inline constexpr uint32_t WATCH_CONDITION_PRESENT = 1;  // started, no problem
inline constexpr uint32_t WATCH_CONDITION_DISABLED = 2; // CM_PROB_DISABLED (22)
inline constexpr uint32_t WATCH_CONDITION_MISSING = 3;  // not in the device tree
inline constexpr uint32_t WATCH_CONDITION_FAILED = 4;   // any other problem code
inline constexpr uint32_t WATCH_CONDITION_LEAVING = 5;  // DN_WILL_BE_REMOVED
inline constexpr uint32_t WATCH_CONDITION_STARTING = 6; // present, not started

// Which devnode a condition is about.
inline constexpr uint32_t WATCH_DEVNODE_DEVICE = 0; // the HID parent, or a composite's USB device
inline constexpr uint32_t WATCH_DEVNODE_HID_FUNCTION = 1;   // a composite's HID interface
inline constexpr uint32_t WATCH_DEVNODE_HID_COLLECTION = 2; // the HID child games open
inline constexpr uint32_t WATCH_DEVNODE_COMPANION = 3;      // the XUSB companion
inline constexpr uint32_t WATCH_DEVNODE_AUDIO_FUNCTION = 4; // a composite's USB-audio function

enum class Presentation : uint32_t {
    Hid = WATCH_PRESENTS_HID,
    Xinput = WATCH_PRESENTS_XINPUT,
    Audio = WATCH_PRESENTS_AUDIO,
};

enum class DevnodeCondition : uint32_t {
    Unknown = WATCH_CONDITION_UNKNOWN,
    Present = WATCH_CONDITION_PRESENT,
    Disabled = WATCH_CONDITION_DISABLED,
    Missing = WATCH_CONDITION_MISSING,
    Failed = WATCH_CONDITION_FAILED,
    Leaving = WATCH_CONDITION_LEAVING,
    Starting = WATCH_CONDITION_STARTING,
};

enum class WatchedDevnode : uint32_t {
    Device = WATCH_DEVNODE_DEVICE,
    HidFunction = WATCH_DEVNODE_HID_FUNCTION,
    HidCollection = WATCH_DEVNODE_HID_COLLECTION,
    Companion = WATCH_DEVNODE_COMPANION,
    AudioFunction = WATCH_DEVNODE_AUDIO_FUNCTION,
};

struct WatchBlock {
    uint32_t layout = 0;
    uint32_t seq = 0;
    uint32_t heartbeat = 0;
    uint32_t presents = 0;
    uint32_t hid[3] = {};
    uint32_t xinput[3] = {};
    uint32_t audio[3] = {};
    uint32_t reserved[3] = {};
};

static_assert(sizeof(WatchBlock) == WATCH_BLOCK_SIZE);
static_assert(offsetof(WatchBlock, layout) == WATCH_LAYOUT_OFFSET);
static_assert(offsetof(WatchBlock, seq) == WATCH_SEQ_OFFSET);
static_assert(offsetof(WatchBlock, heartbeat) == WATCH_HEARTBEAT_OFFSET);
static_assert(offsetof(WatchBlock, presents) == WATCH_PRESENTS_OFFSET);
static_assert(offsetof(WatchBlock, hid) == WATCH_HID_OFFSET);
static_assert(offsetof(WatchBlock, xinput) == WATCH_XINPUT_OFFSET);
static_assert(offsetof(WatchBlock, audio) == WATCH_AUDIO_OFFSET);
static_assert(sizeof(WatchBlock::hid) == WATCH_PRESENTATION_SIZE);
static_assert(WATCH_FIELD_PROBLEM_OFFSET + sizeof(uint32_t) == WATCH_PRESENTATION_SIZE);

struct PresentationReading {
    DevnodeCondition condition = DevnodeCondition::Unknown;
    WatchedDevnode devnode = WatchedDevnode::Device;
    uint32_t problem = 0;
};

// What one finished look of the watcher's found.
struct WatchLook {
    // The presentations the pad has, WATCH_PRESENTS_* bits.
    uint32_t presents = 0;
    PresentationReading hid;
    PresentationReading xinput;
    PresentationReading audio;
};

struct WatchReading {
    // The block is layout 1; nothing else in a reading counts otherwise.
    bool known = false;
    // The look came from one finished write.
    bool stable = false;
    uint32_t seq = 0;
    uint32_t heartbeat = 0;
    WatchLook look;
    // Not from the block: the helper connection the pad was plugged on is gone
    // (the provisioner has let it go, or serves plugs on another).
    bool channelGone = false;
};

inline DevnodeCondition conditionOf(uint32_t raw) {
    const bool named = raw <= WATCH_CONDITION_STARTING;
    return named ? static_cast<DevnodeCondition>(raw) : DevnodeCondition::Unknown;
}

inline WatchedDevnode devnodeOf(uint32_t raw) {
    const bool named = raw <= WATCH_DEVNODE_AUDIO_FUNCTION;
    return named ? static_cast<WatchedDevnode>(raw) : WatchedDevnode::Device;
}

inline PresentationReading presentationAt(const uint8_t* copy, size_t offset) {
    PresentationReading p;
    p.condition = conditionOf(readLE32(copy + offset + WATCH_FIELD_CONDITION_OFFSET));
    p.devnode = devnodeOf(readLE32(copy + offset + WATCH_FIELD_DEVNODE_OFFSET));
    p.problem = readLE32(copy + offset + WATCH_FIELD_PROBLEM_OFFSET);
    return p;
}

// `copy` holds WATCH_BLOCK_SIZE bytes taken between two equal, even sequences.
inline WatchReading parseWatchBlock(const uint8_t* copy, bool stable) {
    WatchReading r;
    r.known = static_cast<int32_t>(readLE32(copy + WATCH_LAYOUT_OFFSET)) == WATCH_LAYOUT;
    r.stable = r.known && stable;
    r.seq = readLE32(copy + WATCH_SEQ_OFFSET);
    r.heartbeat = readLE32(copy + WATCH_HEARTBEAT_OFFSET);
    const uint32_t named = WATCH_PRESENTS_HID | WATCH_PRESENTS_XINPUT | WATCH_PRESENTS_AUDIO;
    r.look.presents = (readLE32(copy + WATCH_PRESENTS_OFFSET) & named) | WATCH_PRESENTS_HID;
    r.look.hid = presentationAt(copy, WATCH_HID_OFFSET);
    r.look.xinput = presentationAt(copy, WATCH_XINPUT_OFFSET);
    r.look.audio = presentationAt(copy, WATCH_AUDIO_OFFSET);
    return r;
}

// The seqlock's reader side: a copy whose sequence was even and unchanged
// across it is one write; after WATCH_READ_ATTEMPTS the reading is unstable
// and its look is not used.
inline WatchReading readWatchBlock(const uint8_t* view) {
    if (view == nullptr) return {};
    uint8_t copy[WATCH_BLOCK_SIZE] = {};
    bool stable = false;
    for (int attempt = 0; attempt < WATCH_READ_ATTEMPTS && !stable; ++attempt) {
        const uint32_t before = readLE32(view + WATCH_SEQ_OFFSET);
        std::atomic_thread_fence(std::memory_order_acquire);
        std::memcpy(copy, view, WATCH_BLOCK_SIZE);
        std::atomic_thread_fence(std::memory_order_acquire);
        const bool even = (before & 1u) == 0;
        stable = even && readLE32(view + WATCH_SEQ_OFFSET) == before;
    }
    return parseWatchBlock(copy, stable);
}

// Every pad has its HID game controller, looked at or not.
inline bool presents(const WatchLook& look, Presentation part) {
    const uint32_t has = look.presents | WATCH_PRESENTS_HID;
    return (has & static_cast<uint32_t>(part)) != 0;
}

inline bool isAway(DevnodeCondition condition) {
    switch (condition) {
    case DevnodeCondition::Missing:
    case DevnodeCondition::Failed:
    case DevnodeCondition::Leaving:
    case DevnodeCondition::Starting:
        return true;
    case DevnodeCondition::Unknown:
    case DevnodeCondition::Present:
    case DevnodeCondition::Disabled:
        return false;
    }
    return false;
}

inline const PresentationReading& partOf(const WatchLook& look, Presentation part) {
    switch (part) {
    case Presentation::Hid:
        return look.hid;
    case Presentation::Xinput:
        return look.xinput;
    case Presentation::Audio:
        return look.audio;
    }
    return look.hid;
}

// What a pad's presentations amount to, and the presentation that says so:
// away while any is gone, failed, leaving or not started, whatever the others
// show, since a pad missing one is a pad some games cannot reach (Emir's
// decision: an Xbox pad whose XUSB companion failed is lost to XInput and WGI
// games, so the whole pad is plugged again); unknown while one has not been
// looked at; present while an input presentation takes input; disabled when
// every input presentation is. A disabled presentation is never a loss on its
// own.
struct InputVerdict {
    DevnodeCondition condition = DevnodeCondition::Unknown;
    Presentation part = Presentation::Hid;
    PresentationReading named;
};

inline InputVerdict inputVerdict(const WatchLook& look) {
    constexpr Presentation every[] = {Presentation::Hid, Presentation::Xinput, Presentation::Audio};
    for (const Presentation part : every) {
        const PresentationReading& p = partOf(look, part);
        if (presents(look, part) && isAway(p.condition)) return {p.condition, part, p};
    }
    for (const Presentation part : every) {
        const PresentationReading& p = partOf(look, part);
        const bool unknown = p.condition == DevnodeCondition::Unknown;
        if (presents(look, part) && unknown) return {DevnodeCondition::Unknown, part, p};
    }
    for (const Presentation part : {Presentation::Hid, Presentation::Xinput}) {
        const PresentationReading& p = partOf(look, part);
        const bool present = p.condition == DevnodeCondition::Present;
        if (presents(look, part) && present) return {DevnodeCondition::Present, part, p};
    }
    return {DevnodeCondition::Disabled, Presentation::Hid, look.hid};
}

// The presentations a look finds disabled, WATCH_PRESENTS_* bits.
inline uint32_t disabledParts(const WatchLook& look) {
    uint32_t parts = 0;
    for (const Presentation part : {Presentation::Hid, Presentation::Xinput, Presentation::Audio}) {
        const bool disabled = partOf(look, part).condition == DevnodeCondition::Disabled;
        if (presents(look, part) && disabled) parts |= static_cast<uint32_t>(part);
    }
    return parts;
}

// How long a pad may be away before it is given up, counted in maintenance
// ticks rather than clock time, so a machine that slept is not taken for a
// pad, or a helper, that went quiet.
struct WatchPolicy {
    uint32_t graceTicks = 0;
    uint32_t helperSilenceTicks = 0;
    uint32_t unarmedNoticeTicks = 0;
    // A look older than this is not trusted: nothing ages and nothing changes
    // until the watcher looks again.
    uint32_t staleTicks = 0;
};

// A UMDF2 pad whose host failed is restarted by Windows, which runs the
// device start the SDK's create waits VERSION_HALF_DEADLINE_MS for; the
// timeout scale lengthens that, as it lengthens the SDK's own wait, and never
// shortens it, since the restart is Windows' and not on the SDK's clock. A dropped
// USB/IP connection is imported again by usbip-win2's first reattach, 30 s
// after the drop, and HIDMaestro's own probe gives the composite 45 s to be
// back (test/probes/coexist_check/SectionV.cs:260-261, 415-416, 474-475 at
// v2.1.0); that timer is usbip-win2's, so the timeout scale leaves it alone.
// The watcher looks every WATCH_BEAT_MS, and a look takes milliseconds
// (tests/test_windows_devnode_probe.cpp times one), so a sequence that has not
// moved for three of its periods is a watcher stuck in a call.
inline constexpr int WATCH_USBIP_GRACE_MS = 45000;
inline constexpr int WATCH_HELPER_SILENCE_MS = 10 * WATCH_BEAT_MS;
inline constexpr int WATCH_UNARMED_NOTICE_MS = 30000;
inline constexpr int WATCH_LOOK_STALE_MS = 3 * WATCH_BEAT_MS;

inline uint32_t watchTicks(std::chrono::milliseconds span) {
    const int64_t tick = MAINTENANCE_TICK_MS;
    const int64_t ticks = (span.count() + tick - 1) / tick;
    return static_cast<uint32_t>(std::max<int64_t>(1, ticks));
}

inline std::chrono::milliseconds watchGrace(ProfileBackend backend, double timeoutScale) {
    switch (backend) {
    case ProfileBackend::Umdf2: {
        const std::chrono::milliseconds budget(VERSION_HALF_DEADLINE_MS);
        return std::max(budget, scaleHalfDeadline(budget, timeoutScale));
    }
    case ProfileBackend::Usbip:
        return std::chrono::milliseconds(WATCH_USBIP_GRACE_MS);
    }
    return std::chrono::milliseconds(WATCH_USBIP_GRACE_MS);
}

inline WatchPolicy watchPolicyFor(ProfileBackend backend, double timeoutScale) {
    return {watchTicks(watchGrace(backend, timeoutScale)),
            watchTicks(std::chrono::milliseconds(WATCH_HELPER_SILENCE_MS)),
            watchTicks(std::chrono::milliseconds(WATCH_UNARMED_NOTICE_MS)),
            watchTicks(std::chrono::milliseconds(WATCH_LOOK_STALE_MS))};
}

// Unwatched until a look first finds an input presentation taking input (or
// every one disabled); Lost is final, and the core then unplugs the pad.
enum class PadWatch : uint8_t { Unwatched, Present, Absent, Disabled, Lost };
enum class PadLoss : uint8_t { None, Device, Helper, Channel };

struct WatchTracker {
    PadWatch state = PadWatch::Unwatched;
    PadLoss loss = PadLoss::None;
    // The heartbeat has moved since the plug: only then can it stop.
    bool beating = false;
    uint32_t heartbeat = 0;
    uint32_t silentTicks = 0;
    // The watcher's last finished look: its sequence, the ticks since it
    // moved, and whether that is too long to trust what it found.
    uint32_t seq = 0;
    uint32_t sinceLookTicks = 0;
    bool stale = false;
    uint32_t absentTicks = 0;
    uint32_t unwatchedTicks = 0;
    WatchLook look;
};

inline WatchTracker startWatch(const WatchReading& first) {
    WatchTracker t;
    t.heartbeat = first.known ? first.heartbeat : 0;
    const bool looked = first.known && first.stable && first.seq != 0;
    if (looked) {
        t.seq = first.seq;
        t.look = first.look;
    }
    return t;
}

inline WatchTracker heardFrom(const WatchTracker& was, const WatchReading& now) {
    WatchTracker t = was;
    const bool moved = now.known && now.heartbeat != was.heartbeat;
    t.beating = was.beating || moved;
    t.heartbeat = now.known ? now.heartbeat : was.heartbeat;
    t.silentTicks = moved ? 0 : was.silentTicks + 1;
    return t;
}

// A finished look the tracker has not seen yet replaces what it knew; without
// one, the last look grows older.
inline WatchTracker looked(const WatchTracker& was, const WatchReading& now,
                           const WatchPolicy& policy) {
    WatchTracker t = was;
    const bool fresh = now.known && now.stable && now.seq != was.seq;
    if (fresh) {
        t.seq = now.seq;
        t.sinceLookTicks = 0;
        t.stale = false;
        t.look = now.look;
        return t;
    }
    t.sinceLookTicks = was.sinceLookTicks + 1;
    t.stale = t.sinceLookTicks >= policy.staleTicks;
    return t;
}

inline WatchTracker lose(const WatchTracker& was, PadLoss loss) {
    WatchTracker t = was;
    t.state = PadWatch::Lost;
    t.loss = loss;
    return t;
}

inline WatchTracker becomes(const WatchTracker& was, PadWatch state) {
    WatchTracker t = was;
    t.state = state;
    t.absentTicks = 0;
    return t;
}

inline WatchTracker stillUnwatched(const WatchTracker& was) {
    WatchTracker t = was;
    t.unwatchedTicks = was.unwatchedTicks + 1;
    return t;
}

inline WatchTracker away(const WatchTracker& was, const WatchPolicy& policy) {
    if (was.state == PadWatch::Unwatched) return stillUnwatched(was);
    WatchTracker t = was;
    t.state = PadWatch::Absent;
    t.absentTicks = (was.state == PadWatch::Absent ? was.absentTicks : 0) + 1;
    return t.absentTicks >= policy.graceTicks ? lose(t, PadLoss::Device) : t;
}

inline WatchTracker unchanged(const WatchTracker& was) {
    return was.state == PadWatch::Unwatched ? stillUnwatched(was) : was;
}

inline WatchTracker settleCondition(const WatchTracker& t, const WatchPolicy& policy) {
    switch (inputVerdict(t.look).condition) {
    case DevnodeCondition::Unknown:
        return unchanged(t);
    case DevnodeCondition::Present:
        return becomes(t, PadWatch::Present);
    case DevnodeCondition::Disabled:
        return becomes(t, PadWatch::Disabled);
    case DevnodeCondition::Missing:
    case DevnodeCondition::Failed:
    case DevnodeCondition::Leaving:
    case DevnodeCondition::Starting:
        return away(t, policy);
    }
    return unchanged(t);
}

inline bool helperGone(const WatchTracker& t, const WatchPolicy& policy) {
    return t.beating && t.silentTicks >= policy.helperSilenceTicks;
}

// One maintenance tick's step. A reading taken mid-write keeps the last look,
// and a look grown stale decides nothing until the watcher looks again; the
// heartbeat is its own writer's, so a helper that stops still gives its pads up.
inline WatchTracker advanceWatch(const WatchTracker& was, const WatchReading& now,
                                 const WatchPolicy& policy) {
    if (was.state == PadWatch::Lost) return was;
    const WatchTracker heard = looked(heardFrom(was, now), now, policy);
    if (now.channelGone) return lose(heard, PadLoss::Channel);
    if (helperGone(heard, policy)) return lose(heard, PadLoss::Helper);
    if (heard.stale) return unchanged(heard);
    return settleCondition(heard, policy);
}

inline bool watchHoldsInput(const WatchTracker& t) { return t.state == PadWatch::Disabled; }

inline bool watchHoldsMic(const WatchTracker& t) {
    return (disabledParts(t.look) & WATCH_PRESENTS_AUDIO) != 0;
}

inline bool watchLost(const WatchTracker& t) { return t.state == PadWatch::Lost; }

// The version section and the watch each may hold a pad; a refusal is the
// version's alone, and the watch never refuses: a lost pad is unplugged.
inline WriteGate combineGates(WriteGate version, bool watchHolds) {
    switch (version) {
    case WriteGate::Refused:
        return WriteGate::Refused;
    case WriteGate::Held:
        return WriteGate::Held;
    case WriteGate::Open:
        return watchHolds ? WriteGate::Held : WriteGate::Open;
    }
    return WriteGate::Refused;
}

struct WatchNote {
    LogLevel level = LogLevel::INFO;
    std::string text;
};

inline const char* devnodeName(WatchedDevnode devnode) {
    switch (devnode) {
    case WatchedDevnode::Device:
        return "device";
    case WatchedDevnode::HidFunction:
        return "HID function";
    case WatchedDevnode::HidCollection:
        return "HID collection";
    case WatchedDevnode::Companion:
        return "XUSB companion";
    case WatchedDevnode::AudioFunction:
        return "audio function";
    }
    return "device";
}

// What a presentation is to a game: the name the log gives one that still
// takes input.
inline const char* presentationRole(Presentation part) {
    switch (part) {
    case Presentation::Hid:
        return "HID game controller";
    case Presentation::Xinput:
        return "XInput device";
    case Presentation::Audio:
        return "audio function";
    }
    return "HID game controller";
}

inline std::string conditionPhrase(DevnodeCondition condition, uint32_t problem) {
    switch (condition) {
    case DevnodeCondition::Unknown:
        return "not looked at yet";
    case DevnodeCondition::Present:
        return "present";
    case DevnodeCondition::Disabled:
        return "disabled in Windows (Code " + std::to_string(problem) + ")";
    case DevnodeCondition::Missing:
        return "not in Windows' device tree";
    case DevnodeCondition::Failed:
        return "failed with problem code " + std::to_string(problem);
    case DevnodeCondition::Leaving:
        return "being removed";
    case DevnodeCondition::Starting:
        return "not started";
    }
    return "not looked at yet";
}

// Ticks as seconds, with a tenth when the span has one.
inline std::string watchSeconds(uint32_t ticks) {
    const uint64_t tenths = static_cast<uint64_t>(ticks) * MAINTENANCE_TICK_MS / 100;
    const std::string whole = std::to_string(tenths / 10);
    const uint64_t tenth = tenths % 10;
    return (tenth == 0 ? whole : whole + "." + std::to_string(tenth)) + " s";
}

inline std::string serialName(uint32_t serial) { return "Serial " + std::to_string(serial); }

inline std::string watchSubject(uint32_t serial, const PresentationReading& p) {
    return serialName(serial) + "'s " + devnodeName(p.devnode);
}

// The input presentations a look finds disabled, by their devnodes: "HID
// collection", or "HID collection and XUSB companion".
inline std::string disabledInputs(const WatchLook& look) {
    const bool hid = look.hid.condition == DevnodeCondition::Disabled;
    const bool xinput =
        presents(look, Presentation::Xinput) && look.xinput.condition == DevnodeCondition::Disabled;
    if (hid && xinput) {
        return std::string(devnodeName(look.hid.devnode)) + " and " +
               devnodeName(look.xinput.devnode) + " are";
    }
    return std::string(devnodeName(xinput ? look.xinput.devnode : look.hid.devnode)) + " is";
}

inline WatchNote noteLoss(uint32_t serial, const WatchTracker& now, const WatchPolicy& policy) {
    const std::string consequence =
        "; Satellite gives the controller up and unplugs it, so its client plugs it again";
    switch (now.loss) {
    case PadLoss::Helper:
        return {LogLevel::WARN, serialName(serial) + "'s HIDMaestro helper has not beaten for " +
                                    watchSeconds(policy.helperSilenceTicks) + consequence};
    case PadLoss::Channel:
        return {LogLevel::WARN,
                serialName(serial) + "'s HIDMaestro helper connection is gone" + consequence};
    case PadLoss::Device:
    case PadLoss::None: {
        const PresentationReading named = inputVerdict(now.look).named;
        return {LogLevel::WARN, watchSubject(serial, named) + " is still " +
                                    conditionPhrase(named.condition, named.problem) + " after " +
                                    watchSeconds(policy.graceTicks) + consequence};
    }
    }
    return {};
}

inline WatchNote notePresent(uint32_t serial, const WatchTracker& was) {
    const PresentationReading named = inputVerdict(was.look).named;
    switch (was.state) {
    case PadWatch::Absent:
        return {LogLevel::INFO, watchSubject(serial, named) + " is back in Windows after " +
                                    watchSeconds(was.absentTicks) + "; the controller carries on"};
    case PadWatch::Disabled:
        return {LogLevel::INFO, serialName(serial) + "'s " + disabledInputs(was.look) +
                                    " enabled in Windows again; Satellite writes the "
                                    "controller again"};
    case PadWatch::Unwatched:
    case PadWatch::Present:
    case PadWatch::Lost:
        return {LogLevel::INFO, "Satellite watches " + serialName(serial) +
                                    "'s devices in "
                                    "Windows"};
    }
    return {};
}

inline WatchNote noteArrival(uint32_t serial, const WatchTracker& was, const WatchTracker& now,
                             const WatchPolicy& policy) {
    const PresentationReading named = inputVerdict(now.look).named;
    switch (now.state) {
    case PadWatch::Present:
        return notePresent(serial, was);
    case PadWatch::Absent:
        return {LogLevel::WARN, watchSubject(serial, named) + " is " +
                                    conditionPhrase(named.condition, named.problem) +
                                    "; Satellite waits up to " + watchSeconds(policy.graceTicks) +
                                    " for it to come back"};
    case PadWatch::Disabled:
        return {LogLevel::WARN, serialName(serial) + "'s " + disabledInputs(now.look) + " " +
                                    conditionPhrase(named.condition, named.problem) +
                                    "; Satellite writes nothing to the controller until it is "
                                    "enabled again, and keeps its slot"};
    case PadWatch::Lost:
        return noteLoss(serial, now, policy);
    case PadWatch::Unwatched:
        return {};
    }
    return {};
}

inline WatchNote noteUnarmed(uint32_t serial, const WatchTracker& now, const WatchPolicy& policy) {
    if (now.unwatchedTicks != policy.unarmedNoticeTicks) return {};
    const PresentationReading named = inputVerdict(now.look).named;
    return {LogLevel::WARN, "Satellite has not found " + serialName(serial) +
                                "'s game controller taking input in Windows since its plug (" +
                                devnodeName(named.devnode) + ": " +
                                conditionPhrase(named.condition, named.problem) +
                                "), so it does not watch the controller for losses"};
}

// A presentation's condition as far as the log cares.
enum class PartState : uint8_t { Unknown, Present, Disabled, Away };

inline PartState partState(DevnodeCondition condition) {
    if (condition == DevnodeCondition::Present) return PartState::Present;
    if (condition == DevnodeCondition::Disabled) return PartState::Disabled;
    return isAway(condition) ? PartState::Away : PartState::Unknown;
}

// One input presentation that changed while another still takes input.
inline WatchNote noteInputPart(uint32_t serial, Presentation part, const WatchLook& was,
                               const WatchLook& now) {
    const PresentationReading& p = partOf(now, part);
    const PartState before = partState(partOf(was, part).condition);
    const PartState after = partState(p.condition);
    const bool firstSeenWhole = before == PartState::Unknown && after == PartState::Present;
    if (before == after || firstSeenWhole) return {};
    const std::string still = std::string("; its ") + presentationRole(inputVerdict(now).part) +
                              " still takes input, so Satellite keeps writing the controller";
    switch (after) {
    case PartState::Disabled:
        return {LogLevel::WARN,
                watchSubject(serial, p) + " is " + conditionPhrase(p.condition, p.problem) + still};
    case PartState::Present:
        return {LogLevel::INFO, watchSubject(serial, partOf(was, part)) +
                                    (before == PartState::Disabled ? " is enabled in Windows again"
                                                                   : " is back in Windows")};
    case PartState::Away:
    case PartState::Unknown:
        // A part away makes the pad absent, and the pad's own line says so.
        return {};
    }
    return {};
}

// The audio function, which holds the microphone alone.
inline WatchNote noteAudio(uint32_t serial, const WatchLook& was, const WatchLook& now) {
    if (!presents(now, Presentation::Audio)) return {};
    const PresentationReading& p = now.audio;
    const PartState before = partState(was.audio.condition);
    const PartState after = partState(p.condition);
    const bool firstSeenWhole = before == PartState::Unknown && after == PartState::Present;
    if (before == after || firstSeenWhole) return {};
    const std::string subject = serialName(serial) + "'s audio function";
    switch (after) {
    case PartState::Disabled:
        return {LogLevel::WARN, subject + " is " + conditionPhrase(p.condition, p.problem) +
                                    "; Satellite holds its microphone until it is enabled again"};
    case PartState::Present:
        // Back from away is the pad's own line to say, as away was.
        if (before != PartState::Disabled) return {};
        return {LogLevel::INFO,
                subject + " is enabled in Windows again; its microphone carries on"};
    case PartState::Away:
    case PartState::Unknown:
        // A function away makes the pad absent, and the pad's own line says so.
        return {};
    }
    return {};
}

inline WatchNote noteStale(uint32_t serial, const WatchTracker& was, const WatchTracker& now,
                           const WatchPolicy& policy) {
    const bool watched = was.state != PadWatch::Unwatched && was.state != PadWatch::Lost;
    if (!watched || now.stale == was.stale) return {};
    if (now.stale) {
        return {LogLevel::WARN, "The device watch has not finished a look at " +
                                    serialName(serial) + "'s devices for " +
                                    watchSeconds(policy.staleTicks) +
                                    "; Satellite decides nothing about the controller until it "
                                    "does"};
    }
    return {LogLevel::INFO, "The device watch looks at " + serialName(serial) + "'s devices again"};
}

// The log lines a step makes: one per change of state, one per presentation
// that changed while the pad still takes input (and its audio function's,
// always), one when a look grows stale or fresh again, and one when a pad
// still unwatched comes due for its notice.
inline std::vector<WatchNote> noteWatchStep(uint32_t serial, const WatchTracker& was,
                                            const WatchTracker& now, const WatchPolicy& policy) {
    std::vector<WatchNote> notes;
    const auto add = [&notes](WatchNote note) {
        if (!note.text.empty()) notes.push_back(std::move(note));
    };
    const bool changed = now.state != was.state;
    if (changed) add(noteArrival(serial, was, now, policy));
    if (!changed && now.state == PadWatch::Unwatched) add(noteUnarmed(serial, now, policy));
    const bool takesInput = now.state == PadWatch::Present;
    const bool armedNow = changed && was.state == PadWatch::Unwatched;
    const bool partsCount = takesInput && (!changed || armedNow);
    const WatchLook before = armedNow ? WatchLook{} : was.look;
    if (partsCount) {
        add(noteInputPart(serial, Presentation::Hid, before, now.look));
        if (presents(now.look, Presentation::Xinput)) {
            add(noteInputPart(serial, Presentation::Xinput, before, now.look));
        }
    }
    if (now.state != PadWatch::Lost && now.state != PadWatch::Unwatched) {
        add(noteAudio(serial, before, now.look));
    }
    add(noteStale(serial, was, now, policy));
    return notes;
}

// What a helper's hello reply settles the session on: one that names no
// protocol predates the watch, and one that names a newer one still serves 2.
inline int32_t settledHelperProtocol(bool named, int64_t protocol) {
    const bool watches = named && protocol >= HELPER_PROTOCOL_DEVICE_WATCH;
    return watches ? HELPER_PROTOCOL_DEVICE_WATCH : HELPER_PROTOCOL_BASE;
}

} // namespace hidmaestro
} // namespace satellite
