// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include "core/json.h"
#include "core/types.h"

#include <string>

namespace satellite {

// What POST /api/config changed that the caller has to act on beyond saving.
// The platform hooks (the autostart registration, the crash SDK's consent)
// are the route's to run, since core cannot reach them.
struct ConfigPatchOutcome {
    bool udpPortRejected = false;
    bool autoStartPresent = false;
    bool crashReportingPresent = false;
};

// The unprivileged port range; anything else is refused, never clamped.
inline constexpr long UDP_PORT_MIN = 1024;
inline constexpr long UDP_PORT_MAX = 65535;

// The admin form's POST body applied to `cfg`. Every key is present-only, so
// a partial POST leaves what it does not name untouched; the caller echoes the
// port in force, which after a rejection is the one that was already stored.
inline ConfigPatchOutcome applyConfigPatch(const Json& body, Config& cfg) {
    ConfigPatchOutcome out;

    long port = 0;
    if (jsonTryInt(body, "udpPort", port)) {
        const bool inRange = port >= UDP_PORT_MIN && port <= UDP_PORT_MAX;
        if (inRange) {
            cfg.udpPort = static_cast<int>(port);
        } else {
            out.udpPortRejected = true;
        }
    }

    bool flag = false;
    if (jsonTryBool(body, "autoStart", flag)) {
        cfg.autoStart = flag;
        out.autoStartPresent = true;
    }
    // Present-only so a partial POST cannot silently flip discovery off.
    if (jsonTryBool(body, "discoveryBroadcastEnabled", flag)) cfg.discoveryBroadcastEnabled = flag;
    // Takes effect on the next controller plug: an audio persona already
    // materialized keeps its endpoints until the pad is replugged, so the
    // toggle never yanks a live stream.
    if (jsonTryBool(body, "controllerAudio", flag)) cfg.controllerAudio = flag;
    // The directions gate the wire, not the persona, so unlike the master
    // switch these DO reach a stream already running.
    if (jsonTryBool(body, "controllerAudioMic", flag)) cfg.controllerAudioMic = flag;
    if (jsonTryBool(body, "controllerAudioSpeaker", flag)) cfg.controllerAudioSpeaker = flag;
    if (jsonTryBool(body, "controllerAudioHaptics", flag)) cfg.controllerAudioHaptics = flag;
    if (jsonTryBool(body, "controllerAudioKeepDefaultDevice", flag)) {
        cfg.controllerAudioKeepDefaultDevice = flag;
    }
    // Consent reaches the SDK now rather than at the next restart, because
    // withdrawing it has to stop the very next crash from being sent.
    if (jsonTryBool(body, "crashReporting", flag)) {
        cfg.crashReporting = flag;
        out.crashReportingPresent = true;
    }
    if (body.contains("networkInterface")) {
        cfg.networkInterface = jsonStr(body, "networkInterface");
    }
    return out;
}

inline const char* boolWord(bool value) { return value ? "true" : "false"; }

// The one line the log keeps of an accepted patch: the settings in force after
// it, and whether a port was refused on the way.
inline std::string configPatchLogLine(const Config& cfg, bool udpPortRejected) {
    std::string line = "Config updated: udpPort=" + std::to_string(cfg.udpPort);
    line += std::string(" autoStart=") + boolWord(cfg.autoStart);
    line += std::string(" broadcast=") + boolWord(cfg.discoveryBroadcastEnabled);
    line += std::string(" controllerAudio=") + boolWord(cfg.controllerAudio);
    line += std::string(" mic=") + boolWord(cfg.controllerAudioMic);
    line += std::string(" speaker=") + boolWord(cfg.controllerAudioSpeaker);
    line += std::string(" haptics=") + boolWord(cfg.controllerAudioHaptics);
    if (udpPortRejected) line += " (udpPort out of range, ignored)";
    return line;
}

} // namespace satellite
