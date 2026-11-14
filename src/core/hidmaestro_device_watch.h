// SPDX-License-Identifier: LGPL-3.0-or-later

// The device watch: what Windows has of a HIDMaestro pad's devnodes, which
// nothing in the SDK reports, and what Satellite does about it. The helper
// (helper/hidmaestro DeviceWatch) finds each pad's devnodes, looks at them at
// least every WATCH_BEAT_MS and writes what it found into one 64-byte block per
// pad, which satellite.exe maps read-only beside the version section. A pad
// disabled in Windows is held until it is enabled again; one whose devnode
// stays gone or failed past its grace, or whose helper stops beating, is given
// up, so the core unplugs it and its client plugs it again. The C# writer
// (helper/hidmaestro-core DeviceWatchBlock) is held to these constants by its
// own test, which reads them from this file.
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

namespace satellite {
namespace hidmaestro {

// The hello's protocol. 1 is the pipe as it was before the watch, and a helper
// that predates it names none; a session that settles on 2 gets a watch block
// with every plug, and nothing else changes.
inline constexpr int32_t HELPER_PROTOCOL_BASE = 1;
inline constexpr int32_t HELPER_PROTOCOL_DEVICE_WATCH = 2;
inline constexpr int32_t HELPER_PROTOCOL_OFFERED = HELPER_PROTOCOL_DEVICE_WATCH;

// One block per pad, little-endian, every field a naturally aligned word. The
// watcher writes the condition words under the sequence (odd while it writes);
// the heartbeat has a writer of its own, a thread that makes no Plug and Play
// call, so a watcher stuck in one cannot pass for a dead helper.
inline constexpr size_t WATCH_BLOCK_SIZE = 64;
inline constexpr size_t WATCH_LAYOUT_OFFSET = 0;
inline constexpr size_t WATCH_SEQ_OFFSET = 4;
inline constexpr size_t WATCH_HEARTBEAT_OFFSET = 8;
inline constexpr size_t WATCH_CONDITION_OFFSET = 12;
inline constexpr size_t WATCH_DEVNODE_OFFSET = 16;
inline constexpr size_t WATCH_PROBLEM_OFFSET = 20;
inline constexpr int32_t WATCH_LAYOUT = 1;
inline constexpr int WATCH_READ_ATTEMPTS = 4;
// The helper's heartbeat moves at least this often while it lives.
inline constexpr int WATCH_BEAT_MS = 500;

// What the watcher last found of the pad's devnodes: every one present and
// started, or the first that is not, with a disabled one ahead of any other.
inline constexpr uint32_t WATCH_CONDITION_UNKNOWN = 0;  // not looked at yet
inline constexpr uint32_t WATCH_CONDITION_PRESENT = 1;  // started, no problem
inline constexpr uint32_t WATCH_CONDITION_DISABLED = 2; // CM_PROB_DISABLED (22)
inline constexpr uint32_t WATCH_CONDITION_MISSING = 3;  // not in the device tree
inline constexpr uint32_t WATCH_CONDITION_FAILED = 4;   // any other problem code
inline constexpr uint32_t WATCH_CONDITION_LEAVING = 5;  // DN_WILL_BE_REMOVED
inline constexpr uint32_t WATCH_CONDITION_STARTING = 6; // present, not started

// Which devnode the condition is about.
inline constexpr uint32_t WATCH_DEVNODE_DEVICE = 0; // the HID parent, or a composite's USB device
inline constexpr uint32_t WATCH_DEVNODE_HID_FUNCTION = 1;   // a composite's HID interface
inline constexpr uint32_t WATCH_DEVNODE_HID_COLLECTION = 2; // the HID child games open
inline constexpr uint32_t WATCH_DEVNODE_COMPANION = 3;      // the XUSB companion

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
};

struct WatchBlock {
    uint32_t layout = 0;
    uint32_t seq = 0;
    uint32_t heartbeat = 0;
    uint32_t condition = 0;
    uint32_t devnode = 0;
    uint32_t problem = 0;
    uint32_t reserved[10] = {};
};

static_assert(sizeof(WatchBlock) == WATCH_BLOCK_SIZE);
static_assert(offsetof(WatchBlock, layout) == WATCH_LAYOUT_OFFSET);
static_assert(offsetof(WatchBlock, seq) == WATCH_SEQ_OFFSET);
static_assert(offsetof(WatchBlock, heartbeat) == WATCH_HEARTBEAT_OFFSET);
static_assert(offsetof(WatchBlock, condition) == WATCH_CONDITION_OFFSET);
static_assert(offsetof(WatchBlock, devnode) == WATCH_DEVNODE_OFFSET);
static_assert(offsetof(WatchBlock, problem) == WATCH_PROBLEM_OFFSET);

struct WatchReading {
    // The block is layout 1; nothing else in a reading counts otherwise.
    bool known = false;
    // The condition words came from one finished write.
    bool stable = false;
    uint32_t heartbeat = 0;
    DevnodeCondition condition = DevnodeCondition::Unknown;
    WatchedDevnode devnode = WatchedDevnode::Device;
    uint32_t problem = 0;
};

inline DevnodeCondition conditionOf(uint32_t raw) {
    const bool named = raw <= WATCH_CONDITION_STARTING;
    return named ? static_cast<DevnodeCondition>(raw) : DevnodeCondition::Unknown;
}

inline WatchedDevnode devnodeOf(uint32_t raw) {
    const bool named = raw <= WATCH_DEVNODE_COMPANION;
    return named ? static_cast<WatchedDevnode>(raw) : WatchedDevnode::Device;
}

// `copy` holds WATCH_BLOCK_SIZE bytes taken between two equal, even sequences.
inline WatchReading parseWatchBlock(const uint8_t* copy, bool stable) {
    WatchReading r;
    r.known = static_cast<int32_t>(readLE32(copy + WATCH_LAYOUT_OFFSET)) == WATCH_LAYOUT;
    r.stable = r.known && stable;
    r.heartbeat = readLE32(copy + WATCH_HEARTBEAT_OFFSET);
    r.condition = conditionOf(readLE32(copy + WATCH_CONDITION_OFFSET));
    r.devnode = devnodeOf(readLE32(copy + WATCH_DEVNODE_OFFSET));
    r.problem = readLE32(copy + WATCH_PROBLEM_OFFSET);
    return r;
}

// The seqlock's reader side: a copy whose sequence was even and unchanged
// across it is one write; after WATCH_READ_ATTEMPTS the reading is unstable
// and its condition is not used.
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

// How long a pad may be away before it is given up, counted in maintenance
// ticks rather than clock time, so a machine that slept is not taken for a
// pad, or a helper, that went quiet.
struct WatchPolicy {
    uint32_t graceTicks = 0;
    uint32_t helperSilenceTicks = 0;
    uint32_t unarmedNoticeTicks = 0;
};

// A UMDF2 pad whose host failed is restarted by Windows, which runs the
// device start the SDK's create waits VERSION_HALF_DEADLINE_MS for. A dropped
// USB/IP connection is imported again by usbip-win2's first reattach, 30 s
// after the drop, and HIDMaestro's own probe gives the composite 45 s to be
// back (test/probes/coexist_check/SectionV.cs:260-261, 415-416, 474-475 at
// v2.1.0); that timer is usbip-win2's, so the timeout scale leaves it alone.
inline constexpr int WATCH_USBIP_GRACE_MS = 45000;
inline constexpr int WATCH_HELPER_SILENCE_MS = 10 * WATCH_BEAT_MS;
inline constexpr int WATCH_UNARMED_NOTICE_MS = 30000;

inline uint32_t watchTicks(std::chrono::milliseconds span) {
    const int64_t tick = MAINTENANCE_TICK_MS;
    const int64_t ticks = (span.count() + tick - 1) / tick;
    return static_cast<uint32_t>(std::max<int64_t>(1, ticks));
}

inline std::chrono::milliseconds watchGrace(ProfileBackend backend, double timeoutScale) {
    switch (backend) {
    case ProfileBackend::Umdf2:
        return scaleHalfDeadline(std::chrono::milliseconds(VERSION_HALF_DEADLINE_MS), timeoutScale);
    case ProfileBackend::Usbip:
        return std::chrono::milliseconds(WATCH_USBIP_GRACE_MS);
    }
    return std::chrono::milliseconds(WATCH_USBIP_GRACE_MS);
}

inline WatchPolicy watchPolicyFor(ProfileBackend backend, double timeoutScale) {
    return {watchTicks(watchGrace(backend, timeoutScale)),
            watchTicks(std::chrono::milliseconds(WATCH_HELPER_SILENCE_MS)),
            watchTicks(std::chrono::milliseconds(WATCH_UNARMED_NOTICE_MS))};
}

// Unwatched until the helper first finds every devnode present (or one
// disabled); Lost is final, and the core then unplugs the pad.
enum class PadWatch : uint8_t { Unwatched, Present, Absent, Disabled, Lost };
enum class PadLoss : uint8_t { None, Device, Helper };

struct WatchTracker {
    PadWatch state = PadWatch::Unwatched;
    PadLoss loss = PadLoss::None;
    // The heartbeat has moved since the plug: only then can it stop.
    bool beating = false;
    uint32_t heartbeat = 0;
    uint32_t silentTicks = 0;
    uint32_t absentTicks = 0;
    uint32_t unwatchedTicks = 0;
    // The last condition read from a finished write.
    DevnodeCondition condition = DevnodeCondition::Unknown;
    WatchedDevnode devnode = WatchedDevnode::Device;
    uint32_t problem = 0;
};

inline WatchTracker startWatch(const WatchReading& first) {
    WatchTracker t;
    t.heartbeat = first.known ? first.heartbeat : 0;
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

inline WatchTracker observed(const WatchTracker& was, const WatchReading& now) {
    const bool fresh = now.known && now.stable;
    if (!fresh) return was;
    WatchTracker t = was;
    t.condition = now.condition;
    t.devnode = now.devnode;
    t.problem = now.problem;
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
    switch (t.condition) {
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

// One maintenance tick's step. A reading taken mid-write keeps the last
// condition, so it neither starts nor ends an absence by itself.
inline WatchTracker advanceWatch(const WatchTracker& was, const WatchReading& now,
                                 const WatchPolicy& policy) {
    if (was.state == PadWatch::Lost) return was;
    const WatchTracker heard = observed(heardFrom(was, now), now);
    if (helperGone(heard, policy)) return lose(heard, PadLoss::Helper);
    return settleCondition(heard, policy);
}

inline bool watchHoldsInput(const WatchTracker& t) { return t.state == PadWatch::Disabled; }

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
    }
    return "device";
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

inline std::string watchSubject(uint32_t serial, const WatchTracker& t) {
    return "Serial " + std::to_string(serial) + "'s " + devnodeName(t.devnode);
}

inline WatchNote noteLoss(uint32_t serial, const WatchTracker& now, const WatchPolicy& policy) {
    const std::string consequence =
        "; Satellite gives the controller up and unplugs it, so its client plugs it again";
    switch (now.loss) {
    case PadLoss::Helper:
        return {LogLevel::WARN, "Serial " + std::to_string(serial) +
                                    "'s HIDMaestro helper has not beaten for " +
                                    watchSeconds(policy.helperSilenceTicks) + consequence};
    case PadLoss::Device:
    case PadLoss::None:
        return {LogLevel::WARN, watchSubject(serial, now) + " is still " +
                                    conditionPhrase(now.condition, now.problem) + " after " +
                                    watchSeconds(policy.graceTicks) + consequence};
    }
    return {};
}

inline WatchNote notePresent(uint32_t serial, const WatchTracker& was) {
    switch (was.state) {
    case PadWatch::Absent:
        return {LogLevel::INFO, watchSubject(serial, was) + " is back in Windows after " +
                                    watchSeconds(was.absentTicks) + "; the controller carries on"};
    case PadWatch::Disabled:
        return {LogLevel::INFO, watchSubject(serial, was) +
                                    " is enabled in Windows again; Satellite writes the "
                                    "controller again"};
    case PadWatch::Unwatched:
    case PadWatch::Present:
    case PadWatch::Lost:
        return {LogLevel::INFO, "Serial " + std::to_string(serial) +
                                    "'s devices are all present in Windows; Satellite watches "
                                    "them"};
    }
    return {};
}

inline WatchNote noteArrival(uint32_t serial, const WatchTracker& was, const WatchTracker& now,
                             const WatchPolicy& policy) {
    switch (now.state) {
    case PadWatch::Present:
        return notePresent(serial, was);
    case PadWatch::Absent:
        return {LogLevel::WARN, watchSubject(serial, now) + " is " +
                                    conditionPhrase(now.condition, now.problem) +
                                    "; Satellite waits up to " + watchSeconds(policy.graceTicks) +
                                    " for it to come back"};
    case PadWatch::Disabled:
        return {LogLevel::WARN, watchSubject(serial, now) + " is " +
                                    conditionPhrase(now.condition, now.problem) +
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
    return {LogLevel::WARN, "Satellite has not found every one of serial " +
                                std::to_string(serial) + "'s devices in Windows since its plug (" +
                                devnodeName(now.devnode) + ": " +
                                conditionPhrase(now.condition, now.problem) +
                                "), so it does not watch the controller for losses"};
}

// The log line a step makes: one per change of state, and one when a pad that
// is still unwatched comes due for its notice.
inline WatchNote noteWatchStep(uint32_t serial, const WatchTracker& was, const WatchTracker& now,
                               const WatchPolicy& policy) {
    const bool changed = now.state != was.state;
    if (changed) return noteArrival(serial, was, now, policy);
    if (now.state == PadWatch::Unwatched) return noteUnarmed(serial, now, policy);
    return {};
}

// What a helper's hello reply settles the session on: one that names no
// protocol predates the watch, and one that names a newer one still serves 2.
inline int32_t settledHelperProtocol(bool named, int64_t protocol) {
    const bool watches = named && protocol >= HELPER_PROTOCOL_DEVICE_WATCH;
    return watches ? HELPER_PROTOCOL_DEVICE_WATCH : HELPER_PROTOCOL_BASE;
}

} // namespace hidmaestro
} // namespace satellite
