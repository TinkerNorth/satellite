// SPDX-License-Identifier: LGPL-3.0-or-later

// Shared route helpers, moved verbatim from webserver.cpp (D10). The moved
// functions lost their `static` (they are now shared across the two route
// TUs); their bodies are unchanged.
#include "routes_common.h"

#include "app/app_state.h" // g_config / g_configMtx: the controllerAudio switch
#include "core/types.h"

#include <fstream>
#include <mutex>

using satellite::Json;
using satellite::jsonDump;
using satellite::JsonOut;
using satellite::jsonParse;

Json parseBody(const std::string& body) {
    Json j;
    if (!jsonParse(body, j) || !j.is_object()) return Json::object();
    return j;
}

// Web UI keys all backend-status copy off (id, errorCode).
JsonOut backendJsonObj(const BackendStatus& s) {
    JsonOut j;
    j["id"] = s.id;
    j["supported"] = s.supported;
    j["available"] = s.available;
    if (s.errorCode == nullptr) {
        j["errorCode"] = nullptr;
    } else {
        j["errorCode"] = std::string(s.errorCode);
    }
    return j;
}

std::string buildBackendJson(const BackendStatus& s) { return jsonDump(backendJsonObj(s)); }

std::string buildBackendJson() { return buildBackendJson(probeBackend()); }

// GET /api/backend/status: the legacy singular object plus the per-host
// `backends` array, so the dashboard can render remediation for every
// unavailable backend rather than only the preferred one.
// The `controllerAudio` setting under its own lock, so a caller does not have
// to hold g_configMtx across a whole JSON build.
static bool controllerAudioEnabled() {
    std::lock_guard<std::mutex> lk(g_configMtx);
    return g_config.controllerAudio;
}

// The per-direction wire gates, for the capabilities block below. Separate from
// the master switch on purpose: the client learns what will actually flow, and
// the catalog stays static identity (its ETag is version+locale, so a runtime
// setting must never reach it).
static ControllerAudioPolicy controllerAudioPolicy() {
    std::lock_guard<std::mutex> lk(g_configMtx);
    return ControllerAudioPolicy{g_config.controllerAudioMic, g_config.controllerAudioSpeaker};
}

std::string buildBackendStatusJson() {
    JsonOut j = backendJsonObj(probeBackend());
    j["backends"] =
        JsonOut::parse(satellite::buildBackendsJson(enumerateBackends(), controllerAudioEnabled()));
    return jsonDump(j);
}

// Static facts that shape the catalog, folded over the host's enumerated
// backends (registry identity, not live health — the catalog only changes on
// server upgrade; live health is /api/server/capabilities).
satellite::CatalogBackendTraits catalogBackendTraits() {
    // keyboardControlSupported stays false on every backend: the host has no
    // keystroke injection path yet. The field is published so the client gates
    // on it instead of hardwiring the assumption; flip it when injection lands.
    return satellite::deriveCatalogTraits(enumerateBackends());
}

// GET /api/server/capabilities: CURRENT dynamic state (the static
// what-exists layer is /api/catalog).
std::string buildCapabilitiesJson() {
    // Enumerate ONCE and thread it: backend/backends/motion/host must agree
    // within one response, so they can't each re-probe and race a driver
    // unplug mid-build. The singular `backend` object keeps the legacy shape.
    BackendStatus s = probeBackend();
    std::vector<satellite::BackendRuntimeStatus> all = enumerateBackends();
    satellite::CatalogBackendTraits traits = satellite::deriveCatalogTraits(all);
    const bool audio = controllerAudioEnabled();
    JsonOut j;
    j["protocolVersion"] = PROTOCOL_VERSION;
    j["serverVersion"] = SATELLITE_VERSION;
    j["maxControllers"] = MAX_BACKEND_CONTROLLERS;
    j["backend"] = backendJsonObj(s);
    j["backends"] = JsonOut::parse(satellite::buildBackendsJson(all, audio));
    JsonOut motion;
    motion["available"] = (s.available && traits.ds4MotionSupported);
    j["motion"] = std::move(motion);
    j["host"] = JsonOut::parse(satellite::buildHostBlockJson(traits, s.available));
    // What will ACTUALLY flow, which is why this ANDs in backend capability
    // that the raw setting does not: a host whose only backend cannot carry
    // audio streams nothing however the switches are set. The per-backend
    // `audio` field stays the place to learn WHICH backend can.
    const bool anyBackendCarriesAudio = traits.ds4MicSupported || traits.ds4SpeakerSupported ||
                                        traits.dualsenseMicSupported ||
                                        traits.dualsenseSpeakerSupported;
    const bool audioLive = audio && anyBackendCarriesAudio;
    const ControllerAudioPolicy policy = controllerAudioPolicy();
    JsonOut audioBlock;
    audioBlock["enabled"] = audioLive;
    audioBlock["mic"] = audioLive && policy.mic;
    audioBlock["speaker"] = audioLive && policy.speaker;
    j["controllerAudio"] = std::move(audioBlock);
    return jsonDump(j);
}

std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return "";
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
