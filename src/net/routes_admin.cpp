// SPDX-License-Identifier: LGPL-3.0-or-later

// Admin (loopback web UI) route handlers. Each handler is a named function
// and registerAdminRoutes is the table that binds paths to them; the JSON
// builders used only by this surface stay file-static.
#include "routes_admin.h"
#include "routes_common.h"
#include "crypto.h"
#include "config.h"
#include "pairing.h"
#include "pairing_service.h"
#include "adapters/crash_adapter.h"
#include "core/config_patch.h"
#include "core/json.h"
#include "core/log_ring.h"
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

#include <cstdlib>
#include <mutex>
#include <string>

using satellite::applyConfigPatch;
using satellite::buildDebugJson;
using satellite::buildSseStatusObject;
using satellite::buildStatusJson;
using satellite::configPatchLogLine;
using satellite::ConfigPatchOutcome;
using satellite::Json;
using satellite::jsonBool;
using satellite::jsonDump;
using satellite::JsonOut;
using satellite::jsonStr;
using satellite::logEntryJson;
using satellite::logRingSlotsSince;
using satellite::senderIpLabel;
using satellite::sseEvent;
using satellite::StatusFields;

namespace crash = satellite::crash;

using Request = httplib::Request;
using Response = httplib::Response;

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

static JsonOut controllerBatteryObj(const SessionService::ConnectionSnapshot::CtrlInfo& ctrl) {
    JsonOut battery;
    if (ctrl.batteryLevel == BATTERY_LEVEL_UNKNOWN) {
        battery["level"] = nullptr;
    } else {
        battery["level"] = ctrl.batteryLevel;
    }
    battery["status"] = batteryStatusName(ctrl.batteryStatus);
    return battery;
}

static JsonOut controllerSnapshotObj(const SessionService::ConnectionSnapshot::CtrlInfo& ctrl) {
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
    // Resolved here so the dashboard never rebuilds an asset path from the wire enum.
    o["catalogSlug"] = controllerTypeCatalogSlug(ctrl.controllerType);
    o["touchpadMode"] = touchpadModeName(ctrl.touchpadMode);
    if (ctrl.backendId.empty()) {
        o["backend"] = nullptr;
    } else {
        o["backend"] = ctrl.backendId;
    }
    if (ctrl.batteryKnown) {
        o["battery"] = controllerBatteryObj(ctrl);
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
        snprintf(rgb, sizeof(rgb), "#%02x%02x%02x", ctrl.lightbarR, ctrl.lightbarG, ctrl.lightbarB);
        o["lightbar"] = std::string(rgb);
    } else {
        o["lightbar"] = nullptr;
    }
    return o;
}

// `state` fields serialise as lowercase enum names, the canonical wire form.
// `connectedAtEpoch` is steady-clock seconds (boot-relative), not Unix epoch.
static JsonOut connectionSnapshotObj(const SessionService::ConnectionSnapshot& cs) {
    JsonOut c;
    c["connectionId"] = cs.connectionId;
    c["deviceId"] = cs.deviceId;
    c["deviceName"] = cs.deviceName;
    c["senderIP"] = cs.clientIP;
    c["connectedAtEpoch"] = cs.connectedAtEpoch;
    c["epoch"] = cs.epoch;
    c["mouseControlGranted"] = cs.mouseControlGranted;
    c["protocolVersion"] = cs.protocolVersion;
    c["protocolCurrent"] = PROTOCOL_VERSION;
    // Active or NotResponding here; /api/devices covers the Paired (offline) case.
    c["state"] = deviceLinkStateName(cs.linkState);
    JsonOut controllers = JsonOut::array();
    for (const auto& ctrl : cs.controllers) controllers.push_back(controllerSnapshotObj(ctrl));
    c["controllers"] = std::move(controllers);
    c["activeControllerCount"] = cs.activeControllerCount;
    return c;
}

static std::string buildConnectionsJson(const SessionService& svc) {
    const auto snap = svc.getConnectionsSnapshot();
    JsonOut connections = JsonOut::array();
    for (const auto& cs : snap.connections) connections.push_back(connectionSnapshotObj(cs));
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
        const DeviceLinkState s = svc.linkStateForDevice(d.id);
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

// The settings the status surfaces show, read under the config lock in one go.
static void readConfigStatus(StatusFields& f) {
    std::lock_guard<std::mutex> lk(g_configMtx);
    f.udpPort = g_config.udpPort;
    f.webPort = g_config.webPort;
    f.autoStart = g_config.autoStart;
    f.discoveryBroadcastEnabled = g_config.discoveryBroadcastEnabled;
    f.controllerAudio = g_config.controllerAudio;
    f.controllerAudioMic = g_config.controllerAudioMic;
    f.controllerAudioSpeaker = g_config.controllerAudioSpeaker;
    f.controllerAudioHaptics = g_config.controllerAudioHaptics;
    f.controllerAudioKeepDefaultDevice = g_config.controllerAudioKeepDefaultDevice;
    f.crashReporting = g_config.crashReporting;
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

static uint64_t currentLogSeq() {
    std::lock_guard<std::mutex> lk(g_logMtx);
    return g_logSeq;
}

// The updater is wired per platform and null where it is not; a route that
// needs it says so with a 503.
static UpdateService* updaterOr503(Response& res) {
    if (g_updateService == nullptr) replyError(res, 503, "updater not initialized");
    return g_updateService;
}

// Rejects cross-origin / rebound requests before any route runs.
static httplib::Server::HandlerResponse guardOrigin(const Request& req, Response& res) {
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
        std::lock_guard<std::mutex> lk(g_configMtx);
        g_config.allowPublicNetwork = true;
        saveConfig(g_config);
    }
    JsonOut resp;
    resp["ok"] = ok;
    replyJson(res, jsonDump(resp));
}

// The form's patch, under the config lock for the whole of it so a concurrent
// reader never sees half a POST. The response echoes the port in force, which
// after a rejection is the one that was already stored.
static void configRoute(const Request& req, Response& res) {
    const Json body = parseBody(req.body);
    std::lock_guard<std::mutex> lk(g_configMtx);
    const ConfigPatchOutcome patch = applyConfigPatch(body, g_config);
    if (patch.autoStartPresent) setAutoStart(g_config.autoStart);
    if (patch.crashReportingPresent) crash::setEnabled(g_config.crashReporting);
    saveConfig(g_config);
    logMsg(LogLevel::INFO, "web", configPatchLogLine(g_config, patch.udpPortRejected));
    JsonOut resp;
    resp["ok"] = true;
    resp["udpPort"] = g_config.udpPort;
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
    if (UpdateService* updater = updaterOr503(res)) {
        replyJson(res, buildUpdateJson(updater->snapshot()));
    }
}

static void updatesCheckRoute(const Request&, Response& res) {
    if (UpdateService* updater = updaterOr503(res)) {
        updater->requestCheck(/*userInitiated=*/true);
        replyOk(res);
    }
}

static void updatesDownloadRoute(const Request&, Response& res) {
    if (UpdateService* updater = updaterOr503(res)) {
        updater->requestDownload();
        replyOk(res);
    }
}

static void updatesRepairRoute(const Request&, Response& res) {
    if (UpdateService* updater = updaterOr503(res)) {
        updater->requestRepair();
        replyOk(res);
    }
}

static void updatesInstallRoute(const Request&, Response& res) {
    if (UpdateService* updater = updaterOr503(res)) {
        updater->requestInstall();
        replyOk(res);
    }
}

// Cancel and dismiss are no-ops without an updater rather than errors: the
// dashboard sends them on a state it may only have inferred.
static void updatesCancelRoute(const Request&, Response& res) {
    if (g_updateService) g_updateService->cancelInFlight();
    replyOk(res);
}

static void updatesDismissRoute(const Request&, Response& res) {
    if (g_updateService) g_updateService->dismiss();
    replyOk(res);
}

static void updatesSkipRoute(const Request& req, Response& res) {
    const std::string v = jsonStr(parseBody(req.body), "version");
    if (v.empty() || !g_updateService) {
        replyError(res, 400, "missing version");
        return;
    }
    g_updateService->skipVersion(v);
    replyOk(res);
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
    replyOk(res);
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
    f.connections = svc.activeSessionCount();
    f.controllers = svc.totalActiveControllers();
    f.maxControllers = MAX_BACKEND_CONTROLLERS;
    readWireStatus(f);
    f.audio = svc.audioCounts();
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
    std::lock_guard<std::mutex> lk(g_logMtx);
    JsonOut entries = JsonOut::array();
    for (const auto& slot : logRingSlotsSince(LOG_RING_SIZE, g_logHead, g_logSeq, since)) {
        entries.push_back(logEntryJson(slot.seq, g_logRing[static_cast<size_t>(slot.index)]));
    }
    JsonOut j;
    j["seq"] = g_logSeq;
    j["entries"] = std::move(entries);
    replyJson(res, jsonDump(j));
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

// One tick a second until the client goes away or the app shuts down.
static bool streamEvents(SessionService& svc, httplib::DataSink& sink) {
    while (g_appRunning) {
        const std::string frame = sseTick(svc);
        if (!sink.write(frame.c_str(), frame.size())) return false;
        for (int i = 0; i < 10 && g_appRunning; i++) netSleepMs(100);
    }
    return false;
}

// SSE: one stream multiplexes status/connections/devices/update/pin/
// pairRequests events.
static void eventsRoute(SessionService& svc, const Request&, Response& res) {
    res.set_header("Cache-Control", "no-cache");
    res.set_header("X-Accel-Buffering", "no");
    res.set_chunked_content_provider(
        "text/event-stream",
        [&svc](size_t /*offset*/, httplib::DataSink& sink) { return streamEvents(svc, sink); });
}

// Admin server: web UI + admin API. Plain HTTP, 127.0.0.1, no auth.
void registerAdminRoutes(httplib::Server& server, SessionService& svc) {
    server.set_pre_routing_handler(guardOrigin);
    server.set_mount_point("/", g_webDir);
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
    server.Post("/api/config", configRoute);
    server.Get("/api/version", versionRoute);

    server.Get("/api/updates/status", updatesStatusRoute);
    server.Post("/api/updates/check", updatesCheckRoute);
    server.Post("/api/updates/download", updatesDownloadRoute);
    server.Post("/api/updates/repair", updatesRepairRoute);
    server.Post("/api/updates/install", updatesInstallRoute);
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
    server.Get("/api/events",
               [&svc](const Request& req, Response& res) { eventsRoute(svc, req, res); });
}
