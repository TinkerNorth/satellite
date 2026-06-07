// SPDX-License-Identifier: LGPL-3.0-or-later
#include "webserver.h"
#include "tls.h"
#include "crypto.h"
#include "config.h"
#include "pairing.h"
#include "pairing_service.h"
#include "core/gamepad_backend.h"
#include "core/session_service.h"
#include "core/update_service.h"
#include "core/update_types.h"
#include "core/version.h"

#include <sodium.h>

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

// Drives the client touchpad mode-picker; `off` is always offered as the inert fallback.
static std::string buildCapabilitiesJson() {
    TouchpadCapabilities caps = probeTouchpadCapabilities();
    std::string json = "{\"touchpad\":{\"supportedModes\":[";
    bool first = true;
    auto emit = [&](const char* name) {
        if (!first) json += ",";
        json += "\"";
        json += name;
        json += "\"";
        first = false;
    };
    if (caps.offSupported) emit("off");
    if (caps.padSupported) emit("ds4");
    if (caps.mouseSupported) emit("mouse");
    json += "],\"defaultMode\":\"off\"},\"backend\":";
    json += buildBackendJson();
    json += "}";
    return json;
}

// Shared by the admin HTTP server and the authoritative HTTPS client API:
// validate the mode, persist to the paired device, hot-apply to live connections.
static void handleTouchpadModeSet(SessionService& svc, const httplib::Request& req,
                                  httplib::Response& res) {
    auto deviceId = jsonGetString(req.body, "id");
    auto modeStr = jsonGetString(req.body, "mode");
    if (deviceId.empty() || modeStr.empty()) {
        logMsg(LogLevel::WARN, "web",
               "POST /api/devices/touchpad-mode: missing id or mode in body");
        res.status = 400;
        res.set_content(R"({"error":"missing id or mode"})", "application/json");
        return;
    }
    if (modeStr != "ds4" && modeStr != "mouse" && modeStr != "off") {
        logMsg(LogLevel::WARN, "web",
               "POST /api/devices/touchpad-mode: bad mode '" + modeStr +
                   "' (expected ds4|mouse|off) for device " + deviceId);
        res.status = 400;
        res.set_content(R"({"error":"mode must be ds4, mouse, or off"})", "application/json");
        return;
    }
    // `ds4`/`mouse` ride on the virtual-gamepad backend (absent on macOS); `off` always works.
    TouchpadCapabilities caps = probeTouchpadCapabilities();
    bool modeSupported = (modeStr == "off" && caps.offSupported) ||
                         (modeStr == "ds4" && caps.padSupported) ||
                         (modeStr == "mouse" && caps.mouseSupported);
    if (!modeSupported) {
        logMsg(LogLevel::WARN, "web",
               "POST /api/devices/touchpad-mode: '" + modeStr +
                   "' not supported by this host's backend (device " + deviceId + ")");
        res.status = 409; // Conflict — server cannot honour this mode
        res.set_content(R"({"error":"mode not supported on this host","supported":false})",
                        "application/json");
        return;
    }
    uint8_t mode = touchpadModeFromName(modeStr);
    bool found = false;
    {
        std::lock_guard<std::mutex> lk(g_configMtx);
        for (auto& d : g_config.pairedDevices) {
            if (d.id == deviceId) {
                d.touchpadMode = mode;
                found = true;
                break;
            }
        }
        if (found) saveConfig(g_config);
    }
    if (!found) {
        // Usually a stale pairing on the dish (config wiped / reinstalled);
        // the dish surfaces the 404 as a re-pair prompt.
        logMsg(LogLevel::WARN, "web",
               "POST /api/devices/touchpad-mode: device " + deviceId +
                   " is not in pairedDevices — dish needs to re-pair");
        res.status = 404;
        res.set_content(R"({"error":"device not paired"})", "application/json");
        return;
    }
    bool hotApplied = svc.setTouchpadMode(deviceId, mode);
    logMsg(LogLevel::INFO, "web",
           "Touchpad mode for device " + deviceId + " set to " + modeStr +
               (hotApplied ? " (applied to live connection)" : ""));
    res.set_content(std::string("{\"ok\":true,\"hotApplied\":") + (hotApplied ? "true" : "false") +
                        "}",
                    "application/json");
}

// Key-scoped bool/int reads for request bodies: locate the *quoted* key and
// inspect only the token after its colon, so a value can't false-positive on a
// substring elsewhere in the body. Return false (out untouched) if absent/malformed.
static bool jsonValueStart(const std::string& json, const std::string& key, size_t& out) {
    std::string needle = "\"" + key + "\"";
    size_t pos = 0;
    for (;;) {
        pos = json.find(needle, pos);
        if (pos == std::string::npos) return false;
        size_t colon = json.find_first_not_of(" \t\r\n", pos + needle.size());
        if (colon == std::string::npos || json[colon] != ':') {
            pos += needle.size();
            continue; // "key" not followed by ':' — keep searching
        }
        out = colon + 1;
        return true;
    }
}

static bool jsonGetBoolKeyed(const std::string& json, const std::string& key, bool* out) {
    size_t vs;
    if (!jsonValueStart(json, key, vs)) return false;
    size_t t = json.find_first_not_of(" \t\r\n", vs);
    if (t == std::string::npos) return false;
    if (json.compare(t, 4, "true") == 0) {
        *out = true;
        return true;
    }
    if (json.compare(t, 5, "false") == 0) {
        *out = false;
        return true;
    }
    return false; // not a boolean literal — treat as absent
}

static bool jsonGetIntKeyed(const std::string& json, const std::string& key, long* out) {
    size_t vs;
    if (!jsonValueStart(json, key, vs)) return false;
    size_t t = json.find_first_not_of(" \t\r\n", vs);
    if (t == std::string::npos) return false;
    // Require a numeric token so non-numbers aren't silently coerced to 0 by atoi.
    if (json[t] != '-' && (json[t] < '0' || json[t] > '9')) return false;
    char* end = nullptr;
    long v = strtol(json.c_str() + t, &end, 10);
    if (end == json.c_str() + t) return false; // no digits consumed
    *out = v; // strtol overflow is harmless — the caller range-checks
    return true;
}

// LAN-reachable client API: connection routes require a paired deviceId (from
// X-Device-Id, falling back to the body). The admin API needs none — it's loopback.
static bool clientAuthorized(const httplib::Request& req, httplib::Response& res) {
    std::string deviceId;
    auto hdr = req.headers.find("X-Device-Id");
    if (hdr != req.headers.end()) { deviceId = hdr->second; }
    if (deviceId.empty() && !req.body.empty()) { deviceId = jsonGetString(req.body, "deviceId"); }
    if (!deviceId.empty()) {
        std::lock_guard<std::mutex> lk(g_configMtx);
        for (const auto& d : g_config.pairedDevices) {
            if (d.id == deviceId) return true;
        }
    }
    // Log the 401 so the failure is visible server-side: empty id means a dish
    // plumbing bug; an unknown id means a stale pairing needing re-pair.
    logMsg(LogLevel::WARN, "client",
           "401 unauthorized " + req.method + " " + req.path +
               (deviceId.empty() ? " (no X-Device-Id header and no deviceId in body)"
                                 : " (deviceId " + deviceId + " not in pairedDevices)"));
    res.status = 401;
    res.set_content(R"({"error":"unauthorized"})", "application/json");
    return false;
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

// `state` fields serialise as lowercase enum names (deviceLinkStateName /
// controllerStateName in core/types.h) — the canonical wire form.
static std::string buildConnectionsJson(const SessionService& svc) {
    auto snap = svc.getConnectionsSnapshot();
    std::string json = "{\"connections\":[";
    bool first = true;
    for (const auto& cs : snap.connections) {
        if (!first) json += ",";
        first = false;

        char tokenHex[9];
        snprintf(tokenHex, sizeof(tokenHex), "%08x", cs.token);

        json += "{\"connectionId\":\"conn_";
        json += tokenHex;
        json += "\",\"deviceId\":\"" + jsonEscape(cs.deviceId) + "\",\"deviceName\":\"" +
                jsonEscape(cs.deviceName) + "\",\"senderIP\":\"" + jsonEscape(cs.clientIP) + "\"";

        json += ",\"connectedAtEpoch\":" + std::to_string(cs.connectedAtEpoch);
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
                    ",\"pluggedIn\":" + (ctrl.serial > 0 ? "true" : "false") + ",\"state\":\"" +
                    std::string(ctrlState) + "\"" + ",\"controllerType\":\"" +
                    controllerTypeName(ctrl.controllerType) + "\",\"controllerTypeLabel\":\"" +
                    controllerTypeLabel(ctrl.controllerType) + "\"";
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
            json += ",\"motionCapable\":" + std::string(ctrl.motionCapable ? "true" : "false");
            json += ",\"motionActive\":" + std::string(ctrl.motionActive ? "true" : "false");
            json += ",\"motionSink\":" + std::string(ctrl.motionSink ? "true" : "false");
            // Backend has an IMU surface for this controller type; UI warns when
            // motionCapable but not this (motion has nowhere to land, e.g. Xbox pad).
            json += ",\"motionSinkSupportedForType\":" +
                    std::string(ctrl.motionSinkSupportedForType ? "true" : "false");
            // IMU sink was created at plug-in; false flags a kernel-level failure
            // (uinput perms, kernel too old) vs. just "no game subscribed".
            json += ",\"motionBackendOk\":" + std::string(ctrl.motionBackendOk ? "true" : "false");
            json += ",\"touchpadActive\":" + std::string(ctrl.touchpadActive ? "true" : "false");
            json += ",\"lightbarCapable\":" + std::string(ctrl.lightbarCapable ? "true" : "false");
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
        json += "],\"activeControllerCount\":" + std::to_string(cs.activeControllerCount) +
                ",\"touchpadMode\":\"" + touchpadModeName(cs.touchpadMode) + "\"}";
    }
    json += "],\"totalControllers\":" + std::to_string(snap.totalControllers) +
            ",\"maxControllers\":" + std::to_string(snap.maxControllers) +
            ",\"backendAvailable\":" + (snap.backendAvailable ? "true" : "false") + "}";
    return json;
}

// Shared by admin (dashboard disconnect) and client (self-teardown).
static void closeConnectionRoute(SessionService& svc, const httplib::Request& req,
                                 httplib::Response& res) {
    auto connId = req.matches[1].str();

    // connId format: conn_XXXXXXXX (hex token).
    std::string tokenStr = connId;
    if (tokenStr.substr(0, 5) == "conn_") tokenStr = tokenStr.substr(5);

    uint32_t token = 0;
#ifdef _MSC_VER
    if (sscanf_s(tokenStr.c_str(), "%08x", &token) != 1 || token == 0) {
#else
    if (sscanf(tokenStr.c_str(), "%08x", &token) != 1 || token == 0) {
#endif
        res.status = 404;
        res.set_content(R"({"error":"connection not found"})", "application/json");
        return;
    }

    int removed = svc.closeSession(token);
    if (removed < 0) {
        res.status = 404;
        res.set_content(R"({"error":"connection not found"})", "application/json");
        return;
    }

    res.set_content("{\"ok\":true,\"controllersRemoved\":" + std::to_string(removed) + "}",
                    "application/json");
}

static void openConnectionRoute(SessionService& svc, const httplib::Request& req,
                                httplib::Response& res) {
    auto deviceId = jsonGetString(req.body, "deviceId");
    if (deviceId.empty()) {
        logMsg(LogLevel::WARN, "client", "POST /api/connections: missing deviceId");
        res.status = 400;
        res.set_content(R"({"error":"missing deviceId"})", "application/json");
        return;
    }

    PairedDevice* found = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_configMtx);
        for (auto& d : g_config.pairedDevices) {
            if (d.id == deviceId) {
                found = &d;
                break;
            }
        }
    }
    if (!found) {
        logMsg(LogLevel::WARN, "client",
               "POST /api/connections: device not paired (id=" + deviceId + ")");
        res.status = 403;
        res.set_content(R"({"error":"device not paired"})", "application/json");
        return;
    }

    uint8_t sharedKey[CRYPTO_KEY_SIZE];
    if (!hexDecode(found->sharedKeyHex, sharedKey, CRYPTO_KEY_SIZE)) {
        logMsg(LogLevel::ERR, "client",
               "POST /api/connections: invalid shared key for " + found->name);
        res.status = 500;
        res.set_content(R"({"error":"invalid shared key"})", "application/json");
        return;
    }

    // SessionService handles stale cleanup, token gen, and slot counting.
    auto result =
        svc.openSession(found->id, found->name, found->lastIP, sharedKey, found->touchpadMode);
    if (!result.ok) {
        res.status = 500;
        res.set_content("{\"error\":\"" + jsonEscape(result.error) + "\"}", "application/json");
        return;
    }

    char tokenHex[9];
    snprintf(tokenHex, sizeof(tokenHex), "%08x", result.token);

    std::string response = "{\"connectionId\":\"conn_";
    response += tokenHex;
    response += "\",\"token\":\"";
    response += tokenHex;
    response += "\",\"maxControllers\":" + std::to_string(result.availableSlots);
    response += "}";

    res.status = 201;
    res.set_content(response, "application/json");
}

// upsertPairedDevice + accept/decline live in pairing_service so the dashboard
// route and the native tray prompts share one accept path.

// Dual-path device pairing over HTTPS (PINs + shared key encrypted in transit).
// Path A: `pin` (server-generated, typed into the dish) → verifyPin, pair now.
// Path B: `clientPin` (dish-shown) → register a request, reply pending=true; the
//   operator accepts on the dashboard and the dish polls /api/pair/status.
// See docs/protocol.md. Always 200; the sender classifies on `ok`/`pending`.
static void pairRoute(const httplib::Request& req, httplib::Response& res) {
    auto deviceId = jsonGetString(req.body, "deviceId");
    auto deviceName = jsonGetString(req.body, "deviceName");
    auto pin = jsonGetString(req.body, "pin");               // server-shown PIN (Path A)
    auto clientPin = jsonGetString(req.body, "clientPin");   // dish-shown PIN (Path B)
    auto clientPkHex = jsonGetString(req.body, "publicKey"); // client's X25519 public key
    auto initialMode = jsonGetString(req.body, "touchpadMode");
    const std::string clientIP = req.remote_addr;

    if (deviceId.empty()) {
        res.set_content(R"({"ok":false,"error":"missing deviceId"})", "application/json");
        return;
    }

    // Already paired — hand back the stored key without a PIN. Also the graceful
    // tail of Path B: a dish that missed its status-poll re-pairs into success.
    {
        std::lock_guard<std::mutex> lk(g_configMtx);
        for (auto& d : g_config.pairedDevices) {
            if (d.id == deviceId) {
                d.lastIP = clientIP;
                std::string storedKey = d.sharedKeyHex;
                saveConfig(g_config);
                logMsg(LogLevel::INFO, "pairing",
                       "Device " + deviceName + " (" + clientIP + ") already paired, updating IP");
                res.set_content(R"({"ok":true,"message":"already paired","sharedKey":")" +
                                    storedKey + R"("})",
                                "application/json");
                return;
            }
        }
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

        upsertPairedDevice(deviceId, deviceName, clientIP, sharedKeyHex, initialMode);

        std::string serverPkHex = hexEncode(serverPk, 32);
        sodium_memzero(serverSk, 32);
        logMsg(LogLevel::INFO, "pairing",
               "Paired device via server PIN: " + deviceId + " (" + clientIP + ")");
        if (hasClientKey) {
            res.set_content(R"({"ok":true,"message":"paired successfully","serverPublicKey":")" +
                                serverPkHex + R"("})",
                            "application/json");
        } else {
            res.set_content(R"({"ok":true,"message":"paired successfully","sharedKey":")" +
                                sharedKeyHex + R"("})",
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

    g_httpServer.Post("/api/pin/generate", [](const httplib::Request&, httplib::Response& res) {
        auto pin = generatePin();
        res.set_content("{\"pin\":\"" + pin + "\"}", "application/json");
    });

    // Never echoes the PIN — it's returned only to the generate POST, so a
    // /dashboard refresh can't leak it to a parallel admin tab.
    g_httpServer.Get("/api/pin/status", [](const httplib::Request&, httplib::Response& res) {
        PinSnapshot s = pinSnapshot();
        std::string json = "{\"state\":\"";
        json += pinStateName(s.state);
        json += "\",\"secondsRemaining\":";
        json += std::to_string(s.secondsRemaining);
        json += "}";
        res.set_content(json, "application/json");
    });

    // Reverse-direction pairing (dish shows a PIN, operator accepts here).
    // Localhost admin surface — operator is at the satellite — so no device auth.
    g_httpServer.Get("/api/pair/requests", [](const httplib::Request&, httplib::Response& res) {
        // No PIN in the payload: accepting requires reading it off the dish.
        auto reqs = pendingPairRequests();
        std::string json = "[";
        for (size_t i = 0; i < reqs.size(); i++) {
            const auto& r = reqs[i];
            if (i) json += ",";
            json += "{\"deviceId\":\"" + jsonEscape(r.deviceId) + "\",\"deviceName\":\"" +
                    jsonEscape(r.deviceName) + "\",\"clientIP\":\"" + jsonEscape(r.clientIP) +
                    "\",\"secondsRemaining\":" + std::to_string(r.secondsRemaining) + "}";
        }
        json += "]";
        res.set_content(json, "application/json");
    });

    g_httpServer.Post("/api/pair/respond", [](const httplib::Request& req, httplib::Response& res) {
        auto deviceId = jsonGetString(req.body, "deviceId");
        auto pin = jsonGetString(req.body, "pin");
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
        if (!acceptPairingWithPin(deviceId, pin)) {
            logMsg(LogLevel::WARN, "pairing",
                   "Operator accept for " + deviceId + " rejected (PIN mismatch or no request)");
            res.set_content(R"({"ok":false,"error":"pin mismatch or no pending request"})",
                            "application/json");
            return;
        }
        logMsg(LogLevel::INFO, "pairing", "Operator accepted pairing for " + deviceId);
        res.set_content(R"({"ok":true,"accepted":true})", "application/json");
    });

    g_httpServer.Get("/api/devices", [&svc](const httplib::Request&, httplib::Response& res) {
        // `state` is the DeviceLinkState: paired | active | notResponding.
        std::string json = "[";
        std::lock_guard<std::mutex> lk(g_configMtx);
        for (size_t i = 0; i < g_config.pairedDevices.size(); i++) {
            const auto& d = g_config.pairedDevices[i];
            DeviceLinkState s = svc.linkStateForDevice(d.id);
            json += "{\"id\":\"" + jsonEscape(d.id) + "\",\"name\":\"" + jsonEscape(d.name) +
                    "\",\"lastIP\":\"" + jsonEscape(d.lastIP) + "\",\"pairedAt\":\"" +
                    jsonEscape(d.pairedAt) + "\",\"touchpadMode\":\"" +
                    touchpadModeName(d.touchpadMode) + "\",\"state\":\"" + deviceLinkStateName(s) +
                    "\"}";
            if (i + 1 < g_config.pairedDevices.size()) json += ",";
        }
        json += "]";
        res.set_content(json, "application/json");
    });

    g_httpServer.Post(
        "/api/devices/remove", [](const httplib::Request& req, httplib::Response& res) {
            auto deviceId = jsonGetString(req.body, "id");
            std::lock_guard<std::mutex> lk(g_configMtx);
            auto& devs = g_config.pairedDevices;
            devs.erase(std::remove_if(devs.begin(), devs.end(),
                                      [&](const PairedDevice& d) { return d.id == deviceId; }),
                       devs.end());
            saveConfig(g_config);
            res.set_content(R"({"ok":true})", "application/json");
        });

    // Localhost variant for admin tooling/scripts; the dashboard is read-only and
    // the client owns the setting via the HTTPS variant.
    g_httpServer.Post("/api/devices/touchpad-mode",
                      [&svc](const httplib::Request& req, httplib::Response& res) {
                          handleTouchpadModeSet(svc, req, res);
                      });

    // Advertises which TOUCHPAD_MODE_* this host can honour so the client
    // mode-picker greys out the rest (e.g. macOS → only `off`).
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

    g_httpServer.Delete(R"(/api/connections/(\w+))",
                        [&svc](const httplib::Request& req, httplib::Response& res) {
                            closeConnectionRoute(svc, req, res);
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

    // SSE: one stream multiplexes status/connections/update/pin/pairRequests events.
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

                if (g_updateService) {
                    event += "event: update\ndata: ";
                    event += buildUpdateJson(g_updateService->snapshot());
                    event += "\n\n";
                }

                // Pushed each tick so the countdown ticks and a fresh tab sees
                // current state without a parallel /api/pin/status poll.
                {
                    PinSnapshot pinSnap = pinSnapshot();
                    event += "event: pin\ndata: {\"state\":\"";
                    event += pinStateName(pinSnap.state);
                    event += "\",\"secondsRemaining\":";
                    event += std::to_string(pinSnap.secondsRemaining);
                    event += "}\n\n";
                }

                // Pushed each tick so the accept/deny panel appears the instant a dish asks.
                {
                    auto pairReqs = pendingPairRequests();
                    event += "event: pairRequests\ndata: [";
                    for (size_t i = 0; i < pairReqs.size(); i++) {
                        const auto& r = pairReqs[i];
                        if (i) event += ",";
                        event += "{\"deviceId\":\"" + jsonEscape(r.deviceId) +
                                 "\",\"deviceName\":\"" + jsonEscape(r.deviceName) +
                                 "\",\"clientIP\":\"" + jsonEscape(r.clientIP) +
                                 "\",\"secondsRemaining\":" + std::to_string(r.secondsRemaining) +
                                 "}";
                    }
                    event += "]\n\n";
                }

                if (!sink.write(event.c_str(), event.size())) return false;
                for (int i = 0; i < 10 && g_appRunning; i++) netSleepMs(100);
            }
            return false;
        });
    });

    logMsg(LogLevel::INFO, "web", "Admin web UI on 127.0.0.1:" + std::to_string(g_config.webPort));
    g_httpServer.listen("127.0.0.1", g_config.webPort);
}

// Client API server — pairing + connections. HTTPS (self-signed), 0.0.0.0.
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

    // POST /api/pair — PIN-gated; no device auth (the device is not paired yet).
    server.Post("/api/pair",
                [](const httplib::Request& req, httplib::Response& res) { pairRoute(req, res); });

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

    // POST /api/connections — open a session. Requires a paired deviceId.
    server.Post("/api/connections", [&svc](const httplib::Request& req, httplib::Response& res) {
        if (!clientAuthorized(req, res)) return;
        openConnectionRoute(svc, req, res);
    });

    // DELETE /api/connections/:id — a sender tears down its own session.
    server.Delete(R"(/api/connections/(\w+))",
                  [&svc](const httplib::Request& req, httplib::Response& res) {
                      if (!clientAuthorized(req, res)) return;
                      closeConnectionRoute(svc, req, res);
                  });

    // Authoritative setter: the client pushes its touchpad mode here.
    server.Post("/api/devices/touchpad-mode",
                [&svc](const httplib::Request& req, httplib::Response& res) {
                    if (!clientAuthorized(req, res)) return;
                    handleTouchpadModeSet(svc, req, res);
                });

    // No auth: capabilities aren't sensitive and the picker needs them pre-session.
    server.Get("/api/server/capabilities", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(buildCapabilitiesJson(), "application/json");
    });

    g_clientServer = &server;
    logMsg(LogLevel::INFO, "client",
           "Client API (HTTPS) on 0.0.0.0:" + std::to_string(DEFAULT_CLIENT_PORT));
    server.listen("0.0.0.0", DEFAULT_CLIENT_PORT);
    g_clientServer = nullptr;
}
