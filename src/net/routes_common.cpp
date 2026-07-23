// SPDX-License-Identifier: LGPL-3.0-or-later

// Shared route helpers, moved verbatim from webserver.cpp (D10). The moved
// functions lost their `static` (they are now shared across the two route
// TUs); their bodies are unchanged.
#include "routes_common.h"

#include "core/types.h"

#include <fstream>

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

// Static facts about the backend that shape the catalog, keyed off the
// backend's identity not its live health (the catalog only changes on server
// upgrade; live health is /api/server/capabilities).
satellite::CatalogBackendTraits catalogBackendTraits(const BackendStatus& s) {
    satellite::CatalogBackendTraits t;
    const std::string id = s.id;
    if (id == BACKEND_ID_VIGEM) {
        t.ds4MotionSupported = true;
        t.ds4MotionRequires = "vigembus>=1.17";
        t.ds4TouchpadSupported = true;
        t.ds4LightbarSupported = true;
        t.mouseControlSupported = true;
        // Every emulated pad on a live bus reports rumble back to the client.
        t.rumbleSupported = true;
        t.offersXbox = true;
        t.offersDS4 = true; // ViGEmBus has only Xbox360Wired + DualShock4Wired targets
    } else if (id == BACKEND_ID_UINPUT) {
        t.ds4MotionSupported = true;
        t.ds4TouchpadSupported = true;
        t.ds4LightbarSupported = true;
        t.mouseControlSupported = true;
        t.rumbleSupported = true;
        // uinput stamps any VID/PID: all four identities are wired + verified.
        t.offersXbox = true;
        t.offersDS4 = true;
        t.offersDualSense = true;
        t.offersSwitchPro = true;
    } else if (id == BACKEND_ID_MAC_HID || id == BACKEND_ID_NONE) {
        // Virtual DS4 via IOHIDUserDevice: motion, touchpad, and lightbar all
        // ride the DS4 report set (no driver-version gate, hence no requires
        // code), and rumble+lightbar return through set-report. macOS has no
        // host pointer-injection path yet, so mouseControl stays unsupported
        // (matches IGamepadPort::supportsRelativeMouse() on the adapter).
        t.ds4MotionSupported = true;
        t.ds4TouchpadSupported = true;
        t.ds4LightbarSupported = true;
        t.rumbleSupported = true;
        // macOS offers DS4 (the fake Xbox is dropped); DualSense codec is a
        // follow-up. NONE = unentitled mac: same static catalog, backend.available
        // reports the runtime truth.
        t.offersDS4 = true;
    }
    // keyboardControlSupported stays false on every backend: the host has no keystroke
    // injection path yet. The field is published so the client gates on it (and stays
    // unoffered) instead of hardwiring the assumption; flip it when injection lands.
    return t;
}

satellite::CatalogBackendTraits catalogBackendTraits() {
    return catalogBackendTraits(probeBackend());
}

// GET /api/server/capabilities: CURRENT dynamic state (the static
// what-exists layer is /api/catalog).
std::string buildCapabilitiesJson() {
    // Probe ONCE and thread it: backend/motion/host must agree within one
    // response, so they can't each re-probe and race a driver unplug mid-build.
    BackendStatus s = probeBackend();
    satellite::CatalogBackendTraits traits = catalogBackendTraits(s);
    JsonOut j;
    j["protocolVersion"] = PROTOCOL_VERSION;
    j["serverVersion"] = SATELLITE_VERSION;
    j["maxControllers"] = MAX_BACKEND_CONTROLLERS;
    j["backend"] = backendJsonObj(s);
    JsonOut motion;
    motion["available"] = (s.available && traits.ds4MotionSupported);
    j["motion"] = std::move(motion);
    j["host"] = JsonOut::parse(satellite::buildHostBlockJson(traits, s.available));
    return jsonDump(j);
}

std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return "";
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
