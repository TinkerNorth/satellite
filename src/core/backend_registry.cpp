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

// ViGEm / uinput submit through a kernel path → lowest tier. HIDMaestro submits
// through a user-mode shared-memory section → one tier higher. Same controller
// type across backends is how the client tells the user which route is snappier.
constexpr BackendControllerSupport kVigemSupport[] = {
    {CONTROLLER_TYPE_XBOX, LatencyTier::Lowest},
    {CONTROLLER_TYPE_PLAYSTATION, LatencyTier::Lowest},
};
constexpr BackendControllerSupport kHidMaestroSupport[] = {
    {CONTROLLER_TYPE_XBOX, LatencyTier::Low},
    {CONTROLLER_TYPE_PLAYSTATION, LatencyTier::Low},
};
constexpr BackendControllerSupport kUinputSupport[] = {
    {CONTROLLER_TYPE_XBOX, LatencyTier::Lowest},
    {CONTROLLER_TYPE_PLAYSTATION, LatencyTier::Lowest},
};

const BackendDescriptor kBackends[] = {
    {BACKEND_ID_VIGEM, "Nefarius Software Solutions", "ViGEmBus", true, kVigemSupport, 2},
    {BACKEND_ID_HIDMAESTRO, "HIDMaestro (hifihedgehog)", "HIDMaestro", false, kHidMaestroSupport,
     2},
    {BACKEND_ID_UINPUT, "Linux uinput", "uinput", true, kUinputSupport, 2},
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
            json += "}";
        }
        json += "]}";
    }
    json += "]";
    return json;
}

} // namespace satellite
