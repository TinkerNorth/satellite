// SPDX-License-Identifier: LGPL-3.0-or-later

// Decision logic for the controller-audio default-endpoint guard. A composite
// DualSense/DualShock persona presents a real USB audio render endpoint, and
// Windows promotes it: an endpoint with no `Level` value under
// HKLM\...\MMDevices\Audio\Render wins the "newest device" first-shot bucket,
// where USB bus type and Speakers form factor both rank top. Desktop audio then
// pours into the virtual pad and back out over the network. The guard puts the
// previous default back; it never stops the endpoint existing, because the
// pad speaker/headset is the point of the feature.
//
// Recognising OUR endpoint is the delicate part. A real DualSense is
// byte-identical on VID/PID, hardware id, friendly name, form factor and device
// description, so any name- or VID-match would steal the default away from a
// physical controller. The one sound discriminator is the PnP parent chain,
// which for our endpoint terminates at the usbip-win2 root devnode the
// HIDMaestro SDK stamps with ROOT\HIDMAESTRO_UDE, while a real pad terminates
// at a PCI xHCI controller. That makes it a positive test, not a heuristic.
//
// Deliberately free of <windows.h>: every rule below is string and state work,
// so it links into the portable test target and is verified on every CI
// platform. COM, cfgmgr32 and the undocumented IPolicyConfig vtable slot live
// in audio_endpoint_com.h.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace satellite {
namespace audioguard {

// mmdeviceapi.h ERole values, pinned here so the pure layer never includes it.
// Windows keeps one default render endpoint per role and a promotion moves all
// three, so all three are watched and restored independently.
enum class Role : int { Console = 0, Multimedia = 1, Communications = 2 };

inline constexpr size_t ROLE_COUNT = 3;
inline constexpr Role ALL_ROLES[ROLE_COUNT] = {Role::Console, Role::Multimedia,
                                               Role::Communications};

inline constexpr size_t roleIndex(Role role) { return static_cast<size_t>(role); }
const char* roleName(Role role);

// Windows device and endpoint ids are case-insensitive; every comparison here
// goes through this, and callers must not compare the raw strings themselves.
// Trims surrounding whitespace and NULs too, which is what a REG_SZ read that
// includes its terminator hands back.
std::string normalizeDeviceId(const std::string& id);
bool deviceIdsEqual(const std::string& a, const std::string& b);
bool deviceIdContains(const std::string& haystack, const std::string& needle);

// The {b3f8fa53-0004-438e-9003-51a46e139bfc},2 endpoint property holds a KS
// filter path: a device instance id behind a `{n}.` topology-index prefix, e.g.
// "{1}.USB\VID_054C&PID_0CE6&MI_00\3&253044F8&0&0000". Anything that is not
// exactly `{digits}.` is returned untouched rather than guessed at, so a future
// shape change degrades into a failed devnode lookup, never a wrong match.
std::string stripKsFilterPrefix(const std::string& ksFilterPath);

// No per-plug "already restored" latch, on purpose. Two composite pads in one
// session upsert plug milliseconds apart and Windows promotes each endpoint as
// it enumerates, so one window sees two promotions; a latch would hand the
// second pad the desktop. Inside the window a stolen default is always the
// plug's doing, never the user's, so every observation is judged on its own
// and the poll budget is what bounds the fight.
struct RoleSnapshot {
    bool captured = false;
    std::string priorId;
    bool priorWasOurs = false; // the user had already chosen a pad; leave it
};

struct Snapshot {
    std::array<RoleSnapshot, ROLE_COUNT> roles{};

    RoleSnapshot& role(Role r) { return roles[roleIndex(r)]; }
    const RoleSnapshot& role(Role r) const { return roles[roleIndex(r)]; }
    void clear() { roles = {}; }
};

enum class RestoreAction { None, Restore };

enum class RestoreReason {
    CurrentUnknown,
    NotStolen,
    NoSnapshot,
    PriorWasOurs,
    NoPriorDefault,
    PriorIsCurrent,
    Stolen,
};

const char* restoreReasonName(RestoreReason reason);

struct RoleObservation {
    bool haveCurrent = false;
    std::string currentId;
    bool currentIsOurs = false;
};

struct DefaultEndpointDecision {
    RestoreAction action = RestoreAction::None;
    RestoreReason reason = RestoreReason::CurrentUnknown;
    std::string targetId;
};

DefaultEndpointDecision decideRestore(const RoleSnapshot& snap, const RoleObservation& obs);

// Whether a poll could restore this role at all: a captured prior that is
// neither empty nor one of ours. Promotion is not synchronous with the plug
// returning, so the caller polls over a bounded window; a snapshot with no
// restorable role lets it skip the window entirely.
bool couldRestoreLater(const RoleSnapshot& snap);
bool anyRoleCouldRestoreLater(const Snapshot& snapshot);

// A real USB audio endpoint measures 14 hops to the ACPI root
// (usbaudio -> usbccgp -> 3x USBHUB3 -> USBXHCI -> 3x pci -> 2x ACPI -> ...),
// and ours should reach the usbip root in 3. The cap sits above the real depth
// on purpose: below it, hopCapReached fires for every ordinary device and stops
// meaning anything, and a future deeper nesting of the UDE root would silently
// stop the guard recognising its own endpoint.
inline constexpr size_t MAX_PARENT_HOPS = 16;

// The HIDMaestro SDK stamps its usbip-win2 root devnode with this owner
// hardware id precisely so a consumer can recognise its own devices; the
// service is that devnode driver. Either alone is proof.
inline constexpr const char* OWNER_SERVICE = "usbip2_ude";
inline constexpr const char* OWNER_HARDWARE_ID = "ROOT\\HIDMAESTRO_UDE";

struct DevNode {
    std::string instanceId;
    std::string service;
    std::vector<std::string> hardwareIds;
};

enum class ChainVerdict { NotOurs, OursByService, OursByHardwareId };

struct ChainResult {
    ChainVerdict verdict = ChainVerdict::NotOurs;
    size_t hopsExamined = 0;
    bool hopCapReached = false; // budget spent without a verdict
    bool cycleDetected = false;

    bool isOurs() const { return verdict != ChainVerdict::NotOurs; }
};

const char* chainVerdictName(ChainVerdict verdict);

// Incremental so the IO walker stops the moment there is an answer instead of
// materialising a whole chain of cfgmgr32 reads it will not look at.
class ChainScanner {
  public:
    explicit ChainScanner(size_t maxHops = MAX_PARENT_HOPS) : maxHops_(maxHops) {}

    // False when the walk must stop: a verdict, the hop cap, or a devnode
    // already visited.
    bool accept(const DevNode& node);
    const ChainResult& result() const { return result_; }

  private:
    size_t maxHops_;
    std::vector<std::string> visited_;
    ChainResult result_;
};

ChainResult classifyParentChain(const std::vector<DevNode>& chain,
                                size_t maxHops = MAX_PARENT_HOPS);

// The 12-method IPolicyConfig ({F8679F50-850A-41CF-9C72-430F290290C8} on
// coclass {870AF99C-171D-4F9E-AF0D-E63DF40C2BC9}) puts SetDefaultEndpoint at
// absolute vtable slot 13. The 11-method IPolicyConfigVista layout is a
// separate coclass whose slot 13 is SetEndpointVisibility, so a blind slot-13
// call on the wrong object hides an endpoint instead of selecting it. Slot 7
// separates them with a cheap read: the full layout answers GetProcessingPeriod
// with S_OK and two sane periods, the Vista layout answers there with a stubbed
// SetProcessingPeriod.
inline constexpr int POLICY_CONFIG_SLOT_GET_PROCESSING_PERIOD = 7;
inline constexpr int POLICY_CONFIG_SLOT_SET_DEFAULT_ENDPOINT = 13;

struct PolicyConfigProbe {
    bool coCreateOk = false;
    int32_t probeHr = 0;
    int64_t defaultPeriod = 0;
    int64_t minPeriod = 0;
};

enum class ProbeVerdict { Usable, CoCreateFailed, ProbeCallFailed, ImplausiblePeriods };

const char* probeVerdictName(ProbeVerdict verdict);
ProbeVerdict evaluatePolicyConfigProbe(const PolicyConfigProbe& probe);

inline bool policyConfigSlotIsSafe(const PolicyConfigProbe& probe) {
    return evaluatePolicyConfigProbe(probe) == ProbeVerdict::Usable;
}

} // namespace audioguard
} // namespace satellite
