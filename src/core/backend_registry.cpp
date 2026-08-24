// SPDX-License-Identifier: LGPL-3.0-or-later
#include "core/backend_registry.h"

#include "core/gamepad_backend.h" // BACKEND_ID_*
#include "core/types.h"           // CONTROLLER_TYPE_*, controllerTypeName

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

// ViGEm / uinput submit through a kernel path → lowest tier. HIDMaestro and
// machid submit through user-mode surfaces → one tier higher. Same controller
// type across backends is how the client tells the user which route is snappier.
constexpr BackendControllerSupport kVigemSupport[] = {
    {CONTROLLER_TYPE_XBOX, LatencyTier::Lowest, false, false, false, ""},
    {CONTROLLER_TYPE_PLAYSTATION, LatencyTier::Lowest, true, true, true, "vigembus>=1.17"},
};
constexpr BackendControllerSupport kHidMaestroSupport[] = {
    {CONTROLLER_TYPE_XBOX, LatencyTier::Low, false, false, false, ""},
    {CONTROLLER_TYPE_PLAYSTATION, LatencyTier::Low, true, true, true, "hidmaestro>=1.7"},
    {CONTROLLER_TYPE_DUALSENSE, LatencyTier::Low, true, true, true, "hidmaestro>=1.7"},
    {CONTROLLER_TYPE_SWITCHPRO, LatencyTier::Low, true, false, false, "hidmaestro>=1.7"},
};
constexpr BackendControllerSupport kUinputSupport[] = {
    {CONTROLLER_TYPE_XBOX, LatencyTier::Lowest, false, false, false, ""},
    {CONTROLLER_TYPE_PLAYSTATION, LatencyTier::Lowest, true, true, true, ""},
    {CONTROLLER_TYPE_DUALSENSE, LatencyTier::Lowest, true, true, true, ""},
    {CONTROLLER_TYPE_SWITCHPRO, LatencyTier::Lowest, true, false, false, ""},
};
constexpr BackendControllerSupport kMacHidSupport[] = {
    {CONTROLLER_TYPE_PLAYSTATION, LatencyTier::Low, true, true, true, ""},
};

const BackendDescriptor kBackends[] = {
    {BACKEND_ID_VIGEM, "Nefarius Software Solutions", "ViGEmBus", true, true, true, kVigemSupport,
     2},
    {BACKEND_ID_HIDMAESTRO, "hifihedgehog", "HIDMaestro", false, true, true, kHidMaestroSupport, 4},
    {BACKEND_ID_UINPUT, "Linux uinput", "uinput", true, true, true, kUinputSupport, 4},
    {BACKEND_ID_MAC_HID, "Apple IOHIDUserDevice", "macOS virtual HID", false, false, true,
     kMacHidSupport, 1},
    {BACKEND_ID_NONE, "", "None", false, false, true, kMacHidSupport, 1},
};

} // namespace

const BackendDescriptor* backendDescriptorById(const std::string& id) {
    for (const auto& d : kBackends) {
        if (id == d.id) return &d;
    }
    return nullptr;
}

std::string buildBackendsJson(const std::vector<BackendRuntimeStatus>& statuses) {
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
        json += ",\"available\":";
        json += st.available ? "true" : "false";
        json += ",\"errorCode\":";
        if (st.errorCode.empty()) {
            json += "null";
        } else {
            json += "\"";
            json += st.errorCode;
            json += "\"";
        }
        json += ",\"controllers\":[";
        for (size_t i = 0; i < d->supportCount; ++i) {
            if (i != 0) json += ",";
            const BackendControllerSupport& cs = d->support[i];
            json += "{\"type\":";
            json += std::to_string(cs.controllerType);
            json += ",\"name\":\"";
            json += controllerTypeName(cs.controllerType);
            json += "\",\"latency\":\"";
            json += latencyTierName(cs.latency);
            json += "\",\"latencyRank\":";
            json += std::to_string(latencyTierRank(cs.latency));
            json += ",\"motion\":";
            json += cs.motion ? "true" : "false";
            json += ",\"touchpad\":";
            json += cs.touchpad ? "true" : "false";
            json += ",\"lightbar\":";
            json += cs.lightbar ? "true" : "false";
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
                }
                t.offersDS4 = true;
                break;
            case CONTROLLER_TYPE_DUALSENSE:
                if (!t.offersDualSense) {
                    t.dualsenseMotionSupported = cs.motion;
                    t.dualsenseMotionRequires = cs.motionRequires;
                    t.dualsenseTouchpadSupported = cs.touchpad;
                    t.dualsenseLightbarSupported = cs.lightbar;
                }
                t.offersDualSense = true;
                break;
            case CONTROLLER_TYPE_SWITCHPRO:
                if (!t.offersSwitchPro) {
                    t.switchProMotionSupported = cs.motion;
                    t.switchProMotionRequires = cs.motionRequires;
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
