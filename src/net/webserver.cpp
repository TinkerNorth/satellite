// SPDX-License-Identifier: LGPL-3.0-or-later
#include "webserver.h"
#include "tls.h"
#include "crypto.h"
#include "config.h"
#include "pairing.h"
#include "pairing_service.h"
#include "session_crypto.h"
#include "core/catalog.h"
#include "core/gamepad_backend.h"
#include "core/json_mini.h"
#include "core/session_service.h"
#include "core/update_service.h"
#include "core/update_types.h"
#include "core/version.h"
#include "core/network_info.h"
#include "local_iface.h"
#include "mdns_protocol.h"

#include <sodium.h>

using satellite::jsonGetArrayObjects;
using satellite::jsonGetBoolKeyed;
using satellite::jsonGetIntKeyed;
using satellite::jsonGetObject;

// Web UI keys all backend-status copy off (id, errorCode).
static std::string buildBackendJson() {
    BackendStatus s = probeBackend();
    std::string json = "{\"id\":\"";
    json += s.id;
    json += "\",\"supported\":";
    json += s.supported ? "true" : "false";
    json += ",\"available\":";
    json += s.available ? "true" : "false";
    json += ",\"errorCode\":";
    if (s.errorCode == nullptr) {
        json += "null";
    } else {
        json += "\"";
        json += s.errorCode;
        json += "\"";
    }
    json += "}";
    return json;
}

// Static facts about the backend that shape the catalog — keyed off the
// backend's identity, not its live health (the catalog only changes on
// server upgrade; live health is /api/server/capabilities).
static satellite::CatalogBackendTraits catalogBackendTraits() {
    BackendStatus s = probeBackend();
    satellite::CatalogBackendTraits t;
    const std::string id = s.id;
    if (id == BACKEND_ID_VIGEM) {
        t.ds4MotionSupported = true;
        t.ds4MotionRequires = "vigembus>=1.17";
        t.ds4TouchpadSupported = true;
        t.ds4LightbarSupported = true;
        t.mouseControlSupported = true;
    } else if (id == BACKEND_ID_UINPUT) {
        t.ds4MotionSupported = true;
        t.ds4TouchpadSupported = true;
        t.ds4LightbarSupported = true;
        t.mouseControlSupported = true;
    }
    return t;
}

// GET /api/server/capabilities — CURRENT dynamic state (contract.md layer 2;
// the static what-exists layer is /api/catalog).
static std::string buildCapabilitiesJson() {
    BackendStatus s = probeBackend();
    satellite::CatalogBackendTraits traits = catalogBackendTraits();
    std::string json = "{\"protocolVersion\":" + std::to_string(PROTOCOL_VERSION);
    json += ",\"serverVersion\":\"";
    json += SATELLITE_VERSION;
    json += "\",\"maxControllers\":" + std::to_string(MAX_BACKEND_CONTROLLERS);
    json += ",\"backend\":" + buildBackendJson();
    json += ",\"motion\":{\"available\":";
    json += (s.available && traits.ds4MotionSupported) ? "true" : "false";
    json += "}}";
    return json;
}

static std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return "";
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Keys must stay in sync with the web/ JS that consumes them.
static std::string buildUpdateJson(const UpdateStatusSnapshot& s) {
    std::string json = "{";
    json += "\"state\":\"" + std::string(updateStateName(s.state)) + "\"";
    json += ",\"currentVersion\":\"" + jsonEscape(s.currentVersion) + "\"";
    json += ",\"platformId\":\"" + jsonEscape(s.platformId) + "\"";
    json += ",\"channel\":\"" + jsonEscape(s.channel) + "\"";
    json += ",\"autoCheck\":" + std::string(s.autoCheck ? "true" : "false");
    json += ",\"autoDownload\":" + std::string(s.autoDownload ? "true" : "false");
    json += ",\"autoInstall\":" + std::string(s.autoInstall ? "true" : "false");
    json += ",\"lastCheckEpoch\":" + std::to_string(s.lastCheckEpoch);
    json += ",\"bytesDownloaded\":" + std::to_string(s.bytesDownloaded);
    json += ",\"totalBytes\":" + std::to_string(s.totalBytes);
    json += ",\"message\":\"" + jsonEscape(s.message) + "\"";
    json += ",\"failedPhase\":\"" + std::string(updateStateName(s.failedPhase)) + "\"";
    json += ",\"info\":{";
    json += "\"available\":" + std::string(s.info.available ? "true" : "false");
    json += ",\"version\":\"" + jsonEscape(s.info.version) + "\"";
    json += ",\"channel\":\"" + jsonEscape(s.info.channel) + "\"";
    json += ",\"assetName\":\"" + jsonEscape(s.info.assetName) + "\"";
    json += ",\"assetSize\":" + std::to_string(s.info.assetSize);
    json += ",\"assetSha256\":\"" + jsonEscape(s.info.assetSha256) + "\"";
    json += ",\"htmlUrl\":\"" + jsonEscape(s.info.htmlUrl) + "\"";
    json += ",\"publishedAtEpoch\":" + std::to_string(s.info.publishedAtEpoch);
    json += ",\"installMethod\":\"" +
            std::string(s.info.installMethod == InstallMethod::SelfInstall ? "self" : "manual") +
            "\"";
    json += ",\"manualInstruction\":\"" + jsonEscape(s.info.manualInstruction) + "\"";
    json += ",\"releaseNotes\":\"" + jsonEscape(s.info.releaseNotes) + "\"";
    json += "}}";
    return json;
}

static std::string boolStr(bool v) { return v ? "true" : "false"; }

static std::string capsJson(uint16_t caps) {
    std::string j = "{\"rumble\":" + boolStr((caps & CAP_RUMBLE) != 0);
    j += ",\"motion\":" + boolStr((caps & CAP_MOTION) != 0);
    j += ",\"analogTriggers\":" + boolStr((caps & CAP_ANALOG_TRIGGERS) != 0);
    j += ",\"lightbar\":" + boolStr((caps & CAP_LIGHTBAR) != 0);
    j += "}";
    return j;
}

// `state` fields serialise as lowercase enum names (deviceLinkStateName /
// controllerStateName in core/types.h) — the canonical wire form.
// `connectedAtEpoch` is steady-clock seconds (boot-relative), not Unix epoch.
static std::string buildConnectionsJson(const SessionService& svc) {
    auto snap = svc.getConnectionsSnapshot();
    std::string json = "{\"connections\":[";
    bool first = true;
    for (const auto& cs : snap.connections) {
        if (!first) json += ",";
        first = false;

        json += "{\"connectionId\":\"" + jsonEscape(cs.connectionId) + "\"";
        json += ",\"deviceId\":\"" + jsonEscape(cs.deviceId) + "\",\"deviceName\":\"" +
                jsonEscape(cs.deviceName) + "\",\"senderIP\":\"" + jsonEscape(cs.clientIP) + "\"";

        json += ",\"connectedAtEpoch\":" + std::to_string(cs.connectedAtEpoch);
        json += ",\"epoch\":" + std::to_string(cs.epoch);
        json += ",\"mouseControlGranted\":" + boolStr(cs.mouseControlGranted);
        // Active or NotResponding here; /api/devices covers the Paired (offline) case.
        json += ",\"state\":\"" + std::string(deviceLinkStateName(cs.linkState)) + "\"";

        json += ",\"controllers\":[";
        bool cfirst = true;
        for (const auto& ctrl : cs.controllers) {
            if (!cfirst) json += ",";
            cfirst = false;
            // Only Live/Detached are surfaced today; transient states aren't yet
            // threaded through SessionService. See ControllerState in core/types.h.
            const char* ctrlState = ctrl.active ? controllerStateName(ControllerState::Live)
                                                : controllerStateName(ControllerState::Detached);
            json += "{\"controllerIndex\":" + std::to_string(ctrl.index) +
                    ",\"serialNo\":" + std::to_string(ctrl.serial) +
                    ",\"pluggedIn\":" + boolStr(ctrl.pluggedIn) + ",\"state\":\"" +
                    std::string(ctrlState) + "\"" + ",\"controllerType\":\"" +
                    controllerTypeName(ctrl.controllerType) + "\",\"controllerTypeLabel\":\"" +
                    controllerTypeLabel(ctrl.controllerType) + "\",\"touchpadMode\":\"" +
                    touchpadModeName(ctrl.touchpadMode) + "\"";
            if (ctrl.batteryKnown) {
                json += ",\"battery\":{";
                if (ctrl.batteryLevel == BATTERY_LEVEL_UNKNOWN) {
                    json += "\"level\":null";
                } else {
                    json += "\"level\":" + std::to_string(ctrl.batteryLevel);
                }
                json +=
                    ",\"status\":\"" + std::string(batteryStatusName(ctrl.batteryStatus)) + "\"}";
            } else {
                json += ",\"battery\":null";
            }
            json += ",\"motionCapable\":" + boolStr(ctrl.motionCapable);
            json += ",\"motionActive\":" + boolStr(ctrl.motionActive);
            json += ",\"motionSink\":" + boolStr(ctrl.motionSink);
            // Backend has an IMU surface for this controller type; UI warns when
            // motionCapable but not this (motion has nowhere to land, e.g. Xbox pad).
            json += ",\"motionSinkSupportedForType\":" + boolStr(ctrl.motionSinkSupportedForType);
            // IMU sink was created at plug-in; false flags a kernel-level failure
            // (uinput perms, kernel too old) vs. just "no game subscribed".
            json += ",\"motionBackendOk\":" + boolStr(ctrl.motionBackendOk);
            json += ",\"touchpadActive\":" + boolStr(ctrl.touchpadActive);
            json += ",\"lightbarCapable\":" + boolStr(ctrl.lightbarCapable);
            if (ctrl.lightbarKnown) {
                char rgb[8];
                snprintf(rgb, sizeof(rgb), "#%02x%02x%02x", ctrl.lightbarR, ctrl.lightbarG,
                         ctrl.lightbarB);
                json += ",\"lightbar\":\"" + std::string(rgb) + "\"";
            } else {
                json += ",\"lightbar\":null";
            }
            json += "}";
        }
        json += "],\"activeControllerCount\":" + std::to_string(cs.activeControllerCount) + "}";
    }
    json += "],\"totalControllers\":" + std::to_string(snap.totalControllers) +
            ",\"maxControllers\":" + std::to_string(snap.maxControllers) +
            ",\"backendAvailable\":" + boolStr(snap.backendAvailable) + "}";
    return json;
}

// Paired devices + their live link state — `state` is paired | active |
// notResponding. Shared by the admin route and the SSE devices event.
static std::string buildDevicesJson(const SessionService& svc) {
    std::string json = "[";
    std::lock_guard<std::mutex> lk(g_configMtx);
    for (size_t i = 0; i < g_config.pairedDevices.size(); i++) {
        const auto& d = g_config.pairedDevices[i];
        DeviceLinkState s = svc.linkStateForDevice(d.id);
        json += "{\"id\":\"" + jsonEscape(d.id) + "\",\"name\":\"" + jsonEscape(d.name) +
                "\",\"lastIP\":\"" + jsonEscape(d.lastIP) + "\",\"pairedAt\":\"" +
                jsonEscape(d.pairedAt) + "\",\"state\":\"" + deviceLinkStateName(s) + "\"}";
        if (i + 1 < g_config.pairedDevices.size()) json += ",";
    }
    json += "]";
    return json;
}

static std::string buildPinJson() {
    PinSnapshot s = pinSnapshot();
    return std::string("{\"state\":\"") + pinStateName(s.state) + "\",\"currentPin\":\"" +
           s.currentPin + "\",\"previousPin\":\"" + s.previousPin +
           "\",\"secondsRemaining\":" + std::to_string(s.secondsRemaining) + "}";
}

static std::string buildPairRequestsJson() {
    auto reqs = pendingPairRequests();
    std::string json = "[";
    for (size_t i = 0; i < reqs.size(); i++) {
        const auto& r = reqs[i];
        if (i) json += ",";
        json += "{\"deviceId\":\"" + jsonEscape(r.deviceId) + "\",\"deviceName\":\"" +
                jsonEscape(r.deviceName) + "\",\"clientIP\":\"" + jsonEscape(r.clientIP) +
                "\",\"pin\":\"" + jsonEscape(r.pin) +
                "\",\"secondsRemaining\":" + std::to_string(r.secondsRemaining) + "}";
    }
    json += "]";
    return json;
}

// ── Client auth (HTTPS surface) ──────────────────────────────────────────────

struct ClientAuth {
    std::string deviceId;
    PairedDevice device; // copy-by-value under g_configMtx — never a pointer
    uint8_t pairingKey[CRYPTO_KEY_SIZE];
};

static std::string headerOrBody(const httplib::Request& req, const char* header,
                                const char* bodyKey) {
    auto hdr = req.headers.find(header);
    if (hdr != req.headers.end() && !hdr->second.empty()) return hdr->second;
    if (!req.body.empty()) return jsonGetString(req.body, bodyKey);
    return "";
}

// Every authenticated client route requires a paired deviceId AND an hmacProof
// of the pairing key, so a diverged key fails HERE with a terminal 401 instead
// of producing a silently-undecryptable UDP session. The PairedDevice is
// copied by value under g_configMtx — a concurrent unpair can't dangle it.
static bool clientAuthed(const httplib::Request& req, httplib::Response& res, ClientAuth& out) {
    out.deviceId = headerOrBody(req, "X-Device-Id", "deviceId");
    const std::string proof = headerOrBody(req, "X-Hmac-Proof", "hmacProof");

    const char* code = "NOT_PAIRED";
    if (!out.deviceId.empty()) {
        bool found = false;
        {
            std::lock_guard<std::mutex> lk(g_configMtx);
            for (const auto& d : g_config.pairedDevices) {
                if (d.id == out.deviceId) {
                    out.device = d;
                    found = true;
                    break;
                }
            }
        }
        if (found) {
            if (hexDecode(out.device.sharedKeyHex, out.pairingKey, CRYPTO_KEY_SIZE) &&
                verifyHmacProof(out.pairingKey, out.deviceId, proof)) {
                return true;
            }
            code = "BAD_PROOF";
        }
    }

    logMsg(LogLevel::WARN, "client",
           "401 unauthorized " + req.method + " " + req.path + " (" + code +
               (out.deviceId.empty() ? ", no deviceId supplied" : ", deviceId " + out.deviceId) +
               ")");
    res.status = 401;
    res.set_content(std::string(R"({"error":"unauthorized","code":")") + code + R"("})",
                    "application/json");
    return false;
}

static bool protocolVersionOk(const std::string& body, httplib::Response& res) {
    long pv = PROTOCOL_VERSION;
    if (jsonGetIntKeyed(body, "protocolVersion", &pv) && pv != PROTOCOL_VERSION) {
        res.status = 409;
        res.set_content("{\"error\":\"protocol version unsupported\",\"supported\":" +
                            std::to_string(PROTOCOL_VERSION) + "}",
                        "application/json");
        return false;
    }
    return true;
}

// ── Descriptor parsing (session/controller PUT bodies) ──────────────────────

// One ControllerDescriptor from its JSON object. `type` is REQUIRED — a
// descriptor without it would force a server-side default type, which is
// exactly the default-then-correct bug class this contract removes.
static bool parseDescriptorObject(const std::string& obj, bool requireIdx,
                                  ControllerDescriptor& d) {
    long idx = 0;
    if (jsonGetIntKeyed(obj, "ctrlIdx", &idx)) {
        if (idx < 0) return false;
        d.ctrlIdx = idx > 255 ? 255 : static_cast<uint8_t>(idx);
    } else if (requireIdx) {
        return false;
    }
    long type = 0;
    if (!jsonGetIntKeyed(obj, "type", &type) || type < 0) return false;
    // Out-of-range values pass through; the service reports invalidType per
    // controller rather than failing the whole request.
    d.type = type > 255 ? 255 : static_cast<uint8_t>(type);

    d.caps = 0;
    const std::string caps = jsonGetObject(obj, "caps");
    bool b = false;
    if (jsonGetBoolKeyed(caps, "rumble", &b) && b) d.caps |= CAP_RUMBLE;
    b = false;
    if (jsonGetBoolKeyed(caps, "motion", &b) && b) d.caps |= CAP_MOTION;
    b = false;
    if (jsonGetBoolKeyed(caps, "analogTriggers", &b) && b) d.caps |= CAP_ANALOG_TRIGGERS;
    b = false;
    if (jsonGetBoolKeyed(caps, "lightbar", &b) && b) d.caps |= CAP_LIGHTBAR;

    const std::string mode = jsonGetString(obj, "touchpadMode");
    if (mode == "ds4") {
        d.touchpadMode = TOUCHPAD_MODE_DS4;
    } else if (mode == "mouse") {
        d.touchpadMode = TOUCHPAD_MODE_MOUSE;
    } else {
        d.touchpadMode = TOUCHPAD_MODE_OFF;
    }
    return true;
}

static bool parseControllerDescriptors(const std::string& body,
                                       std::vector<ControllerDescriptor>& out) {
    auto objs = jsonGetArrayObjects(body, "controllers");
    for (const auto& obj : objs) {
        ControllerDescriptor d;
        if (!parseDescriptorObject(obj, /*requireIdx=*/true, d)) return false;
        out.push_back(d);
    }
    return true;
}

// ── Session response builders ────────────────────────────────────────────────

static std::string controllerApplyJson(const ControllerApplyResult& r) {
    std::string j = "{\"ctrlIdx\":" + std::to_string(r.ctrlIdx);
    j += ",\"result\":\"" + std::string(applyResultName(r.result)) + "\"";
    j += ",\"appliedType\":" + std::to_string(r.appliedType);
    j += ",\"motion\":{\"sinkSupportedForType\":" + boolStr(r.motionSinkSupportedForType);
    j += ",\"backendOk\":" + boolStr(r.motionBackendOk) + "}}";
    return j;
}

static std::string mouseControlJson(bool granted, const std::string& denyReason) {
    std::string j = "{\"granted\":" + boolStr(granted);
    if (!granted && !denyReason.empty()) j += ",\"reason\":\"" + jsonEscape(denyReason) + "\"";
    j += "}";
    return j;
}

static std::string buildUpsertResponseJson(const SessionUpsertResult& r) {
    char tokenHex[9];
    snprintf(tokenHex, sizeof(tokenHex), "%08x", r.token);

    std::string json = "{\"connectionId\":\"" + jsonEscape(r.connectionId) + "\"";
    json += ",\"token\":\"";
    json += tokenHex;
    json += "\",\"sessionSalt\":\"" + hexEncode(r.sessionSalt, SESSION_SALT_SIZE) + "\"";
    json += ",\"epoch\":" + std::to_string(r.epoch);
    json += ",\"maxControllers\":" + std::to_string(r.maxControllers);
    json += ",\"protocolVersion\":" + std::to_string(PROTOCOL_VERSION);
    json += ",\"controllers\":[";
    for (size_t i = 0; i < r.controllers.size(); i++) {
        if (i) json += ",";
        json += controllerApplyJson(r.controllers[i]);
    }
    json += "],\"hostFeatures\":{\"mouseControl\":" +
            mouseControlJson(r.mouseControlGranted, r.mouseControlDenyReason) + "}}";
    return json;
}

static std::string buildSessionViewJson(const SessionService::SessionView& v) {
    std::string json = "{\"connectionId\":\"" + jsonEscape(v.connectionId) + "\"";
    json += ",\"deviceId\":\"" + jsonEscape(v.deviceId) + "\"";
    json += ",\"epoch\":" + std::to_string(v.epoch);
    json += ",\"protocolVersion\":" + std::to_string(PROTOCOL_VERSION);
    json += ",\"maxControllers\":" + std::to_string(MAX_BACKEND_CONTROLLERS);
    json += ",\"controllers\":[";
    for (size_t i = 0; i < v.controllers.size(); i++) {
        const auto& c = v.controllers[i];
        if (i) json += ",";
        json += "{\"ctrlIdx\":" + std::to_string(c.ctrlIdx) + ",\"active\":true";
        json += ",\"appliedType\":" + std::to_string(c.appliedType);
        json += ",\"caps\":" + capsJson(c.caps);
        json += ",\"touchpadMode\":\"" + std::string(touchpadModeName(c.touchpadMode)) + "\"";
        json += ",\"motion\":{\"sinkSupportedForType\":" + boolStr(c.motionSinkSupportedForType);
        json += ",\"backendOk\":" + boolStr(c.motionBackendOk) + "}}";
    }
    json += "],\"hostFeatures\":{\"mouseControl\":" + mouseControlJson(v.mouseControlGranted, "") +
            "}}";
    return json;
}

// ── Client session routes ────────────────────────────────────────────────────

// PUT /api/connections — the declarative upsert (docs/contract.md §Session).
// Connect + full topology = ONE call; re-PUT converges; partial success rides
// in the body, never in the HTTP status.
static void upsertConnectionRoute(SessionService& svc, const httplib::Request& req,
                                  httplib::Response& res) {
    if (!g_appRunning) {
        res.status = 503;
        res.set_content(R"({"error":"shutting down"})", "application/json");
        return;
    }
    ClientAuth auth;
    if (!clientAuthed(req, res, auth)) return;
    if (!protocolVersionOk(req.body, res)) return;

    std::string deviceName = jsonGetString(req.body, "deviceName");
    if (deviceName.empty()) deviceName = auth.device.name;

    std::vector<ControllerDescriptor> descriptors;
    if (!parseControllerDescriptors(req.body, descriptors)) {
        logMsg(LogLevel::WARN, "client",
               "PUT /api/connections: malformed controllers array (ctrlIdx and type are "
               "required) from " +
                   auth.deviceId);
        res.status = 400;
        res.set_content(R"({"error":"controllers entries require ctrlIdx and type"})",
                        "application/json");
        return;
    }

    bool mouseRequested = false;
    const std::string hostFeatures = jsonGetObject(req.body, "hostFeatures");
    if (!hostFeatures.empty()) jsonGetBoolKeyed(hostFeatures, "mouseControl", &mouseRequested);

    auto result = svc.upsertSession(auth.deviceId, deviceName, req.remote_addr, auth.pairingKey,
                                    descriptors, mouseRequested);
    if (!result.ok) {
        res.status = 500;
        res.set_content("{\"error\":\"" + jsonEscape(result.error) + "\"}", "application/json");
        return;
    }

    // Refresh the paired record's last-seen identity (name can change on the
    // client between sessions).
    {
        std::lock_guard<std::mutex> lk(g_configMtx);
        for (auto& d : g_config.pairedDevices) {
            if (d.id == auth.deviceId) {
                d.lastIP = req.remote_addr;
                d.name = deviceName;
                break;
            }
        }
        saveConfig(g_config);
    }

    res.set_content(buildUpsertResponseJson(result), "application/json");
}

// ── Pairing routes ───────────────────────────────────────────────────────────

// Dual-path device pairing over HTTPS (PINs + pairing key encrypted in transit).
// Path A: `pin` (server-generated, typed into the dish) → verifyPin, pair now.
// Path B: `clientPin` (dish-shown) → register a request, reply pending=true; the
//   operator accepts on the dashboard/tray and the dish polls /api/pair/status.
// Key rotation: `hmacProof` of the CURRENT key mints and returns a fresh key.
// There is NO PIN-free already-paired short-circuit: handing the stored key to
// anyone who learned a deviceId let any LAN actor exfiltrate it.
// Always 200 on the PIN paths; the sender classifies on `ok`/`pending`.
static void pairRoute(SessionService& svc, const httplib::Request& req, httplib::Response& res) {
    auto deviceId = jsonGetString(req.body, "deviceId");
    auto deviceName = jsonGetString(req.body, "deviceName");
    auto pin = jsonGetString(req.body, "pin");               // server-shown PIN (Path A)
    auto clientPin = jsonGetString(req.body, "clientPin");   // dish-shown PIN (Path B)
    auto clientPkHex = jsonGetString(req.body, "publicKey"); // client's X25519 public key
    auto hmacProof = jsonGetString(req.body, "hmacProof");   // key-rotation proof
    const std::string clientIP = req.remote_addr;

    if (deviceId.empty()) {
        res.set_content(R"({"ok":false,"error":"missing deviceId"})", "application/json");
        return;
    }
    if (!protocolVersionOk(req.body, res)) return;

    // Key rotation / re-pair with proof of the current key. A failed proof
    // falls through to the PIN paths — identical to a fresh pairing attempt.
    if (!hmacProof.empty()) {
        PairedDevice dev;
        bool found = false;
        {
            std::lock_guard<std::mutex> lk(g_configMtx);
            for (const auto& d : g_config.pairedDevices) {
                if (d.id == deviceId) {
                    dev = d;
                    found = true;
                    break;
                }
            }
        }
        uint8_t currentKey[CRYPTO_KEY_SIZE];
        if (found && hexDecode(dev.sharedKeyHex, currentKey, CRYPTO_KEY_SIZE) &&
            verifyHmacProof(currentKey, deviceId, hmacProof)) {
            std::string newKeyHex;
            rotatePairedDeviceKey(deviceId, clientIP, newKeyHex);
            // The old key dies with the rotation, so any live session keyed off
            // it must die too.
            svc.closeSessionsForDevice(deviceId, CLOSE_REASON_REPLACED);
            logMsg(LogLevel::INFO, "pairing",
                   "Rotated pairing key for " + deviceId + " (" + clientIP + ")");
            res.set_content(R"({"ok":true,"message":"key rotated","sharedKey":")" + newKeyHex +
                                R"(","protocolVersion":)" + std::to_string(PROTOCOL_VERSION) + "}",
                            "application/json");
            return;
        }
        logMsg(LogLevel::WARN, "pairing",
               "Rejected proof-based re-pair for " + deviceId + " (" + clientIP + ")");
    }

    // Path A — dish entered the operator's server-generated PIN.
    if (!pin.empty() && verifyPin(pin)) {
        uint8_t serverPk[32], serverSk[32];
        generateKeyPair(serverPk, serverSk);

        // Client key is optional — absent in trusted-network mode.
        uint8_t clientPk[32];
        bool hasClientKey = !clientPkHex.empty() && hexDecode(clientPkHex, clientPk, 32);

        std::string sharedKeyHex;
        if (hasClientKey) {
            uint8_t sharedKey[32];
            if (computeSharedKey(sharedKey, clientPk, serverSk, serverPk)) {
                sharedKeyHex = hexEncode(sharedKey, 32);
                sodium_memzero(sharedKey, 32);
            }
        }
        if (sharedKeyHex.empty()) {
            // No key exchange — mint a random key, returned over TLS.
            uint8_t randomKey[32];
            randombytes_buf(randomKey, 32);
            sharedKeyHex = hexEncode(randomKey, 32);
            sodium_memzero(randomKey, 32);
        }

        upsertPairedDevice(deviceId, deviceName, clientIP, sharedKeyHex);
        // A re-pair invalidates the previous key; a session still keyed off it
        // would churn undecryptably, so close it now.
        svc.closeSessionsForDevice(deviceId, CLOSE_REASON_REPLACED);

        std::string serverPkHex = hexEncode(serverPk, 32);
        sodium_memzero(serverSk, 32);
        logMsg(LogLevel::INFO, "pairing",
               "Paired device via server PIN: " + deviceId + " (" + clientIP + ")");
        if (hasClientKey) {
            res.set_content(R"({"ok":true,"message":"paired successfully","serverPublicKey":")" +
                                serverPkHex + R"(","protocolVersion":)" +
                                std::to_string(PROTOCOL_VERSION) + "}",
                            "application/json");
        } else {
            res.set_content(R"({"ok":true,"message":"paired successfully","sharedKey":")" +
                                sharedKeyHex + R"(","protocolVersion":)" +
                                std::to_string(PROTOCOL_VERSION) + "}",
                            "application/json");
        }
        return;
    }

    // Path B — register the dish's request; it then polls /api/pair/status. The
    // clientPin is never echoed server-side — the operator must read it off the
    // dish, which is what makes the accept meaningful.
    if (!clientPin.empty()) {
        submitPairRequest(deviceId, deviceName, clientIP, clientPin);
        logMsg(LogLevel::INFO, "pairing",
               "Pairing request from " + (deviceName.empty() ? deviceId : deviceName) + " (" +
                   clientIP + ") awaiting operator approval");
        res.set_content(
            R"({"ok":false,"pending":true,"message":"awaiting approval on the satellite"})",
            "application/json");
        return;
    }

    logMsg(LogLevel::WARN, "pairing", "Invalid or empty PIN attempt from " + clientIP);
    res.set_content(R"({"ok":false,"error":"invalid or expired PIN"})", "application/json");
}

// DELETE /api/pair — client self-unpair (hmacProof-authed). Closes any live
// session first (close-notify reason=unpaired rides the still-valid key).
static void selfUnpairRoute(SessionService& svc, const httplib::Request& req,
                            httplib::Response& res) {
    ClientAuth auth;
    if (!clientAuthed(req, res, auth)) return;

    svc.closeSessionsForDevice(auth.deviceId, CLOSE_REASON_UNPAIRED);
    {
        std::lock_guard<std::mutex> lk(g_configMtx);
        auto& devs = g_config.pairedDevices;
        devs.erase(std::remove_if(devs.begin(), devs.end(),
                                  [&](const PairedDevice& d) { return d.id == auth.deviceId; }),
                   devs.end());
        saveConfig(g_config);
    }
    logMsg(LogLevel::INFO, "pairing", "Device self-unpaired: " + auth.deviceId);
    res.set_content(R"({"ok":true})", "application/json");
}

// ── Catalog routes (unauthenticated — the UI renders BEFORE pairing) ────────

static void catalogRoute(const httplib::Request& req, httplib::Response& res) {
    const std::string locale =
        satellite::resolveCatalogLocale(req.get_header_value("Accept-Language"));
    const std::string etag = satellite::catalogETag(SATELLITE_VERSION, locale);
    res.set_header("ETag", etag);
    res.set_header("Vary", "Accept-Language");
    if (req.get_header_value("If-None-Match") == etag) {
        res.status = 304;
        return;
    }
    const std::string langJson = readFile(g_webDir + "/lang/" + locale + ".json");
    const std::string enJson = (locale == "en") ? langJson : readFile(g_webDir + "/lang/en.json");
    res.set_content(satellite::buildCatalogJson(locale, langJson, enJson, SATELLITE_VERSION,
                                                catalogBackendTraits()),
                    "application/json");
}

static void catalogImageRoute(const httplib::Request& req, httplib::Response& res) {
    const std::string slug = req.matches[1].str();
    bool known = false;
    for (const auto& s : satellite::catalogImageSlugs()) {
        if (s == slug) {
            known = true;
            break;
        }
    }
    std::string svg = known ? readFile(g_webDir + "/img/catalog/" + slug + ".svg") : "";
    if (svg.empty()) {
        res.status = 404;
        res.set_content(R"({"error":"unknown catalog image"})", "application/json");
        return;
    }
    const std::string etag = std::string("\"") + SATELLITE_VERSION + "\"";
    res.set_header("ETag", etag);
    if (req.get_header_value("If-None-Match") == etag) {
        res.status = 304;
        return;
    }
    res.set_content(svg, "image/svg+xml");
}

// Admin server — web UI + admin API. Plain HTTP, 127.0.0.1, no auth.

// Origin guard closing the two browser-borne attacks that cross the loopback
// trust boundary: DNS rebinding (reject non-loopback Host) and CSRF (reject
// writes whose Origin is present and non-loopback). See SECURITY.md.
static bool isLoopbackHostValue(const std::string& v) {
    // Strip an optional :port, and surrounding [] for IPv6 literals.
    std::string h = v.substr(0, v.rfind(':') == std::string::npos ? v.size() : v.rfind(':'));
    if (!h.empty() && h.front() == '[' && h.back() == ']') h = h.substr(1, h.size() - 2);
    return h == "127.0.0.1" || h == "localhost" || h == "::1";
}
static bool isLoopbackOrigin(const std::string& origin) {
    // origin is like "http://127.0.0.1:8080" — match the host segment.
    auto schemeEnd = origin.find("//");
    if (schemeEnd == std::string::npos) return false;
    return isLoopbackHostValue(origin.substr(schemeEnd + 2));
}

void adminHttpThread(SessionService& svc) {
    // Reject cross-origin / rebound requests before any route runs.
    g_httpServer.set_pre_routing_handler([](const httplib::Request& req, httplib::Response& res) {
        auto host = req.get_header_value("Host");
        if (!host.empty() && !isLoopbackHostValue(host)) {
            res.status = 403;
            res.set_content(R"({"error":"forbidden host"})", "application/json");
            return httplib::Server::HandlerResponse::Handled;
        }
        if (req.method != "GET" && req.method != "HEAD" && req.method != "OPTIONS") {
            auto origin = req.get_header_value("Origin");
            if (!origin.empty() && !isLoopbackOrigin(origin)) {
                res.status = 403;
                res.set_content(R"({"error":"cross-site request blocked"})", "application/json");
                return httplib::Server::HandlerResponse::Handled;
            }
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });

    g_httpServer.set_mount_point("/", g_webDir);

    g_httpServer.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_redirect("/dashboard");
    });

    // SPA routes fall back to index.html.
    auto serveIndex = [](const httplib::Request&, httplib::Response& res) {
        std::string html = readFile(g_webDir + "/index.html");
        if (html.empty()) {
            res.status = 404;
            return;
        }
        res.set_content(html, "text/html");
    };
    g_httpServer.Get("/dashboard", serveIndex);
    g_httpServer.Get("/settings", serveIndex);
    g_httpServer.Get("/debug", serveIndex);
    g_httpServer.Get("/logs", serveIndex);
    g_httpServer.Get("/donate", serveIndex);

    g_httpServer.Get("/api/backend/status", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(buildBackendJson(), "application/json");
    });

    g_httpServer.Get("/api/status", [&svc](const httplib::Request&, httplib::Response& res) {
        char senderIP[INET_ADDRSTRLEN] = "none";
        uint32_t ipRaw = g_senderIP.load(std::memory_order_relaxed);
        if (ipRaw != 0) {
            in_addr ia;
            ia.s_addr = ipRaw;
            inet_ntop(AF_INET, &ia, senderIP, sizeof(senderIP));
        }
        std::string backendJson = buildBackendJson();
        bool backendUp = svc.isBackendAvailable();
        char json[1024];
        snprintf(
            json, sizeof(json),
            R"({"listening":%s,"packets":%llu,"senderIP":"%s","udpPort":%d,"webPort":%d,"autoStart":%s,"discoveryBroadcastEnabled":%s,"mdnsResponderActive":%s,"backendAvailable":%s,"backend":%s})",
            g_listening.load() ? "true" : "false", (unsigned long long)g_packetCount.load(),
            senderIP, g_config.udpPort, g_config.webPort, g_config.autoStart ? "true" : "false",
            g_config.discoveryBroadcastEnabled ? "true" : "false",
            g_mdnsResponderActive.load() ? "true" : "false", backendUp ? "true" : "false",
            backendJson.c_str());
        res.set_content(json, "application/json");
    });

    g_httpServer.Get("/api/netinfo", [](const httplib::Request&, httplib::Response& res) {
        NetworkInfo info;
        {
            std::lock_guard<std::mutex> lk(g_configMtx);
            info.udpPort = g_config.udpPort;
            info.webPort = g_config.webPort;
            info.pairPort = g_config.pairPort;
            info.discPort = g_config.discPort;
        }
        info.clientPort = DEFAULT_CLIENT_PORT;
        info.mdnsPort = mdns::MULTICAST_PORT;
        resolveLocalInterface(info.lanIp, info.device);
        res.set_content(buildNetworkInfoJson(info), "application/json");
    });

    g_httpServer.Post("/api/config", [](const httplib::Request& req, httplib::Response& res) {
        const std::string& body = req.body;
        bool portRejected = false;
        std::lock_guard<std::mutex> lk(g_configMtx);

        // Out-of-range ports are rejected, not clamped; the response echoes the effective port.
        long port = 0;
        if (jsonGetIntKeyed(body, "udpPort", &port)) {
            if (port >= 1024 && port <= 65535) {
                g_config.udpPort = static_cast<int>(port);
            } else {
                portRejected = true;
            }
        }

        // Applied only when present, so a partial POST leaves the stored value untouched.
        bool autoStartVal = false;
        if (jsonGetBoolKeyed(body, "autoStart", &autoStartVal)) {
            g_config.autoStart = autoStartVal;
            setAutoStart(g_config.autoStart);
        }

        // Applied only when present so a partial POST can't silently flip discovery off.
        bool broadcastVal = false;
        if (jsonGetBoolKeyed(body, "discoveryBroadcastEnabled", &broadcastVal)) {
            g_config.discoveryBroadcastEnabled = broadcastVal;
        }

        saveConfig(g_config);
        logMsg(LogLevel::INFO, "web",
               "Config updated: udpPort=" + std::to_string(g_config.udpPort) + " autoStart=" +
                   std::string(g_config.autoStart ? "true" : "false") + " broadcast=" +
                   std::string(g_config.discoveryBroadcastEnabled ? "true" : "false") +
                   (portRejected ? " (udpPort out of range — ignored)" : ""));
        std::string resp = "{\"ok\":true,\"udpPort\":" + std::to_string(g_config.udpPort) +
                           ",\"udpPortRejected\":" + (portRejected ? "true" : "false") + "}";
        res.set_content(resp, "application/json");
    });

    g_httpServer.Get("/api/version", [](const httplib::Request&, httplib::Response& res) {
        std::string json = "{\"version\":\"";
        json += SATELLITE_VERSION;
        json += "\",\"platformId\":\"";
        json += g_updateService ? g_updateService->snapshot().platformId : "unknown";
        json += "\"}";
        res.set_content(json, "application/json");
    });

    g_httpServer.Get("/api/updates/status", [](const httplib::Request&, httplib::Response& res) {
        if (!g_updateService) {
            res.status = 503;
            res.set_content(R"({"error":"updater not initialized"})", "application/json");
            return;
        }
        res.set_content(buildUpdateJson(g_updateService->snapshot()), "application/json");
    });

    g_httpServer.Post("/api/updates/check", [](const httplib::Request&, httplib::Response& res) {
        if (!g_updateService) {
            res.status = 503;
            res.set_content(R"({"error":"updater not initialized"})", "application/json");
            return;
        }
        g_updateService->requestCheck(/*userInitiated=*/true);
        res.set_content(R"({"ok":true})", "application/json");
    });

    g_httpServer.Post("/api/updates/download", [](const httplib::Request&, httplib::Response& res) {
        if (!g_updateService) {
            res.status = 503;
            res.set_content(R"({"error":"updater not initialized"})", "application/json");
            return;
        }
        g_updateService->requestDownload();
        res.set_content(R"({"ok":true})", "application/json");
    });

    g_httpServer.Post("/api/updates/install", [](const httplib::Request&, httplib::Response& res) {
        if (!g_updateService) {
            res.status = 503;
            res.set_content(R"({"error":"updater not initialized"})", "application/json");
            return;
        }
        g_updateService->requestInstall();
        res.set_content(R"({"ok":true})", "application/json");
    });

    g_httpServer.Post("/api/updates/cancel", [](const httplib::Request&, httplib::Response& res) {
        if (g_updateService) g_updateService->cancelInFlight();
        res.set_content(R"({"ok":true})", "application/json");
    });

    g_httpServer.Post("/api/updates/skip", [](const httplib::Request& req, httplib::Response& res) {
        std::string v = jsonGetString(req.body, "version");
        if (v.empty() || !g_updateService) {
            res.status = 400;
            res.set_content(R"({"error":"missing version"})", "application/json");
            return;
        }
        g_updateService->skipVersion(v);
        res.set_content(R"({"ok":true})", "application/json");
    });

    g_httpServer.Post("/api/updates/dismiss", [](const httplib::Request&, httplib::Response& res) {
        if (g_updateService) g_updateService->dismiss();
        res.set_content(R"({"ok":true})", "application/json");
    });

    g_httpServer.Post("/api/updates/preferences", [](const httplib::Request& req,
                                                     httplib::Response& res) {
        if (!g_updateService) {
            res.status = 503;
            res.set_content(R"({"error":"updater not initialized"})", "application/json");
            return;
        }
        std::string channel = jsonGetString(req.body, "channel");
        if (channel.empty()) channel = UPDATE_CHANNEL_STABLE;
        auto findBool = [&](const std::string& key) -> bool {
            bool v = false;
            jsonGetBoolKeyed(req.body, key, &v);
            return v;
        };
        bool autoCheck = findBool("autoCheck");
        bool autoDownload = findBool("autoDownload");
        bool autoInstall = findBool("autoInstall");
        g_updateService->updatePreferences(channel, autoCheck, autoDownload, autoInstall);
        logMsg(LogLevel::INFO, "web",
               "Update prefs: channel=" + channel + " autoCheck=" + (autoCheck ? "true" : "false") +
                   " autoDownload=" + (autoDownload ? "true" : "false") +
                   " autoInstall=" + (autoInstall ? "true" : "false"));
        res.set_content(R"({"ok":true})", "application/json");
    });

    // The rotating PINs are echoed here for the dashboard to display — safe
    // because this is the loopback-only admin surface.
    g_httpServer.Get("/api/pin/status", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(buildPinJson(), "application/json");
    });

    // Reverse-direction pairing (dish shows a PIN, operator accepts here).
    // Localhost admin surface — operator is at the satellite — so no device auth.
    g_httpServer.Get("/api/pair/requests", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(buildPairRequestsJson(), "application/json");
    });

    g_httpServer.Post("/api/pair/respond", [](const httplib::Request& req, httplib::Response& res) {
        auto deviceId = jsonGetString(req.body, "deviceId");
        bool accept = false;
        jsonGetBoolKeyed(req.body, "accept", &accept);
        if (deviceId.empty()) {
            res.status = 400;
            res.set_content(R"({"ok":false,"error":"missing deviceId"})", "application/json");
            return;
        }
        if (!accept) {
            declinePairing(deviceId);
            logMsg(LogLevel::INFO, "pairing", "Operator denied pairing request " + deviceId);
            res.set_content(R"({"ok":true,"accepted":false})", "application/json");
            return;
        }
        // Key minting + persistence live in pairing_service (shared with tray prompts).
        if (!confirmPairing(deviceId)) {
            logMsg(LogLevel::WARN, "pairing",
                   "Operator accept for " + deviceId + " rejected (no pending request)");
            res.set_content(R"({"ok":false,"error":"no pending request"})", "application/json");
            return;
        }
        logMsg(LogLevel::INFO, "pairing", "Operator accepted pairing for " + deviceId);
        res.set_content(R"({"ok":true,"accepted":true})", "application/json");
    });

    g_httpServer.Get("/api/devices", [&svc](const httplib::Request&, httplib::Response& res) {
        res.set_content(buildDevicesJson(svc), "application/json");
    });

    // Admin unpair. Closes any live session for the device first — an unpaired
    // device must not keep streaming on a key the server no longer trusts.
    g_httpServer.Delete(
        R"(/api/devices/([^/]+))", [&svc](const httplib::Request& req, httplib::Response& res) {
            auto deviceId = req.matches[1].str();
            int closed = svc.closeSessionsForDevice(deviceId, CLOSE_REASON_UNPAIRED);
            bool removed = false;
            {
                std::lock_guard<std::mutex> lk(g_configMtx);
                auto& devs = g_config.pairedDevices;
                size_t before = devs.size();
                devs.erase(std::remove_if(devs.begin(), devs.end(),
                                          [&](const PairedDevice& d) { return d.id == deviceId; }),
                           devs.end());
                removed = devs.size() != before;
                if (removed) saveConfig(g_config);
            }
            if (!removed) {
                res.status = 404;
                res.set_content(R"({"error":"device not paired"})", "application/json");
                return;
            }
            logMsg(LogLevel::INFO, "pairing",
                   "Unpaired device " + deviceId + (closed > 0 ? " (live session closed)" : ""));
            res.set_content("{\"ok\":true,\"sessionsClosed\":" + std::to_string(closed) + "}",
                            "application/json");
        });

    g_httpServer.Get("/api/server/capabilities",
                     [](const httplib::Request&, httplib::Response& res) {
                         res.set_content(buildCapabilitiesJson(), "application/json");
                     });

    g_httpServer.Get("/api/debug", [&svc](const httplib::Request&, httplib::Response& res) {
        char senderIP[INET_ADDRSTRLEN] = "none";
        uint32_t ipRaw = g_senderIP.load(std::memory_order_relaxed);
        if (ipRaw != 0) {
            in_addr ia;
            ia.s_addr = ipRaw;
            inet_ntop(AF_INET, &ia, senderIP, sizeof(senderIP));
        }
        uint64_t maxUs = g_maxLoopUs.exchange(0, std::memory_order_relaxed);
        char json[1536];
        std::string backendJson = buildBackendJson();
        bool backendUp = svc.isBackendAvailable();
        snprintf(json, sizeof(json),
                 R"({"listening":%s,"packets":%llu,"submitOk":%llu,"submitFail":%llu,)"
                 R"("lastLoopUs":%llu,"maxLoopUs":%llu,"senderIP":"%s","udpPort":%d,)"
                 R"("decryptFail":%llu,"replayDrop":%llu,)"
                 R"("backendAvailable":%s,"backend":%s})",
                 g_listening.load() ? "true" : "false", (unsigned long long)g_packetCount.load(),
                 (unsigned long long)g_submitOk.load(), (unsigned long long)g_submitFail.load(),
                 (unsigned long long)g_lastLoopUs.load(), (unsigned long long)maxUs, senderIP,
                 g_config.udpPort, (unsigned long long)g_decryptFail.load(),
                 (unsigned long long)g_replayDrop.load(), backendUp ? "true" : "false",
                 backendJson.c_str());
        res.set_content(json, "application/json");
    });

    g_httpServer.Get("/api/connections", [&svc](const httplib::Request&, httplib::Response& res) {
        res.set_content(buildConnectionsJson(svc), "application/json");
    });

    // Admin kick — transient by design (a retrying client may re-PUT and
    // reconnect; to keep a device out, unpair it). Close-notify rides first.
    g_httpServer.Delete(
        R"(/api/connections/(\w+))", [&svc](const httplib::Request& req, httplib::Response& res) {
            auto connId = req.matches[1].str();
            int removed = svc.closeSessionById(connId, "", CLOSE_REASON_KICKED,
                                               /*notify=*/true);
            if (removed < 0) {
                res.status = 404;
                res.set_content(R"({"error":"connection not found"})", "application/json");
                return;
            }
            res.set_content("{\"ok\":true,\"controllersRemoved\":" + std::to_string(removed) + "}",
                            "application/json");
        });

    g_httpServer.Get("/api/logs", [](const httplib::Request& req, httplib::Response& res) {
        uint64_t since = 0;
        if (req.has_param("since")) {
            since = strtoull(req.get_param_value("since").c_str(), nullptr, 10);
        }

        std::lock_guard<std::mutex> lk(g_logMtx);

        int count = static_cast<int>(std::min(g_logSeq, static_cast<uint64_t>(LOG_RING_SIZE)));
        uint64_t oldestSeq = g_logSeq - count;

        std::string json = "{\"seq\":" + std::to_string(g_logSeq) + ",\"entries\":[";
        bool first = true;

        for (int i = 0; i < count; i++) {
            uint64_t entrySeq = oldestSeq + i;
            if (entrySeq <= since) continue;

            int idx = (g_logHead - count + i + LOG_RING_SIZE) % LOG_RING_SIZE;
            const auto& e = g_logRing[idx];

            if (!first) json += ",";
            first = false;

            auto epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                e.timestamp.time_since_epoch())
                                .count();
            const char* lvl = (e.level == LogLevel::ERR)    ? "error"
                              : (e.level == LogLevel::WARN) ? "warn"
                                                            : "info";

            json += "{\"seq\":" + std::to_string(entrySeq) + ",\"ts\":" + std::to_string(epoch_ms) +
                    ",\"level\":\"" + lvl + "\",\"source\":\"" + jsonEscape(e.source) +
                    "\",\"message\":\"" + jsonEscape(e.message) + "\"}";
        }
        json += "]}";
        res.set_content(json, "application/json");
    });

    // SSE: one stream multiplexes status/connections/devices/update/pin/
    // pairRequests events.
    g_httpServer.Get("/api/events", [&svc](const httplib::Request&, httplib::Response& res) {
        res.set_header("Cache-Control", "no-cache");
        res.set_header("X-Accel-Buffering", "no");
        res.set_chunked_content_provider("text/event-stream", [&svc](size_t /*offset*/,
                                                                     httplib::DataSink& sink) {
            while (g_appRunning) {
                char senderIP[INET_ADDRSTRLEN] = "none";
                uint32_t ipRaw = g_senderIP.load(std::memory_order_relaxed);
                if (ipRaw != 0) {
                    in_addr ia;
                    ia.s_addr = ipRaw;
                    inet_ntop(AF_INET, &ia, senderIP, sizeof(senderIP));
                }

                std::string connJson = buildConnectionsJson(svc);
                std::string devicesJson = buildDevicesJson(svc);

                uint64_t logSeqNow;
                {
                    std::lock_guard<std::mutex> lk2(g_logMtx);
                    logSeqNow = g_logSeq;
                }

                bool backendUp = svc.isBackendAvailable();
                std::string backendJson = buildBackendJson();
                char statusBuf[1536];
                snprintf(statusBuf, sizeof(statusBuf),
                         R"({"listening":%s,"packets":%llu,"senderIP":"%s","udpPort":%d,)"
                         R"("autoStart":%s,"backendAvailable":%s,"backend":%s,)"
                         R"("submitOk":%llu,"submitFail":%llu,)"
                         R"("lastLoopUs":%llu,"decryptFail":%llu,"replayDrop":%llu,)"
                         R"("logSeq":%llu})",
                         g_listening.load() ? "true" : "false",
                         (unsigned long long)g_packetCount.load(), senderIP, g_config.udpPort,
                         g_config.autoStart ? "true" : "false", backendUp ? "true" : "false",
                         backendJson.c_str(), (unsigned long long)g_submitOk.load(),
                         (unsigned long long)g_submitFail.load(),
                         (unsigned long long)g_lastLoopUs.load(),
                         (unsigned long long)g_decryptFail.load(),
                         (unsigned long long)g_replayDrop.load(), (unsigned long long)logSeqNow);

                std::string event = "event: status\ndata: ";
                event += statusBuf;
                event += "\n\n";

                event += "event: connections\ndata: ";
                event += connJson;
                event += "\n\n";

                // The dashboard renders one device-centric list, so it needs
                // the paired set on every tick, not just the live connections.
                event += "event: devices\ndata: ";
                event += devicesJson;
                event += "\n\n";

                if (g_updateService) {
                    event += "event: update\ndata: ";
                    event += buildUpdateJson(g_updateService->snapshot());
                    event += "\n\n";
                }

                // Pushed each tick so the countdown ticks and a fresh tab sees
                // current state without a parallel /api/pin/status poll.
                event += "event: pin\ndata: ";
                event += buildPinJson();
                event += "\n\n";

                // Pushed each tick so the accept/deny panel appears the instant a dish asks.
                event += "event: pairRequests\ndata: ";
                event += buildPairRequestsJson();
                event += "\n\n";

                if (!sink.write(event.c_str(), event.size())) return false;
                for (int i = 0; i < 10 && g_appRunning; i++) netSleepMs(100);
            }
            return false;
        });
    });

    logMsg(LogLevel::INFO, "web", "Admin web UI on 127.0.0.1:" + std::to_string(g_config.webPort));
    g_httpServer.listen("127.0.0.1", g_config.webPort);
}

// Client API server — pairing + sessions + catalog. HTTPS (self-signed), 0.0.0.0.
void clientApiThread(SessionService& svc) {
    std::string certPath, keyPath;
    if (!ensureServerCert(certPath, keyPath)) {
        logMsg(LogLevel::ERR, "client", "Failed to generate TLS certificate — client API disabled");
        return;
    }

    httplib::SSLServer server(certPath.c_str(), keyPath.c_str());
    if (!server.is_valid()) {
        logMsg(LogLevel::ERR, "client", "TLS server context invalid — client API disabled");
        return;
    }

    // POST /api/pair — PIN-gated (or hmacProof-gated rotation); no device auth
    // for the PIN paths (the device is not paired yet).
    server.Post("/api/pair", [&svc](const httplib::Request& req, httplib::Response& res) {
        pairRoute(svc, req, res);
    });

    // Path-B poll. No device auth (not paired yet); the minted key is handed
    // back exactly once on approval (pollPairRequest clears it).
    server.Get("/api/pair/status", [](const httplib::Request& req, httplib::Response& res) {
        std::string deviceId;
        if (req.has_param("deviceId")) deviceId = req.get_param_value("deviceId");
        if (deviceId.empty()) {
            res.status = 400;
            res.set_content(R"({"ok":false,"error":"missing deviceId"})", "application/json");
            return;
        }
        std::string keyHex;
        PairRequestState st = pollPairRequest(deviceId, keyHex);
        if (st == PairRequestState::Approved) {
            res.set_content(R"({"ok":true,"status":"approved","sharedKey":")" + keyHex + R"("})",
                            "application/json");
            return;
        }
        res.set_content(std::string(R"({"ok":false,"status":")") + pairRequestStateName(st) +
                            R"("})",
                        "application/json");
    });

    // DELETE /api/pair — client self-unpair (closes any live session first).
    server.Delete("/api/pair", [&svc](const httplib::Request& req, httplib::Response& res) {
        selfUnpairRoute(svc, req, res);
    });

    // PUT /api/connections — idempotent session upsert keyed on deviceId.
    server.Put("/api/connections", [&svc](const httplib::Request& req, httplib::Response& res) {
        upsertConnectionRoute(svc, req, res);
    });

    // GET /api/connections/:id — the reconcile endpoint, scoped to OWN session.
    server.Get(R"(/api/connections/(\w+))",
               [&svc](const httplib::Request& req, httplib::Response& res) {
                   ClientAuth auth;
                   if (!clientAuthed(req, res, auth)) return;
                   auto view = svc.getSessionView(req.matches[1].str(), auth.deviceId);
                   if (!view.found) {
                       res.status = 404;
                       res.set_content(R"({"error":"connection not found"})", "application/json");
                       return;
                   }
                   res.set_content(buildSessionViewJson(view), "application/json");
               });

    // DELETE /api/connections/:id — graceful close of OWN session (no notify:
    // the closer already knows).
    server.Delete(
        R"(/api/connections/(\w+))", [&svc](const httplib::Request& req, httplib::Response& res) {
            ClientAuth auth;
            if (!clientAuthed(req, res, auth)) return;
            int removed = svc.closeSessionById(req.matches[1].str(), auth.deviceId,
                                               CLOSE_REASON_REPLACED, /*notify=*/false);
            if (removed < 0) {
                res.status = 404;
                res.set_content(R"({"error":"connection not found"})", "application/json");
                return;
            }
            res.set_content("{\"ok\":true,\"controllersRemoved\":" + std::to_string(removed) + "}",
                            "application/json");
        });

    // PUT /api/connections/:id/controllers/:idx — standalone single-descriptor
    // upsert (the FULL descriptor; ctrlIdx in the path wins).
    server.Put(R"(/api/connections/(\w+)/controllers/(\d+))", [&svc](const httplib::Request& req,
                                                                     httplib::Response& res) {
        ClientAuth auth;
        if (!clientAuthed(req, res, auth)) return;
        if (!protocolVersionOk(req.body, res)) return;
        ControllerDescriptor d;
        if (!parseDescriptorObject(req.body, /*requireIdx=*/false, d)) {
            res.status = 400;
            res.set_content(R"({"error":"descriptor requires type"})", "application/json");
            return;
        }
        long idx = strtol(req.matches[2].str().c_str(), nullptr, 10);
        d.ctrlIdx = idx > 255 ? 255 : static_cast<uint8_t>(idx);
        ControllerApplyResult ar;
        uint16_t epoch = 0;
        if (!svc.applyController(req.matches[1].str(), auth.deviceId, d, ar, epoch)) {
            res.status = 404;
            res.set_content(R"({"error":"connection not found"})", "application/json");
            return;
        }
        res.set_content("{\"epoch\":" + std::to_string(epoch) +
                            ",\"controller\":" + controllerApplyJson(ar) + "}",
                        "application/json");
    });

    // DELETE /api/connections/:id/controllers/:idx — removes the SLOT only;
    // the session lives on (zero-controller sessions are valid).
    server.Delete(R"(/api/connections/(\w+)/controllers/(\d+))", [&svc](const httplib::Request& req,
                                                                        httplib::Response& res) {
        ClientAuth auth;
        if (!clientAuthed(req, res, auth)) return;
        long idx = strtol(req.matches[2].str().c_str(), nullptr, 10);
        uint16_t epoch = 0;
        if (!svc.removeController(req.matches[1].str(), auth.deviceId,
                                  idx > 255 ? 255 : static_cast<uint8_t>(idx), epoch)) {
            res.status = 404;
            res.set_content(R"({"error":"connection not found"})", "application/json");
            return;
        }
        res.set_content("{\"ok\":true,\"epoch\":" + std::to_string(epoch) + "}",
                        "application/json");
    });

    // No auth on the read-only info surface: the client UI renders BEFORE pairing.
    server.Get("/api/server/capabilities", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(buildCapabilitiesJson(), "application/json");
    });
    server.Get("/api/catalog",
               [](const httplib::Request& req, httplib::Response& res) { catalogRoute(req, res); });
    server.Get(
        R"(/api/catalog/images/([\w-]+))",
        [](const httplib::Request& req, httplib::Response& res) { catalogImageRoute(req, res); });

    g_clientServer = &server;
    logMsg(LogLevel::INFO, "client",
           "Client API (HTTPS) on 0.0.0.0:" + std::to_string(DEFAULT_CLIENT_PORT));
    server.listen("0.0.0.0", DEFAULT_CLIENT_PORT);
    g_clientServer = nullptr;
}
