// SPDX-License-Identifier: LGPL-3.0-or-later
#include "core/backend_registry.h"

#include "core/gamepad_backend.h" // BACKEND_ID_*
#include "core/semver.h"
#include "core/types.h" // CONTROLLER_TYPE_*, controllerTypeName

namespace satellite {

const char* latencyTierName(LatencyTier tier) {
    switch (tier) {
    case LatencyTier::Lowest:
        return "lowest";
    case LatencyTier::Low:
        return "low";
    case LatencyTier::Medium:
        return "medium";
    case LatencyTier::High:
        return "high";
    }
    return "low";
}

uint8_t latencyTierRank(LatencyTier tier) { return static_cast<uint8_t>(tier); }

namespace {

constexpr SubmitPathFacts kFactsKernelDirect{1, 0, 0, false, 0};
constexpr SubmitPathFacts kFactsHidMaestroSony{2, 1, 0, false, 0};
constexpr SubmitPathFacts kFactsHidMaestroXbox{3, 2, 0, false, 0};

constexpr BackendControllerSupport kVigemSupport[] = {
    {CONTROLLER_TYPE_XBOX, kFactsKernelDirect, false, false, false, ""},
    {CONTROLLER_TYPE_PLAYSTATION, kFactsKernelDirect, true, true, true, "vigembus>=1.17"},
};
// Trailing columns are triggerEffects, playerLeds, mic, speaker. Only the two
// Sony pads have real audio endpoints to emulate (the DualShock 4 v2 and the
// DualSense composite personas); an Xbox pad and a Switch Pro have none, so
// they advertise none.
constexpr BackendControllerSupport kHidMaestroSupport[] = {
    {CONTROLLER_TYPE_XBOX, kFactsHidMaestroXbox, false, false, false, ""},
    {CONTROLLER_TYPE_PLAYSTATION, kFactsHidMaestroSony, true, true, true, "hidmaestro>=1.7", false,
     false, true, true},
    {CONTROLLER_TYPE_DUALSENSE, kFactsHidMaestroSony, true, true, true, "hidmaestro>=1.7", true,
     true, true, true},
    {CONTROLLER_TYPE_SWITCHPRO, kFactsHidMaestroSony, true, false, false, "hidmaestro>=1.7", false,
     true},
};
constexpr BackendControllerSupport kUinputSupport[] = {
    {CONTROLLER_TYPE_XBOX, kFactsKernelDirect, false, false, false, ""},
    {CONTROLLER_TYPE_PLAYSTATION, kFactsKernelDirect, true, true, false, ""},
    {CONTROLLER_TYPE_DUALSENSE, kFactsKernelDirect, true, true, false, ""},
    {CONTROLLER_TYPE_SWITCHPRO, kFactsKernelDirect, true, false, false, ""},
};
constexpr BackendControllerSupport kMacHidSupport[] = {
    {CONTROLLER_TYPE_PLAYSTATION, kFactsKernelDirect, true, true, true, ""},
};

const BackendDescriptor kBackends[] = {
    {BACKEND_ID_VIGEM, "Nefarius Software Solutions", "ViGEmBus", true, true, true,
     BACKEND_LIFECYCLE_EOL, "2023-11-02", kVigemSupport, 2},
    {BACKEND_ID_HIDMAESTRO, "hifihedgehog", "HIDMaestro", false, true, true,
     BACKEND_LIFECYCLE_SUPPORTED, "", kHidMaestroSupport, 4},
    {BACKEND_ID_UINPUT, "Linux uinput", "uinput", true, true, true, BACKEND_LIFECYCLE_SUPPORTED, "",
     kUinputSupport, 4},
    {BACKEND_ID_MAC_HID, "Apple IOHIDUserDevice", "macOS virtual HID", false, false, true,
     BACKEND_LIFECYCLE_SUPPORTED, "", kMacHidSupport, 1},
    {BACKEND_ID_NONE, "", "None", false, false, true, BACKEND_LIFECYCLE_SUPPORTED, "",
     kMacHidSupport, 1},
};

void appendNullable(std::string& json, const char* value) {
    if (value == nullptr || *value == '\0') {
        json += "null";
        return;
    }
    json += "\"";
    json += value;
    json += "\"";
}

void appendSubmitLatency(std::string& json, const SubmitPathFacts& f) {
    const LatencyCost cost = estimateCost(f);
    const LatencyTier tier = tierForScore(cost.score);

    json += "{\"tier\":\"";
    json += latencyTierName(tier);
    json += "\",\"rank\":";
    json += std::to_string(latencyTierRank(tier));
    json += ",\"score\":";
    json += std::to_string(cost.score);
    json += ",\"nominalUs\":";
    json += std::to_string(cost.nominalUs);
    json += ",\"tailUs\":";
    json += std::to_string(cost.tailUs);
    json += ",\"submitPath\":\"";
    json += submitPathName(f);
    json += "\",\"facts\":{\"kernelCrossings\":";
    json += std::to_string(f.kernelCrossings);
    json += ",\"threadWakeups\":";
    json += std::to_string(f.threadWakeups);
    json += ",\"brokerHops\":";
    json += std::to_string(f.brokerHops);
    json += ",\"managedRuntime\":";
    json += f.managedRuntime ? "true" : "false";
    json += ",\"pollIntervalUs\":";
    json += std::to_string(f.pollIntervalUs);
    json += "}}";
}

} // namespace

const BackendDescriptor* backendDescriptorById(const std::string& id) {
    for (const auto& d : kBackends) {
        if (id == d.id) return &d;
    }
    return nullptr;
}

const char* driverVersionState(const std::string& driverVersion,
                               const std::string& bundledVersion) {
    if (driverVersion.empty() || bundledVersion.empty()) return DRIVER_VERSION_STATE_UNKNOWN;
    const int c = compareDottedVersion(driverVersion, bundledVersion);
    if (c < 0) return DRIVER_VERSION_STATE_OUTDATED;
    if (c > 0) return DRIVER_VERSION_STATE_NEWER;
    return DRIVER_VERSION_STATE_CURRENT;
}

bool backendCanCarryAudio(const BackendDescriptor& d) {
    for (size_t i = 0; i < d.supportCount; ++i) {
        if (d.support[i].mic || d.support[i].speaker) return true;
    }
    return false;
}

std::string buildBackendsJson(const std::vector<BackendRuntimeStatus>& statuses,
                              bool controllerAudio) {
    std::string json = "[";
    bool first = true;
    for (const auto& st : statuses) {
        const BackendDescriptor* d = backendDescriptorById(st.id);
        if (d == nullptr) continue; // unknown id: nothing to advertise
        if (!first) json += ",";
        first = false;

        json += "{\"id\":\"";
        json += d->id;
        json += "\",\"vendor\":\"";
        json += d->vendor;
        json += "\",\"displayName\":\"";
        json += d->displayName;
        json += "\",\"kernelMode\":";
        json += d->kernelMode ? "true" : "false";
        json += ",\"audio\":";
        json += (controllerAudio && backendCanCarryAudio(*d)) ? "true" : "false";
        json += ",\"available\":";
        json += st.available ? "true" : "false";
        json += ",\"errorCode\":";
        appendNullable(json, st.errorCode.c_str());
        json += ",\"lifecycle\":\"";
        json += d->lifecycle;
        json += "\",\"eolDate\":";
        appendNullable(json, d->eolDate);
        json += ",\"driverVersion\":";
        appendNullable(json, st.driverVersion.c_str());
        json += ",\"bundledVersion\":";
        appendNullable(json, st.bundledVersion.c_str());
        json += ",\"versionState\":\"";
        json += driverVersionState(st.driverVersion, st.bundledVersion);
        json += "\",\"restartPending\":";
        json += st.restartPending ? "true" : "false";
        json += ",\"controllers\":[";
        for (size_t i = 0; i < d->supportCount; ++i) {
            if (i != 0) json += ",";
            const BackendControllerSupport& cs = d->support[i];
            const LatencyTier tier = tierForScore(estimateCost(cs.facts).score);
            json += "{\"type\":";
            json += std::to_string(cs.controllerType);
            json += ",\"name\":\"";
            json += controllerTypeName(cs.controllerType);
            json += "\",\"latency\":\"";
            json += latencyTierName(tier);
            json += "\",\"latencyRank\":";
            json += std::to_string(latencyTierRank(tier));
            json += ",\"motion\":";
            json += cs.motion ? "true" : "false";
            json += ",\"touchpad\":";
            json += cs.touchpad ? "true" : "false";
            json += ",\"lightbar\":";
            json += cs.lightbar ? "true" : "false";
            json += ",\"triggerEffects\":";
            json += cs.triggerEffects ? "true" : "false";
            json += ",\"playerLeds\":";
            json += cs.playerLeds ? "true" : "false";
            json += ",\"mic\":";
            json += cs.mic ? "true" : "false";
            json += ",\"speaker\":";
            json += cs.speaker ? "true" : "false";
            json += ",\"motionRequires\":";
            appendNullable(json, cs.motionRequires);
            json += ",\"submitLatency\":";
            appendSubmitLatency(json, cs.facts);
            json += "}";
        }
        json += "]}";
    }
    json += "]";
    return json;
}

CatalogBackendTraits deriveCatalogTraits(const std::vector<BackendRuntimeStatus>& statuses) {
    CatalogBackendTraits t;
    for (const auto& st : statuses) {
        const BackendDescriptor* d = backendDescriptorById(st.id);
        if (d == nullptr) continue;

        t.mouseControlSupported = t.mouseControlSupported || d->mouseControl;
        t.rumbleSupported = t.rumbleSupported || d->rumble;

        for (size_t i = 0; i < d->supportCount; ++i) {
            const BackendControllerSupport& cs = d->support[i];
            switch (cs.controllerType) {
            case CONTROLLER_TYPE_XBOX:
                t.offersXbox = true;
                break;
            case CONTROLLER_TYPE_PLAYSTATION:
                if (!t.offersDS4) {
                    t.ds4MotionSupported = cs.motion;
                    t.ds4MotionRequires = cs.motionRequires;
                    t.ds4TouchpadSupported = cs.touchpad;
                    t.ds4LightbarSupported = cs.lightbar;
                    t.ds4MicSupported = cs.mic;
                    t.ds4SpeakerSupported = cs.speaker;
                }
                t.offersDS4 = true;
                break;
            case CONTROLLER_TYPE_DUALSENSE:
                if (!t.offersDualSense) {
                    t.dualsenseMotionSupported = cs.motion;
                    t.dualsenseMotionRequires = cs.motionRequires;
                    t.dualsenseTouchpadSupported = cs.touchpad;
                    t.dualsenseLightbarSupported = cs.lightbar;
                    t.dualsenseTriggerEffectsSupported = cs.triggerEffects;
                    t.dualsensePlayerLedsSupported = cs.playerLeds;
                    t.dualsenseMicSupported = cs.mic;
                    t.dualsenseSpeakerSupported = cs.speaker;
                }
                t.offersDualSense = true;
                break;
            case CONTROLLER_TYPE_SWITCHPRO:
                if (!t.offersSwitchPro) {
                    t.switchProMotionSupported = cs.motion;
                    t.switchProMotionRequires = cs.motionRequires;
                    t.switchProPlayerLedsSupported = cs.playerLeds;
                }
                t.offersSwitchPro = true;
                break;
            default:
                break;
            }
        }
    }
    return t;
}

} // namespace satellite
