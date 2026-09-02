// SPDX-License-Identifier: LGPL-3.0-or-later

#include "audio_default_guard.h"

namespace satellite {
namespace audioguard {

namespace {

bool isTrimmable(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\0'; }

char lowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

} // namespace

const char* roleName(Role role) {
    switch (role) {
    case Role::Console:
        return "console";
    case Role::Multimedia:
        return "multimedia";
    case Role::Communications:
        return "communications";
    }
    return "?";
}

std::string normalizeDeviceId(const std::string& id) {
    size_t begin = 0;
    size_t end = id.size();
    while (begin < end && isTrimmable(id[begin])) ++begin;
    while (end > begin && isTrimmable(id[end - 1])) --end;

    std::string out;
    out.reserve(end - begin);
    for (size_t i = begin; i < end; ++i) out.push_back(lowerAscii(id[i]));
    return out;
}

bool deviceIdsEqual(const std::string& a, const std::string& b) {
    return normalizeDeviceId(a) == normalizeDeviceId(b);
}

bool deviceIdContains(const std::string& haystack, const std::string& needle) {
    const std::string n = normalizeDeviceId(needle);
    if (n.empty()) return false;
    return normalizeDeviceId(haystack).find(n) != std::string::npos;
}

std::string stripKsFilterPrefix(const std::string& ksFilterPath) {
    if (ksFilterPath.size() < 3 || ksFilterPath[0] != '{') return ksFilterPath;

    size_t i = 1;
    while (i < ksFilterPath.size() && ksFilterPath[i] >= '0' && ksFilterPath[i] <= '9') ++i;
    if (i == 1) return ksFilterPath;
    if (i + 1 >= ksFilterPath.size()) return ksFilterPath;
    if (ksFilterPath[i] != '}' || ksFilterPath[i + 1] != '.') return ksFilterPath;
    return ksFilterPath.substr(i + 2);
}

const char* restoreReasonName(RestoreReason reason) {
    switch (reason) {
    case RestoreReason::CurrentUnknown:
        return "current-unknown";
    case RestoreReason::NotStolen:
        return "not-stolen";
    case RestoreReason::NoSnapshot:
        return "no-snapshot";
    case RestoreReason::PriorWasOurs:
        return "prior-was-ours";
    case RestoreReason::NoPriorDefault:
        return "no-prior-default";
    case RestoreReason::PriorIsCurrent:
        return "prior-is-current";
    case RestoreReason::Stolen:
        return "stolen";
    }
    return "?";
}

DefaultEndpointDecision decideRestore(const RoleSnapshot& snap, const RoleObservation& obs) {
    DefaultEndpointDecision d;
    if (!obs.haveCurrent) {
        d.reason = RestoreReason::CurrentUnknown;
        return d;
    }
    if (!obs.currentIsOurs) {
        d.reason = RestoreReason::NotStolen;
        return d;
    }
    if (!snap.captured) {
        d.reason = RestoreReason::NoSnapshot;
        return d;
    }
    if (snap.priorWasOurs) {
        d.reason = RestoreReason::PriorWasOurs;
        return d;
    }
    if (normalizeDeviceId(snap.priorId).empty()) {
        d.reason = RestoreReason::NoPriorDefault;
        return d;
    }
    if (deviceIdsEqual(snap.priorId, obs.currentId)) {
        d.reason = RestoreReason::PriorIsCurrent;
        return d;
    }

    d.action = RestoreAction::Restore;
    d.reason = RestoreReason::Stolen;
    d.targetId = snap.priorId;
    return d;
}

bool couldRestoreLater(const RoleSnapshot& snap) {
    if (!snap.captured) return false;
    if (snap.priorWasOurs) return false;
    return !normalizeDeviceId(snap.priorId).empty();
}

bool anyRoleCouldRestoreLater(const Snapshot& snapshot) {
    for (const RoleSnapshot& r : snapshot.roles) {
        if (couldRestoreLater(r)) return true;
    }
    return false;
}

const char* chainVerdictName(ChainVerdict verdict) {
    switch (verdict) {
    case ChainVerdict::NotOurs:
        return "not-ours";
    case ChainVerdict::OursByService:
        return "ours-by-service";
    case ChainVerdict::OursByHardwareId:
        return "ours-by-hardware-id";
    }
    return "?";
}

bool ChainScanner::accept(const DevNode& node) {
    if (result_.verdict != ChainVerdict::NotOurs) return false;
    if (result_.cycleDetected) return false;
    if (result_.hopsExamined >= maxHops_) {
        result_.hopCapReached = true;
        return false;
    }

    const std::string key = normalizeDeviceId(node.instanceId);
    if (!key.empty()) {
        for (const std::string& seen : visited_) {
            if (seen == key) {
                result_.cycleDetected = true;
                return false;
            }
        }
        visited_.push_back(key);
    }
    ++result_.hopsExamined;

    if (deviceIdsEqual(node.service, OWNER_SERVICE)) {
        result_.verdict = ChainVerdict::OursByService;
        return false;
    }
    for (const std::string& hw : node.hardwareIds) {
        if (deviceIdContains(hw, OWNER_HARDWARE_ID)) {
            result_.verdict = ChainVerdict::OursByHardwareId;
            return false;
        }
    }

    if (result_.hopsExamined >= maxHops_) {
        result_.hopCapReached = true;
        return false;
    }
    return true;
}

ChainResult classifyParentChain(const std::vector<DevNode>& chain, size_t maxHops) {
    ChainScanner scanner(maxHops);
    for (const DevNode& node : chain) {
        if (!scanner.accept(node)) break;
    }
    return scanner.result();
}

const char* probeVerdictName(ProbeVerdict verdict) {
    switch (verdict) {
    case ProbeVerdict::Usable:
        return "usable";
    case ProbeVerdict::CoCreateFailed:
        return "cocreate-failed";
    case ProbeVerdict::ProbeCallFailed:
        return "probe-call-failed";
    case ProbeVerdict::ImplausiblePeriods:
        return "implausible-periods";
    }
    return "?";
}

ProbeVerdict evaluatePolicyConfigProbe(const PolicyConfigProbe& probe) {
    if (!probe.coCreateOk) return ProbeVerdict::CoCreateFailed;
    // Exactly S_OK. A SUCCEEDED-but-not-S_OK answer is a layout nobody has
    // characterised, and the next call after this one writes to the audio
    // service through an unnamed vtable slot.
    if (probe.probeHr != 0) return ProbeVerdict::ProbeCallFailed;
    if (probe.defaultPeriod <= 0 || probe.minPeriod <= 0) return ProbeVerdict::ImplausiblePeriods;
    if (probe.minPeriod > probe.defaultPeriod) return ProbeVerdict::ImplausiblePeriods;
    return ProbeVerdict::Usable;
}

} // namespace audioguard
} // namespace satellite
