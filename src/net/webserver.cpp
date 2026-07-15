// SPDX-License-Identifier: LGPL-3.0-or-later
#include "webserver.h"
#include "tls.h"
#include "crypto.h"
#include "config.h"
#include "pairing.h"
#include "pairing_keys.h"
#include "pairing_service.h"
#include "session_crypto.h"
#include "core/catalog.h"
#include "core/gamepad_backend.h"
#include "core/json.h"
#include "core/session_service.h"
#include "core/update_service.h"
#include "core/update_types.h"
#include "core/version.h"
#include "core/firewall_status.h"
#include "core/network_info.h"
#include "local_iface.h"
#include "mdns_protocol.h"
#include "origin_guard.h"
#include "status_json.h"

#include <sodium.h>

using satellite::buildDebugJson;
using satellite::buildSseStatusObject;
using satellite::buildStatusJson;
using satellite::Json;
using satellite::jsonBool;
using satellite::jsonDump;
using satellite::jsonInt;
using satellite::jsonObject;
using satellite::JsonOut;
using satellite::jsonParse;
using satellite::jsonStr;
using satellite::jsonTryBool;
using satellite::jsonTryInt;
using satellite::StatusFields;

static Json parseBody(const std::string& body) {
    Json j;
    if (!jsonParse(body, j) || !j.is_object()) return Json::object();
    return j;
}

// Web UI keys all backend-status copy off (id, errorCode).
static JsonOut backendJsonObj(const BackendStatus& s) {
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

static std::string buildBackendJson(const BackendStatus& s) { return jsonDump(backendJsonObj(s)); }

static std::string buildBackendJson() { return buildBackendJson(probeBackend()); }

// Static facts about the backend that shape the catalog, keyed off the
// backend's identity not its live health (the catalog only changes on server
// upgrade; live health is /api/server/capabilities).
static satellite::CatalogBackendTraits catalogBackendTraits(const BackendStatus& s) {
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
    } else if (id == BACKEND_ID_UINPUT) {
        t.ds4MotionSupported = true;
        t.ds4TouchpadSupported = true;
        t.ds4LightbarSupported = true;
        t.mouseControlSupported = true;
        t.rumbleSupported = true;
    } else if (id == BACKEND_ID_MAC_HID) {
        // Virtual DS4 via IOHIDUserDevice: motion, touchpad, and lightbar all
        // ride the DS4 report set (no driver-version gate, hence no requires
        // code), and rumble+lightbar return through set-report. macOS has no
        // host pointer-injection path yet, so mouseControl stays unsupported
        // (matches IGamepadPort::supportsRelativeMouse() on the adapter).
        t.ds4MotionSupported = true;
        t.ds4TouchpadSupported = true;
        t.ds4LightbarSupported = true;
        t.rumbleSupported = true;
    }
    // keyboardControlSupported stays false on every backend: the host has no keystroke
    // injection path yet. The field is published so the client gates on it (and stays
    // unoffered) instead of hardwiring the assumption; flip it when injection lands.
    return t;
}

static satellite::CatalogBackendTraits catalogBackendTraits() {
    return catalogBackendTraits(probeBackend());
}

// GET /api/server/capabilities: CURRENT dynamic state (the static
// what-exists layer is /api/catalog).
static std::string buildCapabilitiesJson() {
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

static std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return "";
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Keys must stay in sync with the web/ JS that consumes them.
static std::string buildUpdateJson(const UpdateStatusSnapshot& s) {
    JsonOut j;
    j["state"] = updateStateName(s.state);
    j["currentVersion"] = s.currentVersion;
    j["platformId"] = s.platformId;
    j["channel"] = s.channel;
    j["autoCheck"] = s.autoCheck;
    j["autoDownload"] = s.autoDownload;
    j["autoInstall"] = s.autoInstall;
    j["lastCheckEpoch"] = s.lastCheckEpoch;
    j["bytesDownloaded"] = s.bytesDownloaded;
    j["totalBytes"] = s.totalBytes;
    j["message"] = s.message;
    j["failedPhase"] = updateStateName(s.failedPhase);
    JsonOut info;
    info["available"] = s.info.available;
    info["version"] = s.info.version;
    info["channel"] = s.info.channel;
    info["assetName"] = s.info.assetName;
    info["assetSize"] = s.info.assetSize;
    info["assetSha256"] = s.info.assetSha256;
    info["htmlUrl"] = s.info.htmlUrl;
    info["publishedAtEpoch"] = s.info.publishedAtEpoch;
    info["installMethod"] = s.info.installMethod == InstallMethod::SelfInstall ? "self" : "manual";
    info["manualInstruction"] = s.info.manualInstruction;
    info["releaseNotes"] = s.info.releaseNotes;
    j["info"] = std::move(info);
    return jsonDump(j);
}

static JsonOut capsJsonObj(uint16_t caps) {
    JsonOut j;
    j["rumble"] = (caps & CAP_RUMBLE) != 0;
    j["motion"] = (caps & CAP_MOTION) != 0;
    j["analogTriggers"] = (caps & CAP_ANALOG_TRIGGERS) != 0;
    j["lightbar"] = (caps & CAP_LIGHTBAR) != 0;
    return j;
}

// `state` fields serialise as lowercase enum names, the canonical wire form.
// `connectedAtEpoch` is steady-clock seconds (boot-relative), not Unix epoch.
static std::string buildConnectionsJson(const SessionService& svc) {
    auto snap = svc.getConnectionsSnapshot();
    JsonOut connections = JsonOut::array();
    for (const auto& cs : snap.connections) {
        JsonOut c;
        c["connectionId"] = cs.connectionId;
        c["deviceId"] = cs.deviceId;
        c["deviceName"] = cs.deviceName;
        c["senderIP"] = cs.clientIP;
        c["connectedAtEpoch"] = cs.connectedAtEpoch;
        c["epoch"] = cs.epoch;
        c["mouseControlGranted"] = cs.mouseControlGranted;
        // Active or NotResponding here; /api/devices covers the Paired (offline) case.
        c["state"] = deviceLinkStateName(cs.linkState);

        JsonOut controllers = JsonOut::array();
        for (const auto& ctrl : cs.controllers) {
            // Only Live/Detached are surfaced today; transient states aren't yet
            // threaded through SessionService. See ControllerState in core/types.h.
            const char* ctrlState = ctrl.active ? controllerStateName(ControllerState::Live)
                                                : controllerStateName(ControllerState::Detached);
            JsonOut o;
            o["controllerIndex"] = ctrl.index;
            o["serialNo"] = ctrl.serial;
            o["pluggedIn"] = ctrl.pluggedIn;
            o["state"] = ctrlState;
            o["controllerType"] = controllerTypeName(ctrl.controllerType);
            o["controllerTypeLabel"] = controllerTypeLabel(ctrl.controllerType);
            o["touchpadMode"] = touchpadModeName(ctrl.touchpadMode);
            if (ctrl.batteryKnown) {
                JsonOut battery;
                if (ctrl.batteryLevel == BATTERY_LEVEL_UNKNOWN) {
                    battery["level"] = nullptr;
                } else {
                    battery["level"] = ctrl.batteryLevel;
                }
                battery["status"] = batteryStatusName(ctrl.batteryStatus);
                o["battery"] = std::move(battery);
            } else {
                o["battery"] = nullptr;
            }
            o["motionCapable"] = ctrl.motionCapable;
            o["motionActive"] = ctrl.motionActive;
            o["motionSink"] = ctrl.motionSink;
            // Backend has an IMU surface for this controller type; UI warns when
            // motionCapable but not this (motion has nowhere to land, e.g. Xbox pad).
            o["motionSinkSupportedForType"] = ctrl.motionSinkSupportedForType;
            // IMU sink was created at plug-in; false flags a kernel-level failure
            // (uinput perms, kernel too old) vs. just "no game subscribed".
            o["motionBackendOk"] = ctrl.motionBackendOk;
            o["touchpadActive"] = ctrl.touchpadActive;
            o["lightbarCapable"] = ctrl.lightbarCapable;
            if (ctrl.lightbarKnown) {
                char rgb[8];
                snprintf(rgb, sizeof(rgb), "#%02x%02x%02x", ctrl.lightbarR, ctrl.lightbarG,
                         ctrl.lightbarB);
                o["lightbar"] = std::string(rgb);
            } else {
                o["lightbar"] = nullptr;
            }
            controllers.push_back(std::move(o));
        }
        c["controllers"] = std::move(controllers);
        c["activeControllerCount"] = cs.activeControllerCount;
        connections.push_back(std::move(c));
    }
    JsonOut j;
    j["connections"] = std::move(connections);
    j["totalControllers"] = snap.totalControllers;
    j["maxControllers"] = snap.maxControllers;
    j["backendAvailable"] = snap.backendAvailable;
    return jsonDump(j);
}

// Paired devices + their live link state (paired | active | notResponding).
// Shared by the admin route and the SSE devices event.
static std::string buildDevicesJson(const SessionService& svc) {
    JsonOut arr = JsonOut::array();
    std::lock_guard<std::mutex> lk(g_configMtx);
    for (const auto& d : g_config.pairedDevices) {
        DeviceLinkState s = svc.linkStateForDevice(d.id);
        JsonOut o;
        o["id"] = d.id;
        o["name"] = d.name;
        o["lastIP"] = d.lastIP;
        o["pairedAt"] = d.pairedAt;
        o["state"] = deviceLinkStateName(s);
        arr.push_back(std::move(o));
    }
    return jsonDump(arr);
}

static std::string buildPinJson() {
    PinSnapshot s = pinSnapshot();
    JsonOut j;
    j["state"] = pinStateName(s.state);
    j["currentPin"] = s.currentPin;
    j["previousPin"] = s.previousPin;
    j["secondsRemaining"] = s.secondsRemaining;
    return jsonDump(j);
}

static std::string buildPairRequestsJson() {
    auto reqs = pendingPairRequests();
    JsonOut arr = JsonOut::array();
    for (const auto& r : reqs) {
        JsonOut o;
        o["deviceId"] = r.deviceId;
        o["deviceName"] = r.deviceName;
        o["clientIP"] = r.clientIP;
        o["pin"] = r.pin;
        o["secondsRemaining"] = r.secondsRemaining;
        arr.push_back(std::move(o));
    }
    return jsonDump(arr);
}

struct ClientAuth {
    std::string deviceId;
    PairedDevice device; // copy-by-value under g_configMtx, never a pointer
    uint8_t pairingKey[CRYPTO_KEY_SIZE];
};

static std::string headerOrBody(const httplib::Request& req, const char* header,
                                const char* bodyKey) {
    auto hdr = req.headers.find(header);
    if (hdr != req.headers.end() && !hdr->second.empty()) return hdr->second;
    if (!req.body.empty()) return jsonStr(parseBody(req.body), bodyKey);
    return "";
}

// Every authenticated client route requires a paired deviceId AND an hmacProof
// of the pairing key, so a diverged key fails HERE with a terminal 401 instead
// of a silently-undecryptable UDP session. The PairedDevice is copied by value
// under g_configMtx so a concurrent unpair can't dangle it.
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
    JsonOut err;
    err["error"] = "unauthorized";
    err["code"] = code;
    res.set_content(jsonDump(err), "application/json");
    return false;
}

static bool protocolVersionOk(const std::string& body, httplib::Response& res) {
    long pv = PROTOCOL_VERSION;
    if (jsonTryInt(parseBody(body), "protocolVersion", pv) && pv != PROTOCOL_VERSION) {
        res.status = 409;
        JsonOut err;
        err["error"] = "protocol version unsupported";
        err["supported"] = PROTOCOL_VERSION;
        res.set_content(jsonDump(err), "application/json");
        return false;
    }
    return true;
}

// `type` is REQUIRED: a descriptor without it would force a server-side default
// type, the default-then-correct bug class this contract removes.
static bool parseDescriptorObject(const Json& obj, bool requireIdx, ControllerDescriptor& d) {
    long idx = 0;
    if (jsonTryInt(obj, "ctrlIdx", idx)) {
        if (idx < 0) return false;
        d.ctrlIdx = idx > 255 ? 255 : static_cast<uint8_t>(idx);
    } else if (requireIdx) {
        return false;
    }
    long type = 0;
    if (!jsonTryInt(obj, "type", type) || type < 0) return false;
    // Out-of-range values pass through; the service reports invalidType per
    // controller rather than failing the whole request.
    d.type = type > 255 ? 255 : static_cast<uint8_t>(type);

    d.caps = 0;
    const Json caps = jsonObject(obj, "caps");
    if (jsonBool(caps, "rumble")) d.caps |= CAP_RUMBLE;
    if (jsonBool(caps, "motion")) d.caps |= CAP_MOTION;
    if (jsonBool(caps, "analogTriggers")) d.caps |= CAP_ANALOG_TRIGGERS;
    if (jsonBool(caps, "lightbar")) d.caps |= CAP_LIGHTBAR;

    const std::string mode = jsonStr(obj, "touchpadMode");
    if (mode == "ds4") {
        d.touchpadMode = TOUCHPAD_MODE_DS4;
    } else if (mode == "mouse") {
        d.touchpadMode = TOUCHPAD_MODE_MOUSE;
    } else {
        d.touchpadMode = TOUCHPAD_MODE_OFF;
    }
    return true;
}

static bool parseControllerDescriptors(const Json& body, std::vector<ControllerDescriptor>& out) {
    auto it = body.find("controllers");
    if (it == body.end() || !it->is_array()) return true; // absent → no descriptors
    for (const auto& obj : *it) {
        if (!obj.is_object()) continue; // ignore non-object array entries, as before
        ControllerDescriptor d;
        if (!parseDescriptorObject(obj, /*requireIdx=*/true, d)) return false;
        out.push_back(d);
    }
    return true;
}

static JsonOut controllerApplyObj(const ControllerApplyResult& r) {
    JsonOut j;
    j["ctrlIdx"] = r.ctrlIdx;
    j["result"] = applyResultName(r.result);
    j["appliedType"] = r.appliedType;
    JsonOut motion;
    motion["sinkSupportedForType"] = r.motionSinkSupportedForType;
    motion["backendOk"] = r.motionBackendOk;
    j["motion"] = std::move(motion);
    return j;
}

static JsonOut mouseControlObj(bool granted, const std::string& denyReason) {
    JsonOut j;
    j["granted"] = granted;
    if (!granted && !denyReason.empty()) j["reason"] = denyReason;
    return j;
}

static std::string buildUpsertResponseJson(const SessionUpsertResult& r) {
    char tokenHex[9];
    snprintf(tokenHex, sizeof(tokenHex), "%08x", r.token);

    JsonOut j;
    j["connectionId"] = r.connectionId;
    j["token"] = std::string(tokenHex);
    j["sessionSalt"] = hexEncode(r.sessionSalt, SESSION_SALT_SIZE);
    j["epoch"] = r.epoch;
    j["maxControllers"] = r.maxControllers;
    j["protocolVersion"] = PROTOCOL_VERSION;
    JsonOut controllers = JsonOut::array();
    for (const auto& c : r.controllers) controllers.push_back(controllerApplyObj(c));
    j["controllers"] = std::move(controllers);
    JsonOut hostFeatures;
    hostFeatures["mouseControl"] = mouseControlObj(r.mouseControlGranted, r.mouseControlDenyReason);
    j["hostFeatures"] = std::move(hostFeatures);
    return jsonDump(j);
}

static std::string buildSessionViewJson(const SessionService::SessionView& v) {
    JsonOut j;
    j["connectionId"] = v.connectionId;
    j["deviceId"] = v.deviceId;
    j["epoch"] = v.epoch;
    j["protocolVersion"] = PROTOCOL_VERSION;
    j["maxControllers"] = MAX_BACKEND_CONTROLLERS;
    JsonOut controllers = JsonOut::array();
    for (const auto& c : v.controllers) {
        JsonOut o;
        o["ctrlIdx"] = c.ctrlIdx;
        o["active"] = true;
        o["appliedType"] = c.appliedType;
        o["caps"] = capsJsonObj(c.caps);
        o["touchpadMode"] = touchpadModeName(c.touchpadMode);
        JsonOut motion;
        motion["sinkSupportedForType"] = c.motionSinkSupportedForType;
        motion["backendOk"] = c.motionBackendOk;
        o["motion"] = std::move(motion);
        controllers.push_back(std::move(o));
    }
    j["controllers"] = std::move(controllers);
    JsonOut hostFeatures;
    hostFeatures["mouseControl"] = mouseControlObj(v.mouseControlGranted, "");
    j["hostFeatures"] = std::move(hostFeatures);
    return jsonDump(j);
}

// PUT /api/connections: the declarative upsert. Connect + full topology = ONE
// call; re-PUT converges; partial success rides in the body, never the status.
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

    Json body = parseBody(req.body);
    std::string deviceName = jsonStr(body, "deviceName");
    if (deviceName.empty()) deviceName = auth.device.name;

    std::vector<ControllerDescriptor> descriptors;
    if (!parseControllerDescriptors(body, descriptors)) {
        logMsg(LogLevel::WARN, "client",
               "PUT /api/connections: malformed controllers array (ctrlIdx and type are "
               "required) from " +
                   auth.deviceId);
        res.status = 400;
        res.set_content(R"({"error":"controllers entries require ctrlIdx and type"})",
                        "application/json");
        return;
    }

    bool mouseRequested = jsonBool(jsonObject(body, "hostFeatures"), "mouseControl");

    auto result = svc.upsertSession(auth.deviceId, deviceName, req.remote_addr, auth.pairingKey,
                                    descriptors, mouseRequested);
    if (!result.ok) {
        res.status = 500;
        JsonOut err;
        err["error"] = result.error;
        res.set_content(jsonDump(err), "application/json");
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

// Dual-path device pairing over HTTPS.
// Path A: `pin` (server-generated, typed into the dish), verifyPin, pair now.
// Path B: `clientPin` (dish-shown), register a request, reply pending=true; the
//   operator accepts on the dashboard/tray and the dish polls /api/pair/status.
// Key rotation: `hmacProof` of the CURRENT key mints and returns a fresh key.
// There is NO PIN-free already-paired short-circuit: handing the stored key to
// anyone who learned a deviceId would let any LAN actor exfiltrate it.
// Always 200 on the PIN paths; the sender classifies on `ok`/`pending`.
static void pairRoute(SessionService& svc, const httplib::Request& req, httplib::Response& res) {
    Json body = parseBody(req.body);
    auto deviceId = jsonStr(body, "deviceId");
    auto deviceName = jsonStr(body, "deviceName");
    auto pin = jsonStr(body, "pin");               // server-shown PIN (Path A)
    auto clientPin = jsonStr(body, "clientPin");   // dish-shown PIN (Path B)
    auto clientPkHex = jsonStr(body, "publicKey"); // client's X25519 public key
    auto hmacProof = jsonStr(body, "hmacProof");   // key-rotation proof
    const std::string clientIP = req.remote_addr;

    if (deviceId.empty()) {
        res.set_content(R"({"ok":false,"error":"missing deviceId"})", "application/json");
        return;
    }
    if (!protocolVersionOk(req.body, res)) return;

    // Key rotation / re-pair with proof of the current key. A failed proof
    // falls through to the PIN paths, identical to a fresh pairing attempt.
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
            // The old key dies with the rotation, so any live session keyed on
            // it must die too.
            svc.closeSessionsForDevice(deviceId, CLOSE_REASON_REPLACED);
            logMsg(LogLevel::INFO, "pairing",
                   "Rotated pairing key for " + deviceId + " (" + clientIP + ")");
            JsonOut ok;
            ok["ok"] = true;
            ok["message"] = "key rotated";
            ok["sharedKey"] = newKeyHex;
            ok["protocolVersion"] = PROTOCOL_VERSION;
            res.set_content(jsonDump(ok), "application/json");
            return;
        }
        logMsg(LogLevel::WARN, "pairing",
               "Rejected proof-based re-pair for " + deviceId + " (" + clientIP + ")");
    }

    // Path A: dish entered the operator's server-generated PIN.
    if (!pin.empty() && verifyPin(pin)) {
        uint8_t serverPk[32], serverSk[32];
        generateKeyPair(serverPk, serverSk);

        std::string sharedKeyHex;
        PairingKeyOutcome outcome =
            resolvePairingSharedKey(clientPkHex, serverPk, serverSk, sharedKeyHex);
        if (outcome == PairingKeyOutcome::InvalidClientKey) {
            sodium_memzero(serverSk, 32);
            logMsg(LogLevel::WARN, "pairing",
                   "Rejected pairing: unusable client public key from " + deviceId + " (" +
                       clientIP + ")");
            res.set_content(R"({"ok":false,"error":"invalid public key"})", "application/json");
            return;
        }

        upsertPairedDevice(deviceId, deviceName, clientIP, sharedKeyHex);
        // A re-pair invalidates the previous key; a session still keyed on it
        // would churn undecryptably, so close it now.
        svc.closeSessionsForDevice(deviceId, CLOSE_REASON_REPLACED);

        std::string serverPkHex = hexEncode(serverPk, 32);
        sodium_memzero(serverSk, 32);
        logMsg(LogLevel::INFO, "pairing",
               "Paired device via server PIN: " + deviceId + " (" + clientIP + ")");
        JsonOut ok;
        ok["ok"] = true;
        ok["message"] = "paired successfully";
        if (outcome == PairingKeyOutcome::Derived) {
            ok["serverPublicKey"] = serverPkHex;
        } else {
            ok["sharedKey"] = sharedKeyHex;
        }
        ok["protocolVersion"] = PROTOCOL_VERSION;
        res.set_content(jsonDump(ok), "application/json");
        return;
    }

    // Path B: register the dish's request; it then polls /api/pair/status. The
    // clientPin is never echoed server-side; the operator must read it off the
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

// DELETE /api/pair: client self-unpair (hmacProof-authed). Closes any live
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

// Catalog routes are unauthenticated: the UI renders BEFORE pairing.
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

// Admin server: web UI + admin API. Plain HTTP, 127.0.0.1, no auth.
void adminHttpThread(SessionService& svc) {
    // Reject cross-origin / rebound requests before any route runs.
    g_httpServer.set_pre_routing_handler([](const httplib::Request& req, httplib::Response& res) {
        auto host = req.get_header_value("Host");
        if (!host.empty() && !satellite::isLoopbackHost(host)) {
            res.status = 403;
            res.set_content(R"({"error":"forbidden host"})", "application/json");
            return httplib::Server::HandlerResponse::Handled;
        }
        if (req.method != "GET" && req.method != "HEAD" && req.method != "OPTIONS") {
            auto origin = req.get_header_value("Origin");
            if (!origin.empty() && !satellite::isLoopbackOrigin(origin)) {
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
        StatusFields f;
        f.backend = backendJsonObj(probeBackend());
        f.backendAvailable = svc.isBackendAvailable();
        {
            std::lock_guard<std::mutex> lk(g_configMtx);
            f.udpPort = g_config.udpPort;
            f.webPort = g_config.webPort;
            f.autoStart = g_config.autoStart;
            f.discoveryBroadcastEnabled = g_config.discoveryBroadcastEnabled;
        }
        f.listening = g_listening.load();
        f.packets = static_cast<uint64_t>(g_packetCount.load());
        f.senderIP = senderIP;
        f.mdnsResponderActive = g_mdnsResponderActive.load();
        res.set_content(buildStatusJson(f), "application/json");
    });

    g_httpServer.Get("/api/netinfo", [](const httplib::Request&, httplib::Response& res) {
        NetworkInfo info;
        std::string selected;
        {
            std::lock_guard<std::mutex> lk(g_configMtx);
            info.udpPort = g_config.udpPort;
            info.webPort = g_config.webPort;
            info.pairPort = g_config.pairPort;
            info.discPort = g_config.discPort;
            selected = g_config.networkInterface;
            info.allowPublic = g_config.allowPublicNetwork;
        }
        info.clientPort = DEFAULT_CLIENT_PORT;
        info.mdnsPort = mdns::MULTICAST_PORT;
        info.selected = selected;
        info.interfaces = enumerateInterfaces(true);
        int idx = chooseInterface(info.interfaces, selected);
        if (idx >= 0) {
            const LocalInterface& bound = info.interfaces[static_cast<size_t>(idx)];
            info.lanIp = bound.ipv4;
            info.device = bound.name;
            info.category = bound.category;
        }
        int ruleMask = 0;
        bool haveRule = false;
        if (selfInboundFirewallRules(ruleMask, haveRule)) {
            info.firewallSupported = true;
            info.firewallState = fw::firewallStateString(
                fw::evaluateFirewall(fw::profileBit(info.category), ruleMask, haveRule));
        }
        res.set_content(buildNetworkInfoJson(info), "application/json");
    });

    g_httpServer.Post("/api/network/allow-public",
                      [](const httplib::Request&, httplib::Response& res) {
                          bool ok = allowPublicFirewall();
                          if (ok) {
                              std::lock_guard<std::mutex> lk(g_configMtx);
                              g_config.allowPublicNetwork = true;
                              saveConfig(g_config);
                          }
                          JsonOut resp;
                          resp["ok"] = ok;
                          res.set_content(jsonDump(resp), "application/json");
                      });

    g_httpServer.Post("/api/config", [](const httplib::Request& req, httplib::Response& res) {
        Json body = parseBody(req.body);
        bool portRejected = false;
        std::lock_guard<std::mutex> lk(g_configMtx);

        // Out-of-range ports are rejected, not clamped; the response echoes the effective port.
        long port = 0;
        if (jsonTryInt(body, "udpPort", port)) {
            if (port >= 1024 && port <= 65535) {
                g_config.udpPort = static_cast<int>(port);
            } else {
                portRejected = true;
            }
        }

        // Applied only when present, so a partial POST leaves the stored value untouched.
        bool autoStartVal = false;
        if (jsonTryBool(body, "autoStart", autoStartVal)) {
            g_config.autoStart = autoStartVal;
            setAutoStart(g_config.autoStart);
        }

        // Applied only when present so a partial POST can't silently flip discovery off.
        bool broadcastVal = false;
        if (jsonTryBool(body, "discoveryBroadcastEnabled", broadcastVal)) {
            g_config.discoveryBroadcastEnabled = broadcastVal;
        }

        if (body.contains("networkInterface")) {
            g_config.networkInterface = jsonStr(body, "networkInterface");
        }

        saveConfig(g_config);
        logMsg(LogLevel::INFO, "web",
               "Config updated: udpPort=" + std::to_string(g_config.udpPort) + " autoStart=" +
                   std::string(g_config.autoStart ? "true" : "false") + " broadcast=" +
                   std::string(g_config.discoveryBroadcastEnabled ? "true" : "false") +
                   (portRejected ? " (udpPort out of range, ignored)" : ""));
        JsonOut resp;
        resp["ok"] = true;
        resp["udpPort"] = g_config.udpPort;
        resp["udpPortRejected"] = portRejected;
        res.set_content(jsonDump(resp), "application/json");
    });

    g_httpServer.Get("/api/version", [](const httplib::Request&, httplib::Response& res) {
        JsonOut j;
        j["version"] = SATELLITE_VERSION;
        j["platformId"] = g_updateService ? g_updateService->snapshot().platformId : "unknown";
        res.set_content(jsonDump(j), "application/json");
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
        std::string v = jsonStr(parseBody(req.body), "version");
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
        Json body = parseBody(req.body);
        std::string channel = jsonStr(body, "channel");
        if (channel.empty()) channel = UPDATE_CHANNEL_STABLE;
        bool autoCheck = jsonBool(body, "autoCheck");
        bool autoDownload = jsonBool(body, "autoDownload");
        bool autoInstall = jsonBool(body, "autoInstall");
        g_updateService->updatePreferences(channel, autoCheck, autoDownload, autoInstall);
        logMsg(LogLevel::INFO, "web",
               "Update prefs: channel=" + channel + " autoCheck=" + (autoCheck ? "true" : "false") +
                   " autoDownload=" + (autoDownload ? "true" : "false") +
                   " autoInstall=" + (autoInstall ? "true" : "false"));
        res.set_content(R"({"ok":true})", "application/json");
    });

    // PINs are echoed here for the dashboard; safe because this is the
    // loopback-only admin surface.
    g_httpServer.Get("/api/pin/status", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(buildPinJson(), "application/json");
    });

    // Reverse-direction pairing (dish shows a PIN, operator accepts here).
    // Localhost admin surface (operator is at the satellite), so no device auth.
    g_httpServer.Get("/api/pair/requests", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(buildPairRequestsJson(), "application/json");
    });

    g_httpServer.Post("/api/pair/respond", [](const httplib::Request& req, httplib::Response& res) {
        Json body = parseBody(req.body);
        auto deviceId = jsonStr(body, "deviceId");
        bool accept = jsonBool(body, "accept");
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

    // Admin unpair. Closes any live session first: an unpaired device must not
    // keep streaming on a key the server no longer trusts.
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
            JsonOut ok;
            ok["ok"] = true;
            ok["sessionsClosed"] = closed;
            res.set_content(jsonDump(ok), "application/json");
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
        StatusFields f;
        f.backend = backendJsonObj(probeBackend());
        f.backendAvailable = svc.isBackendAvailable();
        {
            std::lock_guard<std::mutex> lk(g_configMtx);
            f.udpPort = g_config.udpPort;
        }
        f.listening = g_listening.load();
        f.packets = static_cast<uint64_t>(g_packetCount.load());
        f.submitOk = static_cast<uint64_t>(g_submitOk.load());
        f.submitFail = static_cast<uint64_t>(g_submitFail.load());
        f.lastLoopUs = static_cast<uint64_t>(g_lastLoopUs.load());
        f.maxLoopUs = maxUs;
        f.senderIP = senderIP;
        f.decryptFail = static_cast<uint64_t>(g_decryptFail.load());
        f.replayDrop = static_cast<uint64_t>(g_replayDrop.load());
        res.set_content(buildDebugJson(f), "application/json");
    });

    g_httpServer.Get("/api/connections", [&svc](const httplib::Request&, httplib::Response& res) {
        res.set_content(buildConnectionsJson(svc), "application/json");
    });

    // Admin kick is transient by design (a retrying client may re-PUT and
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
            JsonOut ok;
            ok["ok"] = true;
            ok["controllersRemoved"] = removed;
            res.set_content(jsonDump(ok), "application/json");
        });

    g_httpServer.Get("/api/logs", [](const httplib::Request& req, httplib::Response& res) {
        uint64_t since = 0;
        if (req.has_param("since")) {
            since = strtoull(req.get_param_value("since").c_str(), nullptr, 10);
        }

        std::lock_guard<std::mutex> lk(g_logMtx);

        int count = static_cast<int>(std::min(g_logSeq, static_cast<uint64_t>(LOG_RING_SIZE)));
        uint64_t oldestSeq = g_logSeq - count;

        JsonOut entries = JsonOut::array();
        for (int i = 0; i < count; i++) {
            uint64_t entrySeq = oldestSeq + i;
            if (entrySeq <= since) continue;

            int idx = (g_logHead - count + i + LOG_RING_SIZE) % LOG_RING_SIZE;
            const auto& e = g_logRing[idx];

            auto epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                e.timestamp.time_since_epoch())
                                .count();
            const char* lvl = (e.level == LogLevel::ERR)    ? "error"
                              : (e.level == LogLevel::WARN) ? "warn"
                                                            : "info";

            JsonOut o;
            o["seq"] = entrySeq;
            o["ts"] = static_cast<int64_t>(epoch_ms);
            o["level"] = lvl;
            o["source"] = e.source;
            o["message"] = e.message;
            entries.push_back(std::move(o));
        }
        JsonOut j;
        j["seq"] = g_logSeq;
        j["entries"] = std::move(entries);
        res.set_content(jsonDump(j), "application/json");
    });

    // SSE: one stream multiplexes status/connections/devices/update/pin/
    // pairRequests events.
    g_httpServer.Get("/api/events", [&svc](const httplib::Request&, httplib::Response& res) {
        res.set_header("Cache-Control", "no-cache");
        res.set_header("X-Accel-Buffering", "no");
        res.set_chunked_content_provider(
            "text/event-stream", [&svc](size_t /*offset*/, httplib::DataSink& sink) {
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

                    StatusFields f;
                    f.backendAvailable = svc.isBackendAvailable();
                    f.backend = backendJsonObj(probeBackend());
                    {
                        std::lock_guard<std::mutex> lk(g_configMtx);
                        f.udpPort = g_config.udpPort;
                        f.autoStart = g_config.autoStart;
                    }
                    f.listening = g_listening.load();
                    f.packets = static_cast<uint64_t>(g_packetCount.load());
                    f.senderIP = senderIP;
                    f.submitOk = static_cast<uint64_t>(g_submitOk.load());
                    f.submitFail = static_cast<uint64_t>(g_submitFail.load());
                    f.lastLoopUs = static_cast<uint64_t>(g_lastLoopUs.load());
                    f.decryptFail = static_cast<uint64_t>(g_decryptFail.load());
                    f.replayDrop = static_cast<uint64_t>(g_replayDrop.load());
                    f.logSeq = logSeqNow;

                    std::string event = "event: status\ndata: ";
                    event += jsonDump(buildSseStatusObject(f));
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

    int webPort;
    {
        std::lock_guard<std::mutex> lk(g_configMtx);
        webPort = g_config.webPort;
    }
    logMsg(LogLevel::INFO, "web", "Admin web UI on 127.0.0.1:" + std::to_string(webPort));
    g_httpServer.listen("127.0.0.1", webPort);
}

// Client API server: pairing + sessions + catalog. HTTPS (self-signed), 0.0.0.0.
void clientApiThread(SessionService& svc) {
    std::string certPath, keyPath;
    if (!ensureServerCert(certPath, keyPath)) {
        logMsg(LogLevel::ERR, "client", "Failed to generate TLS certificate; client API disabled");
        return;
    }

    httplib::SSLServer server(certPath.c_str(), keyPath.c_str());
    if (!server.is_valid()) {
        logMsg(LogLevel::ERR, "client", "TLS server context invalid; client API disabled");
        return;
    }

    // POST /api/pair: PIN-gated (or hmacProof-gated rotation); no device auth
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
            JsonOut ok;
            ok["ok"] = true;
            ok["status"] = "approved";
            ok["sharedKey"] = keyHex;
            res.set_content(jsonDump(ok), "application/json");
            return;
        }
        JsonOut r;
        r["ok"] = false;
        r["status"] = pairRequestStateName(st);
        res.set_content(jsonDump(r), "application/json");
    });

    // DELETE /api/pair: client self-unpair (closes any live session first).
    server.Delete("/api/pair", [&svc](const httplib::Request& req, httplib::Response& res) {
        selfUnpairRoute(svc, req, res);
    });

    // PUT /api/connections: idempotent session upsert keyed on deviceId.
    server.Put("/api/connections", [&svc](const httplib::Request& req, httplib::Response& res) {
        upsertConnectionRoute(svc, req, res);
    });

    // GET /api/connections/:id: the reconcile endpoint, scoped to OWN session.
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

    // DELETE /api/connections/:id: graceful close of OWN session (no notify:
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
            JsonOut ok;
            ok["ok"] = true;
            ok["controllersRemoved"] = removed;
            res.set_content(jsonDump(ok), "application/json");
        });

    // PUT /api/connections/:id/controllers/:idx: standalone single-descriptor
    // upsert (the FULL descriptor; ctrlIdx in the path wins).
    server.Put(R"(/api/connections/(\w+)/controllers/(\d+))", [&svc](const httplib::Request& req,
                                                                     httplib::Response& res) {
        ClientAuth auth;
        if (!clientAuthed(req, res, auth)) return;
        if (!protocolVersionOk(req.body, res)) return;
        ControllerDescriptor d;
        if (!parseDescriptorObject(parseBody(req.body), /*requireIdx=*/false, d)) {
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
        JsonOut j;
        j["epoch"] = epoch;
        j["controller"] = controllerApplyObj(ar);
        res.set_content(jsonDump(j), "application/json");
    });

    // DELETE /api/connections/:id/controllers/:idx: removes the SLOT only;
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
        JsonOut ok;
        ok["ok"] = true;
        ok["epoch"] = epoch;
        res.set_content(jsonDump(ok), "application/json");
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
