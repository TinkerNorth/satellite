// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include "core/json.h"
#include "core/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace satellite {

// The wire names of the CAP_* bits: the one place the `caps` object a client
// sends and the one the server echoes both read, in the order the echo writes
// them, so the two cannot drift.
struct CapName {
    const char* name;
    uint16_t bit;
};
inline constexpr CapName CAP_NAMES[] = {
    {"rumble", CAP_RUMBLE},
    {"motion", CAP_MOTION},
    {"analogTriggers", CAP_ANALOG_TRIGGERS},
    {"lightbar", CAP_LIGHTBAR},
    {"triggerEffects", CAP_TRIGGER_EFFECTS},
    {"playerLeds", CAP_PLAYER_LEDS},
    {"mic", CAP_MIC},
    {"speaker", CAP_SPEAKER},
    {"hapticAudio", CAP_HAPTIC_AUDIO},
};

inline JsonOut capsJsonObj(uint16_t caps) {
    JsonOut j;
    for (const auto& cap : CAP_NAMES) j[cap.name] = (caps & cap.bit) != 0;
    return j;
}

inline uint16_t capsFromJson(const Json& caps) {
    uint16_t out = 0;
    for (const auto& cap : CAP_NAMES) {
        if (jsonBool(caps, cap.name)) out |= cap.bit;
    }
    return out;
}

// A JSON integer narrowed to the byte the wire carries; anything above
// saturates rather than wraps, so an absurd value stays absurd downstream.
inline uint8_t saturateToByte(long value) {
    return value > 255 ? 255 : static_cast<uint8_t>(value);
}

// `type` is REQUIRED: a descriptor without it would force a server-side
// default type, the default-then-correct bug class this contract removes.
// Out-of-range values pass through; the service reports invalidType per
// controller rather than failing the whole request.
inline bool parseDescriptorObject(const Json& obj, bool requireIdx, ControllerDescriptor& d) {
    long idx = 0;
    if (jsonTryInt(obj, "ctrlIdx", idx)) {
        if (idx < 0) return false;
        d.ctrlIdx = saturateToByte(idx);
    } else if (requireIdx) {
        return false;
    }
    long type = 0;
    if (!jsonTryInt(obj, "type", type) || type < 0) return false;
    d.type = saturateToByte(type);
    d.caps = capsFromJson(jsonObject(obj, "caps"));
    d.preferredBackend = jsonStr(obj, "preferredBackend");
    d.touchpadMode = touchpadModeFromName(jsonStr(obj, "touchpadMode"));
    return true;
}

// The `controllers` array of a session PUT. Absent means no descriptors; an
// entry that is not an object is skipped; one malformed object fails the lot,
// because a partially applied topology is worse than a refused one.
inline bool parseControllerDescriptors(const Json& body, std::vector<ControllerDescriptor>& out) {
    const auto it = body.find("controllers");
    if (it == body.end() || !it->is_array()) return true;
    for (const auto& obj : *it) {
        if (!obj.is_object()) continue;
        ControllerDescriptor d;
        if (!parseDescriptorObject(obj, /*requireIdx=*/true, d)) return false;
        out.push_back(d);
    }
    return true;
}

} // namespace satellite
