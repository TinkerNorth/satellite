// SPDX-License-Identifier: LGPL-3.0-or-later

// Admin (loopback web UI) route handlers, moved verbatim from webserver.cpp
// in the D10 decomposition. The only mechanical change inside the moved code
// is `g_httpServer.` -> `server.` (the server is now a parameter so tests can
// register the same routes on a loopback instance); JSON builders used only
// by this surface stay file-static.
#include "routes_admin.h"
#include "routes_common.h"
#include "crypto.h"
#include "config.h"
#include "pairing.h"
#include "pairing_service.h"
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

using satellite::buildDebugJson;
using satellite::buildSseStatusObject;
using satellite::buildStatusJson;
using satellite::Json;
using satellite::jsonBool;
using satellite::jsonDump;
using satellite::JsonOut;
using satellite::jsonStr;
using satellite::jsonTryBool;
using satellite::jsonTryInt;
using satellite::StatusFields;

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

// Admin server: web UI + admin API. Plain HTTP, 127.0.0.1, no auth.
void registerAdminRoutes(httplib::Server& server, SessionService& svc) {
    // Reject cross-origin / rebound requests before any route runs.
    server.set_pre_routing_handler([](const httplib::Request& req, httplib::Response& res) {
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

    server.set_mount_point("/", g_webDir);

    server.Get("/", [](const httplib::Request&, httplib::Response& res) {
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
    server.Get("/dashboard", serveIndex);
    server.Get("/settings", serveIndex);
    server.Get("/debug", serveIndex);
    server.Get("/logs", serveIndex);
    server.Get("/donate", serveIndex);

    server.Get("/api/backend/status", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(buildBackendJson(), "application/json");
    });

    server.Get("/api/status", [&svc](const httplib::Request&, httplib::Response& res) {
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

    server.Get("/api/netinfo", [](const httplib::Request&, httplib::Response& res) {
        NetworkInfo info;
        std::string selected;
        {
            std::lock_guard<std::mutex> lk(g_configMtx);
            info.udpPort = g_config.udpPort;
            info.webPort = g_config.webPort;
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

    server.Post("/api/network/allow-public", [](const httplib::Request&, httplib::Response& res) {
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

    server.Post("/api/config", [](const httplib::Request& req, httplib::Response& res) {
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

    server.Get("/api/version", [](const httplib::Request&, httplib::Response& res) {
        JsonOut j;
        j["version"] = SATELLITE_VERSION;
        j["platformId"] = g_updateService ? g_updateService->snapshot().platformId : "unknown";
        res.set_content(jsonDump(j), "application/json");
    });

    server.Get("/api/updates/status", [](const httplib::Request&, httplib::Response& res) {
        if (!g_updateService) {
            res.status = 503;
            res.set_content(R"({"error":"updater not initialized"})", "application/json");
            return;
        }
        res.set_content(buildUpdateJson(g_updateService->snapshot()), "application/json");
    });

    server.Post("/api/updates/check", [](const httplib::Request&, httplib::Response& res) {
        if (!g_updateService) {
            res.status = 503;
            res.set_content(R"({"error":"updater not initialized"})", "application/json");
            return;
        }
        g_updateService->requestCheck(/*userInitiated=*/true);
        res.set_content(R"({"ok":true})", "application/json");
    });

    server.Post("/api/updates/download", [](const httplib::Request&, httplib::Response& res) {
        if (!g_updateService) {
            res.status = 503;
            res.set_content(R"({"error":"updater not initialized"})", "application/json");
            return;
        }
        g_updateService->requestDownload();
        res.set_content(R"({"ok":true})", "application/json");
    });

    server.Post("/api/updates/install", [](const httplib::Request&, httplib::Response& res) {
        if (!g_updateService) {
            res.status = 503;
            res.set_content(R"({"error":"updater not initialized"})", "application/json");
            return;
        }
        g_updateService->requestInstall();
        res.set_content(R"({"ok":true})", "application/json");
    });

    server.Post("/api/updates/cancel", [](const httplib::Request&, httplib::Response& res) {
        if (g_updateService) g_updateService->cancelInFlight();
        res.set_content(R"({"ok":true})", "application/json");
    });

    server.Post("/api/updates/skip", [](const httplib::Request& req, httplib::Response& res) {
        std::string v = jsonStr(parseBody(req.body), "version");
        if (v.empty() || !g_updateService) {
            res.status = 400;
            res.set_content(R"({"error":"missing version"})", "application/json");
            return;
        }
        g_updateService->skipVersion(v);
        res.set_content(R"({"ok":true})", "application/json");
    });

    server.Post("/api/updates/dismiss", [](const httplib::Request&, httplib::Response& res) {
        if (g_updateService) g_updateService->dismiss();
        res.set_content(R"({"ok":true})", "application/json");
    });

    server.Post("/api/updates/preferences", [](const httplib::Request& req,
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
    server.Get("/api/pin/status", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(buildPinJson(), "application/json");
    });

    // Reverse-direction pairing (dish shows a PIN, operator accepts here).
    // Localhost admin surface (operator is at the satellite), so no device auth.
    server.Get("/api/pair/requests", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(buildPairRequestsJson(), "application/json");
    });

    server.Post("/api/pair/respond", [](const httplib::Request& req, httplib::Response& res) {
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

    server.Get("/api/devices", [&svc](const httplib::Request&, httplib::Response& res) {
        res.set_content(buildDevicesJson(svc), "application/json");
    });

    // Admin unpair. Closes any live session first: an unpaired device must not
    // keep streaming on a key the server no longer trusts.
    server.Delete(
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

    server.Get("/api/server/capabilities", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(buildCapabilitiesJson(), "application/json");
    });

    server.Get("/api/debug", [&svc](const httplib::Request&, httplib::Response& res) {
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

    server.Get("/api/connections", [&svc](const httplib::Request&, httplib::Response& res) {
        res.set_content(buildConnectionsJson(svc), "application/json");
    });

    // Admin kick is transient by design (a retrying client may re-PUT and
    // reconnect; to keep a device out, unpair it). Close-notify rides first.
    server.Delete(
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

    server.Get("/api/logs", [](const httplib::Request& req, httplib::Response& res) {
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
    server.Get("/api/events", [&svc](const httplib::Request&, httplib::Response& res) {
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
}
