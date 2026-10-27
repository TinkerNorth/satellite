// SPDX-License-Identifier: LGPL-3.0-or-later

// Admin (loopback web UI) route handlers. Each handler is a named function
// and registerAdminRoutes is the table that binds paths to them; the JSON
// builders used only by this surface stay file-static.
#include "routes_admin.h"
#include "admin_json.h"
#include "event_stream.h"
#include "routes_common.h"
#include "crypto.h"
#include "config.h"
#include "pairing.h"
#include "pairing_service.h"
#include "adapters/crash_adapter.h"
#include "core/config_effects.h"
#include "core/config_patch.h"
#include "core/event_broadcaster.h"
#include "core/json.h"
#include "core/session_service.h"
#include "core/tracked_mutex.h"
#include "core/update_service.h"
#include "core/update_types.h"
#include "core/version.h"
#include "core/firewall_status.h"
#include "core/network_info.h"
#include "local_iface.h"
#include "mdns_protocol.h"
#include "origin_guard.h"
#include "status_json.h"

#include <cstdlib>
#include <memory>
#include <string>

using satellite::applyConfigPatch;
using satellite::buildDebugJson;
using satellite::buildSseStatusObject;
using satellite::buildStatusJson;
using satellite::buildUpdateJson;
using satellite::ConfigEffects;
using satellite::ConfigEffectSinks;
using satellite::configPatchLogLine;
using satellite::ConfigPatchOutcome;
using satellite::EventBroadcaster;
using satellite::Json;
using satellite::jsonBool;
using satellite::jsonDump;
using satellite::JsonOut;
using satellite::jsonStr;
using satellite::senderIpLabel;
using satellite::serveEventStreams;
using satellite::sseEvent;
using satellite::StatusFields;

namespace crash = satellite::crash;

using Request = httplib::Request;
using Response = httplib::Response;

static constexpr const char* CACHE_CONTROL = "Cache-Control";
static constexpr const char* NO_CACHE = "no-cache";

static std::string buildPinJson() {
    const PinSnapshot s = pinSnapshot();
    JsonOut j;
    j["state"] = pinStateName(s.state);
    j["currentPin"] = s.currentPin;
    j["previousPin"] = s.previousPin;
    j["secondsRemaining"] = s.secondsRemaining;
    return jsonDump(j);
}

static std::string buildPairRequestsJson() {
    const auto reqs = pendingPairRequests();
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

// The settings the status surfaces show, read from one config snapshot.
static void readConfigStatus(StatusFields& f) {
    const std::shared_ptr<const Config> cfg = g_configStore->current();
    f.udpPort = cfg->udpPort;
    f.webPort = cfg->webPort;
    f.autoStart = cfg->autoStart;
    f.discoveryBroadcastEnabled = cfg->discoveryBroadcastEnabled;
    f.controllerAudio = cfg->controllerAudio;
    f.controllerAudioMic = cfg->controllerAudioMic;
    f.controllerAudioSpeaker = cfg->controllerAudioSpeaker;
    f.controllerAudioHaptics = cfg->controllerAudioHaptics;
    f.controllerAudioKeepDefaultDevice = cfg->controllerAudioKeepDefaultDevice;
    f.crashReporting = cfg->crashReporting;
}

// The receiver thread's counters, as the dashboard reads them.
static void readReceiverStatus(StatusFields& f) {
    f.listening = g_listening.load();
    f.packets = static_cast<uint64_t>(g_packetCount.load());
    f.submitOk = static_cast<uint64_t>(g_submitOk.load());
    f.submitFail = static_cast<uint64_t>(g_submitFail.load());
    f.lastLoopUs = static_cast<uint64_t>(g_lastLoopUs.load());
    f.decryptFail = static_cast<uint64_t>(g_decryptFail.load());
    f.replayDrop = static_cast<uint64_t>(g_replayDrop.load());
    f.senderIP = senderIpLabel(g_senderIP.load(std::memory_order_relaxed));
}

// The wire counters the debug page breaks down. `rx.input` is the receiver's
// own tally rather than the wire's, since every gamepad frame is counted where
// it is submitted.
static void readWireStatus(StatusFields& f) {
    const satellite::WireCounts w = satellite::g_wire.snapshot();
    f.rx = w.rx;
    f.rx.input = f.submitOk + f.submitFail;
    f.tx = w.tx;
    f.authNotPaired = w.authNotPaired;
    f.authBadProof = w.authBadProof;
    f.sessionsReaped = w.sessionsReaped;
}

static uint64_t currentLogSeq() { return logWriter().recent().published(); }

// The updater is wired per platform and null where it is not; a route that
// needs it says so with a 503.
static UpdateService* updaterOr503(Response& res) {
    if (g_updateService == nullptr) replyError(res, 503, "updater not initialized");
    return g_updateService;
}

static void replyUpdateSnapshot(Response& res, const UpdateService& updater) {
    replyJson(res, buildUpdateJson(updater.snapshot()));
}

// Rejects cross-origin / rebound requests before any route runs.
static httplib::Server::HandlerResponse guardOrigin(const Request& req, Response& res) {
    setThreadRole(ThreadRole::HttpAdmin);
    const std::string host = req.get_header_value("Host");
    if (!host.empty() && !satellite::isLoopbackHost(host)) {
        replyError(res, 403, "forbidden host");
        return httplib::Server::HandlerResponse::Handled;
    }
    const bool mutating = req.method != "GET" && req.method != "HEAD" && req.method != "OPTIONS";
    if (mutating) {
        const std::string origin = req.get_header_value("Origin");
        if (!origin.empty() && !satellite::isLoopbackOrigin(origin)) {
            replyError(res, 403, "cross-site request blocked");
            return httplib::Server::HandlerResponse::Handled;
        }
    }
    return httplib::Server::HandlerResponse::Unhandled;
}

static void redirectToDashboard(const Request&, Response& res) { res.set_redirect("/dashboard"); }

// SPA routes fall back to index.html.
static void serveIndex(const Request&, Response& res) {
    const std::string html = readFile(g_webDir + "/index.html");
    if (html.empty()) {
        res.status = 404;
        return;
    }
    res.set_header(CACHE_CONTROL, NO_CACHE);
    res.set_content(html, "text/html");
}

static void backendStatusRoute(const Request&, Response& res) {
    replyJson(res, buildBackendStatusJson());
}

static void installDriverRoute(const Request& req, Response& res) {
    const std::string id = jsonStr(parseBody(req.body), "id");
    if (id.empty()) {
        replyError(res, 400, "missing id");
        return;
    }
    std::string err;
    if (!installBundledDriver(id, err)) {
        replyError(res, 400, err);
        return;
    }
    replyOk(res);
}

static void statusRoute(SessionService& svc, const Request&, Response& res) {
    StatusFields f;
    f.backend = backendJsonObj(probeBackend());
    f.backendAvailable = svc.isBackendAvailable();
    readConfigStatus(f);
    f.crashReportingActive = crash::active();
    readReceiverStatus(f);
    f.mdnsResponderActive = g_mdnsResponderActive.load();
    replyJson(res, buildStatusJson(f));
}

static void netinfoRoute(const Request&, Response& res) {
    const std::shared_ptr<const Config> cfg = g_configStore->current();
    NetworkInfo info;
    const std::string selected = cfg->networkInterface;
    info.udpPort = cfg->udpPort;
    info.webPort = cfg->webPort;
    info.discPort = cfg->discPort;
    info.allowPublic = cfg->allowPublicNetwork;
    info.clientPort = DEFAULT_CLIENT_PORT;
    info.mdnsPort = mdns::MULTICAST_PORT;
    info.selected = selected;
    info.interfaces = enumerateInterfaces(true);
    const int idx = chooseInterface(info.interfaces, selected);
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
    replyJson(res, buildNetworkInfoJson(info));
}

static void allowPublicRoute(const Request&, Response& res) {
    const bool ok = allowPublicFirewall();
    if (ok) {
        g_configStore->update([](Config& cfg) {
            cfg.allowPublicNetwork = true;
            return true;
        });
    }
    JsonOut resp;
    resp["ok"] = ok;
    replyJson(res, jsonDump(resp));
}

static bool patchConfig(Config& cfg, const Json& body, ConfigPatchOutcome& outcome) {
    outcome = applyConfigPatch(body, cfg);
    return true;
}

// The form's patch, published in one update so a concurrent reader never sees
// half a POST. The response echoes the port in force, which after a rejection
// is the one that was already stored.
static void configRoute(ConfigEffects& effects, const Request& req, Response& res) {
    const Json body = parseBody(req.body);
    ConfigPatchOutcome patch;
    const std::shared_ptr<const Config> cfg =
        g_configStore->update([&](Config& next) { return patchConfig(next, body, patch); });
    effects.apply(*g_configStore);
    logMsg(LogLevel::INFO, "web", configPatchLogLine(*cfg, patch.udpPortRejected));
    JsonOut resp;
    resp["ok"] = true;
    resp["udpPort"] = cfg->udpPort;
    resp["udpPortRejected"] = patch.udpPortRejected;
    replyJson(res, jsonDump(resp));
}

static void versionRoute(const Request&, Response& res) {
    JsonOut j;
    j["version"] = SATELLITE_VERSION;
    j["platformId"] = g_updateService ? g_updateService->snapshot().platformId : "unknown";
    replyJson(res, jsonDump(j));
}

static void updatesStatusRoute(const Request&, Response& res) {
    if (UpdateService* updater = updaterOr503(res)) replyUpdateSnapshot(res, *updater);
}

static void updatesCheckRoute(const Request&, Response& res) {
    if (UpdateService* updater = updaterOr503(res)) {
        updater->requestCheck(/*userInitiated=*/true);
        replyUpdateSnapshot(res, *updater);
    }
}

static void updatesDownloadRoute(const Request&, Response& res) {
    if (UpdateService* updater = updaterOr503(res)) {
        updater->requestDownload();
        replyUpdateSnapshot(res, *updater);
    }
}

static void updatesRepairRoute(const Request&, Response& res) {
    if (UpdateService* updater = updaterOr503(res)) {
        updater->requestRepair();
        replyUpdateSnapshot(res, *updater);
    }
}

static void updatesInstallRoute(const Request&, Response& res) {
    if (UpdateService* updater = updaterOr503(res)) {
        updater->requestInstall();
        replyUpdateSnapshot(res, *updater);
    }
}

static void updatesRetryRoute(const Request&, Response& res) {
    if (UpdateService* updater = updaterOr503(res)) {
        updater->requestRetry();
        replyUpdateSnapshot(res, *updater);
    }
}

// Cancel and dismiss are no-ops without an updater rather than errors: the
// dashboard sends them on a state it may only have inferred.
static void updatesCancelRoute(const Request&, Response& res) {
    UpdateService* const updater = g_updateService;
    if (updater == nullptr) {
        replyOk(res);
        return;
    }
    updater->cancelInFlight();
    replyUpdateSnapshot(res, *updater);
}

static void updatesDismissRoute(const Request&, Response& res) {
    UpdateService* const updater = g_updateService;
    if (updater == nullptr) {
        replyOk(res);
        return;
    }
    updater->dismiss();
    replyUpdateSnapshot(res, *updater);
}

static void updatesSkipRoute(const Request& req, Response& res) {
    const std::string v = jsonStr(parseBody(req.body), "version");
    UpdateService* const updater = g_updateService;
    if (v.empty() || updater == nullptr) {
        replyError(res, 400, "missing version");
        return;
    }
    updater->skipVersion(v);
    replyUpdateSnapshot(res, *updater);
}

static void updatesPreferencesRoute(const Request& req, Response& res) {
    UpdateService* updater = updaterOr503(res);
    if (updater == nullptr) return;
    const Json body = parseBody(req.body);
    std::string channel = jsonStr(body, "channel");
    if (channel.empty()) channel = UPDATE_CHANNEL_STABLE;
    const bool autoCheck = jsonBool(body, "autoCheck");
    const bool autoDownload = jsonBool(body, "autoDownload");
    const bool autoInstall = jsonBool(body, "autoInstall");
    updater->updatePreferences(channel, autoCheck, autoDownload, autoInstall);
    logMsg(LogLevel::INFO, "web",
           "Update prefs: channel=" + channel + " autoCheck=" + (autoCheck ? "true" : "false") +
               " autoDownload=" + (autoDownload ? "true" : "false") +
               " autoInstall=" + (autoInstall ? "true" : "false"));
    replyUpdateSnapshot(res, *updater);
}

// PINs are echoed here for the dashboard; safe because this is the
// loopback-only admin surface.
static void pinStatusRoute(const Request&, Response& res) { replyJson(res, buildPinJson()); }

// Reverse-direction pairing (dish shows a PIN, operator accepts here).
// Localhost admin surface (operator is at the satellite), so no device auth.
static void pairRequestsRoute(const Request&, Response& res) {
    replyJson(res, buildPairRequestsJson());
}

static void pairRespondRoute(const Request& req, Response& res) {
    const Json body = parseBody(req.body);
    const std::string deviceId = jsonStr(body, "deviceId");
    const bool accept = jsonBool(body, "accept");
    if (deviceId.empty()) {
        res.status = 400;
        replyJson(res, R"({"ok":false,"error":"missing deviceId"})");
        return;
    }
    if (!accept) {
        declinePairing(deviceId);
        logMsg(LogLevel::INFO, "pairing", "Operator denied pairing request " + deviceId);
        replyJson(res, R"({"ok":true,"accepted":false})");
        return;
    }
    // Key minting + persistence live in pairing_service (shared with tray prompts).
    if (!confirmPairing(deviceId)) {
        logMsg(LogLevel::WARN, "pairing",
               "Operator accept for " + deviceId + " rejected (no pending request)");
        replyJson(res, R"({"ok":false,"error":"no pending request"})");
        return;
    }
    logMsg(LogLevel::INFO, "pairing", "Operator accepted pairing for " + deviceId);
    replyJson(res, R"({"ok":true,"accepted":true})");
}

static void devicesRoute(SessionService& svc, const Request&, Response& res) {
    replyJson(res, buildDevicesJson(svc));
}

// Admin unpair. Closes any live session first: an unpaired device must not
// keep streaming on a key the server no longer trusts.
static void unpairDeviceRoute(SessionService& svc, const Request& req, Response& res) {
    const std::string deviceId = req.matches[1].str();
    const int closed = svc.closeSessionsForDevice(deviceId, CLOSE_REASON_UNPAIRED);
    if (!forgetPairedDevice(deviceId)) {
        replyError(res, 404, "device not paired");
        return;
    }
    logMsg(LogLevel::INFO, "pairing",
           "Unpaired device " + deviceId + (closed > 0 ? " (live session closed)" : ""));
    JsonOut ok;
    ok["ok"] = true;
    ok["sessionsClosed"] = closed;
    replyJson(res, jsonDump(ok));
}

static void capabilitiesRoute(const Request&, Response& res) {
    replyJson(res, buildCapabilitiesJson());
}

static void debugRoute(SessionService& svc, const Request&, Response& res) {
    // Read-and-reset: the figure is the worst loop since the last look.
    const uint64_t maxUs = g_maxLoopUs.exchange(0, std::memory_order_relaxed);
    StatusFields f;
    f.backend = backendJsonObj(probeBackend());
    f.backendAvailable = svc.isBackendAvailable();
    readConfigStatus(f);
    readReceiverStatus(f);
    f.maxLoopUs = maxUs;
    f.peakLoopUs = satellite::g_wire.observePeakLoopUs(maxUs);
    f.mdnsResponderActive = g_mdnsResponderActive.load();
    f.clientApiListening = (g_clientServer != nullptr);
    f.uiAccess = g_uiAccess.load();
    f.connections = svc.activeSessionCount();
    f.controllers = svc.totalActiveControllers();
    f.maxControllers = MAX_BACKEND_CONTROLLERS;
    readWireStatus(f);
    f.audio = svc.audioCounts();
    f.pointer = svc.pointerCounts();
    replyJson(res, buildDebugJson(f));
}

static void connectionsRoute(SessionService& svc, const Request&, Response& res) {
    replyJson(res, buildConnectionsJson(svc));
}

// Admin kick is transient by design (a retrying client may re-PUT and
// reconnect; to keep a device out, unpair it). Close-notify rides first.
static void kickConnectionRoute(SessionService& svc, const Request& req, Response& res) {
    const std::string connId = req.matches[1].str();
    const int removed = svc.closeSessionById(connId, "", CLOSE_REASON_KICKED, /*notify=*/true);
    if (removed < 0) {
        replyError(res, 404, "connection not found");
        return;
    }
    JsonOut ok;
    ok["ok"] = true;
    ok["controllersRemoved"] = removed;
    replyJson(res, jsonDump(ok));
}

// `since` is the `seq` the viewer got last time, so the reply carries every
// entry written after that call and the `seq` to send next.
static void logsRoute(const Request& req, Response& res) {
    uint64_t since = 0;
    if (req.has_param("since")) {
        since = strtoull(req.get_param_value("since").c_str(), nullptr, 10);
    }
    replyJson(res, buildLogsJson(since));
}

// One SSE tick: every stream the dashboard multiplexes, in one write.
static std::string sseTick(SessionService& svc) {
    StatusFields f;
    f.backendAvailable = svc.isBackendAvailable();
    f.backend = backendJsonObj(probeBackend());
    readConfigStatus(f);
    readReceiverStatus(f);
    f.logSeq = currentLogSeq();

    std::string frame = sseEvent("status", jsonDump(buildSseStatusObject(f)));
    frame += sseEvent("connections", buildConnectionsJson(svc));
    // The dashboard renders one device-centric list, so it needs the paired
    // set on every tick, not just the live connections.
    frame += sseEvent("devices", buildDevicesJson(svc));
    if (g_updateService) frame += sseEvent("update", buildUpdateJson(g_updateService->snapshot()));
    // Pushed each tick so the countdown ticks and a fresh tab sees current
    // state without a parallel /api/pin/status poll.
    frame += sseEvent("pin", buildPinJson());
    // Pushed each tick so the accept/deny panel appears the instant a dish asks.
    frame += sseEvent("pairRequests", buildPairRequestsJson());
    return frame;
}

// Admin server: web UI + admin API. Plain HTTP, 127.0.0.1, no auth.
void registerAdminRoutes(httplib::Server& server, SessionService& svc) {
    const auto effects = std::make_shared<ConfigEffects>(ConfigEffectSinks{
        [](bool on) { setAutoStart(on); }, [](bool on) { crash::setEnabled(on); }});
    const auto streams = std::make_shared<EventBroadcaster>([&svc] { return sseTick(svc); });
    server.set_pre_routing_handler(guardOrigin);
    server.set_mount_point("/", g_webDir, httplib::Headers{{CACHE_CONTROL, NO_CACHE}});
    server.Get("/", redirectToDashboard);
    server.Get("/dashboard", serveIndex);
    server.Get("/settings", serveIndex);
    server.Get("/debug", serveIndex);
    server.Get("/logs", serveIndex);
    server.Get("/donate", serveIndex);

    server.Get("/api/backend/status", backendStatusRoute);
    server.Post("/api/backend/install-driver", installDriverRoute);
    server.Get("/api/status",
               [&svc](const Request& req, Response& res) { statusRoute(svc, req, res); });
    server.Get("/api/netinfo", netinfoRoute);
    server.Post("/api/network/allow-public", allowPublicRoute);
    server.Post("/api/config",
                [effects](const Request& req, Response& res) { configRoute(*effects, req, res); });
    server.Get("/api/version", versionRoute);

    server.Get("/api/updates/status", updatesStatusRoute);
    server.Post("/api/updates/check", updatesCheckRoute);
    server.Post("/api/updates/download", updatesDownloadRoute);
    server.Post("/api/updates/repair", updatesRepairRoute);
    server.Post("/api/updates/install", updatesInstallRoute);
    server.Post("/api/updates/retry", updatesRetryRoute);
    server.Post("/api/updates/cancel", updatesCancelRoute);
    server.Post("/api/updates/skip", updatesSkipRoute);
    server.Post("/api/updates/dismiss", updatesDismissRoute);
    server.Post("/api/updates/preferences", updatesPreferencesRoute);

    server.Get("/api/pin/status", pinStatusRoute);
    server.Get("/api/pair/requests", pairRequestsRoute);
    server.Post("/api/pair/respond", pairRespondRoute);

    server.Get("/api/devices",
               [&svc](const Request& req, Response& res) { devicesRoute(svc, req, res); });
    server.Delete(R"(/api/devices/([^/]+))",
                  [&svc](const Request& req, Response& res) { unpairDeviceRoute(svc, req, res); });
    server.Get("/api/server/capabilities", capabilitiesRoute);
    server.Get("/api/debug",
               [&svc](const Request& req, Response& res) { debugRoute(svc, req, res); });
    server.Get("/api/connections",
               [&svc](const Request& req, Response& res) { connectionsRoute(svc, req, res); });
    server.Delete(R"(/api/connections/(\w+))", [&svc](const Request& req, Response& res) {
        kickConnectionRoute(svc, req, res);
    });
    server.Get("/api/logs", logsRoute);
    serveEventStreams(server, "/api/events", streams);
}
