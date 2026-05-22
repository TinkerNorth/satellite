// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * webserver.cpp — the two server threads.
 *
 *   adminHttpThread  — web UI + admin API. Plain HTTP, bound to 127.0.0.1.
 *                      No authentication: localhost is the trust boundary.
 *   clientApiThread  — sender-facing API (pairing + connections). HTTPS with
 *                      a self-signed cert (see tls.cpp), bound to 0.0.0.0.
 *                      The connection routes require a paired deviceId.
 */
#include "webserver.h"
#include "tls.h"
#include "crypto.h"
#include "config.h"
#include "core/gamepad_backend.h"
#include "core/session_service.h"
#include "core/update_service.h"
#include "core/update_types.h"
#include "core/version.h"

#include <sodium.h>

// ── Helper: emit the {backend: { id, supported, available, errorCode }} JSON
// fragment used by /api/backend/status, /api/status, /api/debug, and the SSE
// stream. The web UI keys all user-facing copy off (id, errorCode).
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

// ── JSON value helpers (key-scoped, body-substring-safe) ─────────────────────
// The codebase's shared JSON helper (config.h) only exposes jsonGetString.
// These two add bounded boolean / integer reads for request bodies. Unlike a
// naive body.find("\"key\":true"), they locate the *quoted* key, step past the
// colon, and inspect only the value token that immediately follows — so a
// value cannot false-positive on a substring elsewhere in the body (e.g. a
// device name, or one key's literal appearing inside another's value).
//
// Returns false when the key is absent or malformed; *out is left untouched.

// Find the start of a top-level-ish key's value: index just past the ':'.
static bool jsonValueStart(const std::string& json, const std::string& key, size_t& out) {
    std::string needle = "\"" + key + "\"";
    size_t pos = 0;
    for (;;) {
        pos = json.find(needle, pos);
        if (pos == std::string::npos) return false;
        // Reject a match that is itself the tail of a longer key, e.g. key
        // "autoCheck" must not match inside "\"xautoCheck\"". The opening
        // quote of `needle` already pins the left edge; nothing more needed
        // because the leading '"' cannot be part of an identifier.
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
    // Require a numeric value token — reject strings/objects/garbage so a
    // non-numeric value can't be silently coerced to 0 by atoi.
    if (json[t] != '-' && (json[t] < '0' || json[t] > '9')) return false;
    char* end = nullptr;
    long v = strtol(json.c_str() + t, &end, 10);
    if (end == json.c_str() + t) return false; // no digits consumed
    // Overflow (strtol → LONG_MIN/MAX) is harmless here: the caller range-
    // checks the result, so an out-of-range value is simply rejected.
    *out = v;
    return true;
}

// ── Client API authorization ─────────────────────────────────────────────────
// The client API (HTTPS, 0.0.0.0) is reachable from the LAN, so its connection
// routes require a paired deviceId. The id is read from the X-Device-Id header,
// falling back to the request body. The admin API needs no equivalent — it is
// bound to 127.0.0.1.
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
    res.status = 401;
    res.set_content(R"({"error":"unauthorized"})", "application/json");
    return false;
}

// ── Read file helper ────────────────────────────────────────────────────────
static std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return "";
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// ── Build update-status JSON from an UpdateStatusSnapshot ───────────────────
// Used by GET /api/updates/status, GET /api/version (subset), and the SSE
// "update" event channel. Keep this in sync with the web/ JS which expects
// these exact keys.
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

// ── Build connections JSON from SessionService snapshot ──────────────────────
// JSON shape includes a per-connection `state` (DeviceLinkState) and a
// per-controller `state` (ControllerState). Both serialise as lowercase enum
// names (see deviceLinkStateName / controllerStateName in core/types.h). The
// strings are the canonical wire form; the dashboard maps them onto chip text.
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
        // Per-connection link state — Active or NotResponding here. The
        // /api/devices feed covers the Paired (offline) case.
        json += ",\"state\":\"" + std::string(deviceLinkStateName(cs.linkState)) + "\"";

        json += ",\"controllers\":[";
        bool cfirst = true;
        for (const auto& ctrl : cs.controllers) {
            if (!cfirst) json += ",";
            cfirst = false;
            // Per-controller pipeline state. Today we surface only Live (a
            // virtual device exists and reports are flowing); the snapshot
            // does not yet thread the in-flight transitions
            // (Registering/Allocating/Detached) or per-controller failure
            // reasons. See ControllerState in core/types.h.
            // TODO: thread transient ControllerState through SessionService
            // so MSG_CONTROLLER_ADD's in-flight window can surface as
            // "registering" / "allocating", and the ACK_ERR_* failure code
            // can be retained per controller index to surface "failed".
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
            // True iff this satellite's backend has an IMU surface for the
            // slot's chosen controller type. The UI warns when motionCapable
            // is true but this is false ("dish wants to stream motion but
            // the backend has nowhere to land it for an Xbox-typed pad").
            json += ",\"motionSinkSupportedForType\":" +
                    std::string(ctrl.motionSinkSupportedForType ? "true" : "false");
            // True iff the platform adapter successfully created the IMU
            // sink at plug-in. False distinguishes a kernel-level failure
            // (uinput permissions, kernel too old) from "no game subscribed."
            json += ",\"motionBackendOk\":" +
                    std::string(ctrl.motionBackendOk ? "true" : "false");
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

// ── DELETE /api/connections/:id — shared by the admin and client servers ─────
// Admin uses it for the dashboard's disconnect button; a sender uses it to
// tear down its own session. The teardown is delegated to SessionService.
static void closeConnectionRoute(SessionService& svc, const httplib::Request& req,
                                 httplib::Response& res) {
    auto connId = req.matches[1].str();

    // Parse token from connId (format: conn_XXXXXXXX)
    std::string tokenStr = connId;
    if (tokenStr.substr(0, 5) == "conn_") tokenStr = tokenStr.substr(5);

    uint32_t token = 0;
    if (sscanf(tokenStr.c_str(), "%08x", &token) != 1 || token == 0) {
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

// ── POST /api/connections — open a connection for a paired device ────────────
static void openConnectionRoute(SessionService& svc, const httplib::Request& req,
                                httplib::Response& res) {
    auto deviceId = jsonGetString(req.body, "deviceId");
    if (deviceId.empty()) {
        logMsg(LogLevel::WARN, "client", "POST /api/connections: missing deviceId");
        res.status = 400;
        res.set_content(R"({"error":"missing deviceId"})", "application/json");
        return;
    }

    // Find paired device
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

    // Decode shared key
    uint8_t sharedKey[CRYPTO_KEY_SIZE];
    if (!hexDecode(found->sharedKeyHex, sharedKey, CRYPTO_KEY_SIZE)) {
        logMsg(LogLevel::ERR, "client",
               "POST /api/connections: invalid shared key for " + found->name);
        res.status = 500;
        res.set_content(R"({"error":"invalid shared key"})", "application/json");
        return;
    }

    // Delegate to SessionService (handles stale cleanup, token gen, slot counting)
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

// ── POST /api/pair — PIN-gated device pairing ────────────────────────────────
// Folds in the former TCP pairing server. Runs over HTTPS, so the PIN and the
// resulting shared key are encrypted in transit. Body: {deviceId, deviceName,
// pin, publicKey?}. Always replies 200 with a JSON body the sender classifies
// on its `ok` field.
static void pairRoute(const httplib::Request& req, httplib::Response& res) {
    auto deviceId = jsonGetString(req.body, "deviceId");
    auto deviceName = jsonGetString(req.body, "deviceName");
    auto pin = jsonGetString(req.body, "pin");
    auto clientPkHex = jsonGetString(req.body, "publicKey"); // client's X25519 public key
    const std::string clientIP = req.remote_addr;

    // Check if already paired
    bool alreadyPaired = false;
    std::string storedKey;
    {
        std::lock_guard<std::mutex> lk(g_configMtx);
        for (auto& d : g_config.pairedDevices) {
            if (d.id == deviceId) {
                alreadyPaired = true;
                d.lastIP = clientIP;
                storedKey = d.sharedKeyHex;
                break;
            }
        }
        if (alreadyPaired) saveConfig(g_config);
    }

    if (alreadyPaired) {
        logMsg(LogLevel::INFO, "pairing",
               "Device " + deviceName + " (" + clientIP + ") already paired, updating IP");
        res.set_content(R"({"ok":true,"message":"already paired","sharedKey":")" + storedKey +
                            R"("})",
                        "application/json");
        return;
    }

    if (!verifyPin(pin)) {
        logMsg(LogLevel::WARN, "pairing", "Invalid PIN attempt from " + clientIP);
        res.set_content(R"({"ok":false,"error":"invalid or expired PIN"})", "application/json");
        return;
    }

    // Generate server key pair for X25519 key exchange
    uint8_t serverPk[32], serverSk[32];
    generateKeyPair(serverPk, serverSk);

    // Decode client's public key (optional — absent for the trusted-network mode)
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
        // No client key exchange — mint a random shared key. Sent back over
        // the TLS channel, so it is not exposed on the wire.
        uint8_t randomKey[32];
        randombytes_buf(randomKey, 32);
        sharedKeyHex = hexEncode(randomKey, 32);
        sodium_memzero(randomKey, 32);
    }

    PairedDevice dev;
    dev.id = deviceId;
    dev.name = deviceName.empty() ? ("Device-" + deviceId.substr(0, 8)) : deviceName;
    dev.lastIP = clientIP;
    dev.pairedAt = getCurrentDate();
    dev.sharedKeyHex = sharedKeyHex;
    {
        std::lock_guard<std::mutex> lk(g_configMtx);
        auto& devs = g_config.pairedDevices;
        devs.erase(std::remove_if(devs.begin(), devs.end(),
                                  [&](const PairedDevice& d) { return d.id == deviceId; }),
                   devs.end());
        devs.push_back(dev);
        saveConfig(g_config);
    }

    std::string serverPkHex = hexEncode(serverPk, 32);
    sodium_memzero(serverSk, 32);
    if (hasClientKey) {
        res.set_content(R"({"ok":true,"message":"paired successfully","serverPublicKey":")" +
                            serverPkHex + R"("})",
                        "application/json");
    } else {
        res.set_content(R"({"ok":true,"message":"paired successfully","sharedKey":")" +
                            sharedKeyHex + R"("})",
                        "application/json");
    }
    logMsg(LogLevel::INFO, "pairing",
           "Successfully paired device: " + dev.name + " (" + clientIP + ")");
}

// ════════════════════════════════════════════════════════════════════════════
// Admin server — web UI + admin API. Plain HTTP, 127.0.0.1, no authentication.
// ════════════════════════════════════════════════════════════════════════════
void adminHttpThread(SessionService& svc) {
    // Serve static files from web/
    g_httpServer.set_mount_point("/", g_webDir);

    // ── Root redirect ────────────────────────────────────────────────────
    g_httpServer.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_redirect("/dashboard");
    });

    // ── SPA routing: serve index.html for the client-side routes ────────
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

    // ── Backend probe — web UI keys its copy/remediation table off (id, errorCode).
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

        // udpPort — key-scoped numeric parse. Out-of-range values are
        // rejected (not clamped); the response echoes the effective port so
        // the web UI can show what actually took effect.
        long port = 0;
        if (jsonGetIntKeyed(body, "udpPort", &port)) {
            if (port >= 1024 && port <= 65535) {
                g_config.udpPort = static_cast<int>(port);
            } else {
                portRejected = true;
            }
        }

        // autoStart — key-scoped boolean. Only applied when the key is
        // present, so a partial POST leaves the stored value untouched.
        bool autoStartVal = false;
        if (jsonGetBoolKeyed(body, "autoStart", &autoStartVal)) {
            g_config.autoStart = autoStartVal;
            setAutoStart(g_config.autoStart);
        }

        // Legacy UDP broadcast beacon toggle (Task 1.6). Key-scoped so a
        // device name or any unrelated body text can't false-positive it,
        // and applied only when present so a partial POST can't silently
        // flip discovery off.
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

    // ── Version + update endpoints ──────────────────────────────────────
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

    // ── PIN generation (the operator generates a PIN here, then types it
    // into the sender, which pairs against the client API). ────────────
    g_httpServer.Post("/api/pin/generate", [](const httplib::Request&, httplib::Response& res) {
        auto pin = generatePin();
        res.set_content("{\"pin\":\"" + pin + "\"}", "application/json");
    });

    // GET /api/pin/status — surface the PinState enum + remaining-time hint.
    // Used by the dashboard's PIN panel to render an "Expires in m:ss"
    // countdown and to flash a brief "Paired!" confirmation. Does NOT echo
    // the PIN itself — the PIN is returned only to the original POST so a
    // refresh of /dashboard doesn't leak it to a parallel admin tab.
    g_httpServer.Get("/api/pin/status", [](const httplib::Request&, httplib::Response& res) {
        PinSnapshot s = pinSnapshot();
        std::string json = "{\"state\":\"";
        json += pinStateName(s.state);
        json += "\",\"secondsRemaining\":";
        json += std::to_string(s.secondsRemaining);
        json += "}";
        res.set_content(json, "application/json");
    });

    g_httpServer.Get("/api/devices", [&svc](const httplib::Request&, httplib::Response& res) {
        // Per-device `state` is the DeviceLinkState (lowercase enum name) —
        // either "paired" (no live connection), "active" (live, recent
        // packets), or "notResponding" (live but stalled). Dashboard maps
        // these onto chip text.
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

    // POST /api/devices/touchpad-mode {id, mode} — set a paired device's
    // touchpad routing mode. `mode` is "ds4" | "mouse" | "off". Persisted to
    // config AND hot-applied to any live connection for the device.
    g_httpServer.Post("/api/devices/touchpad-mode", [&svc](const httplib::Request& req,
                                                           httplib::Response& res) {
        auto deviceId = jsonGetString(req.body, "id");
        auto modeStr = jsonGetString(req.body, "mode");
        if (deviceId.empty() || modeStr.empty()) {
            res.status = 400;
            res.set_content(R"({"error":"missing id or mode"})", "application/json");
            return;
        }
        if (modeStr != "ds4" && modeStr != "mouse" && modeStr != "off") {
            res.status = 400;
            res.set_content(R"({"error":"mode must be ds4, mouse, or off"})", "application/json");
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
            res.status = 404;
            res.set_content(R"({"error":"device not paired"})", "application/json");
            return;
        }
        bool hotApplied = svc.setTouchpadMode(deviceId, mode);
        logMsg(LogLevel::INFO, "web",
               "Touchpad mode for device " + deviceId + " set to " + modeStr +
                   (hotApplied ? " (applied to live connection)" : ""));
        res.set_content(std::string("{\"ok\":true,\"hotApplied\":") +
                            (hotApplied ? "true" : "false") + "}",
                        "application/json");
    });

    // ── Debug telemetry endpoint ────────────────────────────────────
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

    // ── Connection management (read + teardown) for the dashboard ────
    g_httpServer.Get("/api/connections", [&svc](const httplib::Request&, httplib::Response& res) {
        res.set_content(buildConnectionsJson(svc), "application/json");
    });

    g_httpServer.Delete(R"(/api/connections/(\w+))",
                        [&svc](const httplib::Request& req, httplib::Response& res) {
                            closeConnectionRoute(svc, req, res);
                        });

    // ── Log endpoint ────────────────────────────────────────────────
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

    // ── SSE: Server-Sent Events for real-time updates ────────────────
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

                // PIN state — pushed every tick so the dashboard's
                // "Expires in m:ss" countdown updates without a parallel
                // poller, and so a freshly opened tab sees the current
                // state immediately rather than waiting for the next
                // /api/pin/status fetch.
                {
                    PinSnapshot pinSnap = pinSnapshot();
                    event += "event: pin\ndata: {\"state\":\"";
                    event += pinStateName(pinSnap.state);
                    event += "\",\"secondsRemaining\":";
                    event += std::to_string(pinSnap.secondsRemaining);
                    event += "}\n\n";
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

// ════════════════════════════════════════════════════════════════════════════
// Client API server — pairing + connections. HTTPS (self-signed), 0.0.0.0.
// ════════════════════════════════════════════════════════════════════════════
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

    g_clientServer = &server;
    logMsg(LogLevel::INFO, "client",
           "Client API (HTTPS) on 0.0.0.0:" + std::to_string(DEFAULT_CLIENT_PORT));
    server.listen("0.0.0.0", DEFAULT_CLIENT_PORT);
    g_clientServer = nullptr;
}
