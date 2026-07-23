// SPDX-License-Identifier: LGPL-3.0-or-later
// In-process route tests for the loopback admin surface (routes_admin.cpp):
// the DNS-rebind/CSRF pre-routing guard, SPA serving, status/version/debug,
// config mutation semantics, updater 503s (updater absent), PIN status,
// reverse-pairing accept/deny, device listing/unpair, connection
// listing/kick, the log ring, netinfo, and SSE endpoint presence (one
// multiplexed tick).
//
// Routes are registered on a plain loopback httplib::Server exactly as
// production does (registerAdminRoutes is the production registration) and
// driven with httplib::Client. SessionService runs against bare port stubs.
// Hermeticity: HOME and XDG_CONFIG_HOME point into a per-run tmpdir (config
// writes + autostart artifacts live there); g_webDir points at a tmp web root.
#include "../src/core/json.h"
#include "../src/core/session_service.h"
#include "../src/net/pairing.h"
#include "../src/net/routes_admin.h"
#include "config.h" // bare platform seam: resolved per-OS by the test target
#include "crypto.h"

#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include "test_util.h"

using satellite::Json;
using satellite::jsonBool;
using satellite::jsonInt;
using satellite::jsonParse;
using satellite::jsonStr;

struct TempHome {
    std::string path;
    TempHome() {
        char tmpl[] = "/tmp/satellite-routes-XXXXXX";
        char* d = mkdtemp(tmpl);
        path = (d != nullptr) ? d : "/tmp";
        setenv("HOME", path.c_str(), 1);
        setenv("XDG_CONFIG_HOME", path.c_str(), 1);
    }
    ~TempHome() {
        if (path.rfind("/tmp/satellite-routes-", 0) == 0) {
            std::string cmd = "rm -rf " + path;
            int rc = system(cmd.c_str());
            (void)rc;
        }
    }
};

struct StubGamepad : IGamepadPort {
    bool ensureBusOpen() override { return true; }
    void closeBus() override {}
    bool isBusOpen() const override { return true; }
    bool pluginDevice(uint32_t, GamepadIdentity) override { return true; }
    bool supportsIdentity(GamepadIdentity) const override { return true; }
    bool unplugDevice(uint32_t) override { return true; }
    bool submitReport(uint32_t, const GamepadReport&) override { return true; }
    void setRumbleCallback(RumbleCallback) override {}
};

struct StubClient : IClientPort {
    void updateClientAddr(uint32_t, const std::string&, uint16_t) override {}
    void removeClientAddr(uint32_t) override {}
    void sendHeartbeatAck(const Connection&, bool, uint8_t, uint16_t, uint16_t) override {}
    void sendSessionClose(const Connection&, uint8_t) override { closeNotifies++; }
    void sendRumble(const Connection&, uint8_t, const RumbleReport&) override {}
    void sendLightbar(const Connection&, uint8_t, uint8_t, uint8_t, uint8_t) override {}
    int closeNotifies = 0;
};

struct StubLog : ILogPort {
    void logMsg(LogLevel, const std::string&, const std::string&) override {}
};

static Json parseJson(const std::string& body) {
    Json j;
    if (!jsonParse(body, j)) return Json();
    return j;
}

static std::string storedKeyHex(const std::string& id) {
    std::lock_guard<std::mutex> lk(g_configMtx);
    for (const auto& d : g_config.pairedDevices) {
        if (d.id == id) return d.sharedKeyHex;
    }
    return "";
}

int main() {
    std::cout << "Running admin API route tests...\n\n";

    if (!sodiumInit()) {
        std::cerr << "sodium init failed\n";
        return 1;
    }

    TempHome home;
    g_appRunning = true;

    // Minimal web root so the mount point + SPA fallback have something real.
    g_webDir = home.path + "/web";
    mkdir(g_webDir.c_str(), 0755);
    {
        std::ofstream f(g_webDir + "/index.html");
        f << "<!doctype html><title>satellite-test-spa</title>";
    }

    StubGamepad gamepad;
    StubClient clientPort;
    StubLog log;
    SessionService svc(gamepad, clientPort, log);

    httplib::Server server;
    registerAdminRoutes(server, svc);
    int port = server.bind_to_any_port("127.0.0.1");
    if (port <= 0) {
        std::cerr << "failed to bind loopback port\n";
        return 1;
    }
    std::thread serverThread([&server] { server.listen_after_bind(); });
    server.wait_until_ready();

    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(5);
    cli.set_read_timeout(5);

    // ---- pre-routing guard (DNS rebind / CSRF) -------------------------------
    {
        TEST("non-loopback Host header: 403 before any route runs");
        auto res = cli.Get("/api/status", {{"Host", "evil.example"}});
        EXPECT(res && res->status == 403);
        if (res) EXPECT_EQ(jsonStr(parseJson(res->body), "error"), std::string("forbidden host"));
    }
    {
        TEST("loopback Host with port passes");
        auto res = cli.Get("/api/status");
        EXPECT(res && res->status == 200);
    }
    {
        TEST("cross-site Origin on a mutating method: 403");
        auto res = cli.Post("/api/updates/cancel", {{"Origin", "http://evil.example"}}, "",
                            "application/json");
        EXPECT(res && res->status == 403);
        if (res) {
            EXPECT_EQ(jsonStr(parseJson(res->body), "error"),
                      std::string("cross-site request blocked"));
        }
    }
    {
        TEST("loopback Origin on a mutating method passes");
        auto res = cli.Post("/api/updates/cancel",
                            {{"Origin", std::string("http://127.0.0.1:") + std::to_string(port)}},
                            "", "application/json");
        EXPECT(res && res->status == 200);
    }
    {
        TEST("cross-site Origin on GET is allowed (reads are loopback-bound anyway)");
        auto res = cli.Get("/api/status", {{"Origin", "http://evil.example"}});
        EXPECT(res && res->status == 200);
    }

    // ---- web UI serving --------------------------------------------------------
    {
        TEST("GET /: mounted index.html wins (the /dashboard redirect is the "
             "fallback for a missing web root)");
        // httplib serves file mounts before route handlers, so with a real web
        // root GET / is the SPA itself, exactly as in production installs.
        auto res = cli.Get("/");
        EXPECT(res && res->status == 200);
        if (res) EXPECT(res->body.find("satellite-test-spa") != std::string::npos);
    }
    {
        TEST("SPA fallbacks serve index.html");
        for (const char* p : {"/dashboard", "/settings", "/debug", "/logs", "/donate"}) {
            auto res = cli.Get(p);
            EXPECT(res && res->status == 200);
            if (res) EXPECT(res->body.find("satellite-test-spa") != std::string::npos);
        }
    }

    // ---- status / version / debug / backend -----------------------------------
    {
        TEST("GET /api/status: config-backed fields present");
        auto res = cli.Get("/api/status");
        EXPECT(res && res->status == 200);
        if (res) {
            Json j = parseJson(res->body);
            EXPECT(j.contains("listening") && j["listening"].is_boolean());
            long udpPort = 0;
            {
                std::lock_guard<std::mutex> lk(g_configMtx);
                udpPort = g_config.udpPort;
            }
            EXPECT_EQ(jsonInt(j, "udpPort"), udpPort);
            EXPECT_EQ(jsonStr(j, "senderIP"), std::string("none"));
            EXPECT(j.contains("backend") && j["backend"].is_object());
        }
    }
    {
        TEST("GET /api/version: version string + platformId fallback");
        auto res = cli.Get("/api/version");
        EXPECT(res && res->status == 200);
        if (res) {
            Json j = parseJson(res->body);
            EXPECT_EQ(jsonStr(j, "version"), std::string(SATELLITE_VERSION));
            EXPECT_EQ(jsonStr(j, "platformId"), std::string("unknown")); // no g_updateService
        }
    }
    {
        TEST("GET /api/debug: telemetry counters present");
        auto res = cli.Get("/api/debug");
        EXPECT(res && res->status == 200);
        if (res) {
            Json j = parseJson(res->body);
            EXPECT(j.contains("submitOk"));
            EXPECT(j.contains("submitFail"));
            EXPECT(j.contains("decryptFail"));
            EXPECT(j.contains("replayDrop"));
        }
    }
    {
        TEST("GET /api/backend/status: stable identifier surface");
        auto res = cli.Get("/api/backend/status");
        EXPECT(res && res->status == 200);
        if (res) {
            Json j = parseJson(res->body);
            EXPECT(!jsonStr(j, "id").empty());
            EXPECT(j.contains("supported") && j["supported"].is_boolean());
            EXPECT(j.contains("available") && j["available"].is_boolean());
            EXPECT(j.contains("errorCode"));
        }
    }
    {
        TEST("GET /api/server/capabilities is served on the admin surface too");
        auto res = cli.Get("/api/server/capabilities");
        EXPECT(res && res->status == 200);
        if (res) {
            EXPECT_EQ(jsonInt(parseJson(res->body), "protocolVersion"),
                      static_cast<long>(PROTOCOL_VERSION));
        }
    }
    {
        TEST("GET /api/netinfo: port + firewall shape");
        auto res = cli.Get("/api/netinfo");
        EXPECT(res && res->status == 200);
        if (res) {
            Json j = parseJson(res->body);
            EXPECT(j.contains("ports") && j["ports"].is_object());
            if (j.contains("ports")) {
                EXPECT_EQ(jsonInt(j["ports"], "mdns"), 5353L);
                EXPECT_EQ(jsonInt(j["ports"], "client"), static_cast<long>(DEFAULT_CLIENT_PORT));
            }
            EXPECT(j.contains("firewall") && j["firewall"].is_object());
            EXPECT(j.contains("interfaces") && j["interfaces"].is_array());
        }
    }

    // ---- config mutation --------------------------------------------------------
    {
        TEST("POST /api/config: in-range udpPort applied and echoed");
        auto res = cli.Post("/api/config", R"({"udpPort":4242,"discoveryBroadcastEnabled":false})",
                            "application/json");
        EXPECT(res && res->status == 200);
        if (res) {
            Json j = parseJson(res->body);
            EXPECT_EQ(jsonBool(j, "ok"), true);
            EXPECT_EQ(jsonInt(j, "udpPort"), 4242L);
            EXPECT_EQ(jsonBool(j, "udpPortRejected"), false);
        }
        std::lock_guard<std::mutex> lk(g_configMtx);
        EXPECT_EQ(g_config.udpPort, 4242);
        EXPECT_EQ(g_config.discoveryBroadcastEnabled, false);
    }
    {
        TEST("POST /api/config: out-of-range udpPort rejected, not clamped");
        auto res = cli.Post("/api/config", R"({"udpPort":80})", "application/json");
        EXPECT(res && res->status == 200);
        if (res) {
            Json j = parseJson(res->body);
            EXPECT_EQ(jsonBool(j, "udpPortRejected"), true);
            EXPECT_EQ(jsonInt(j, "udpPort"), 4242L); // echoes the effective port
        }
    }
    {
        TEST("POST /api/config: absent keys leave stored values untouched");
        auto res = cli.Post("/api/config", R"({"networkInterface":"lo0"})", "application/json");
        EXPECT(res && res->status == 200);
        std::lock_guard<std::mutex> lk(g_configMtx);
        EXPECT_EQ(g_config.udpPort, 4242);
        EXPECT_EQ(g_config.discoveryBroadcastEnabled, false);
        EXPECT_EQ(g_config.networkInterface, std::string("lo0"));
    }
    {
        TEST("POST /api/config: autoStart round-trips through the platform hook");
        auto on = cli.Post("/api/config", R"({"autoStart":true})", "application/json");
        EXPECT(on && on->status == 200);
        EXPECT_EQ(getAutoStart(), true);
        auto off = cli.Post("/api/config", R"({"autoStart":false})", "application/json");
        EXPECT(off && off->status == 200);
        EXPECT_EQ(getAutoStart(), false);
    }
    {
        TEST("POST /api/network/allow-public: honest ok:false on POSIX (no-op)");
        auto res = cli.Post("/api/network/allow-public", "", "application/json");
        EXPECT(res && res->status == 200);
        if (res) EXPECT_EQ(jsonBool(parseJson(res->body), "ok"), false);
        std::lock_guard<std::mutex> lk(g_configMtx);
        EXPECT_EQ(g_config.allowPublicNetwork, false); // only set when the rules applied
    }

    // ---- updater routes (updater absent -> honest 503s) --------------------------
    {
        TEST("updates: status/check/download/install 503 without an updater");
        for (auto [method, path] :
             {std::pair<const char*, const char*>{"GET", "/api/updates/status"},
              {"POST", "/api/updates/check"},
              {"POST", "/api/updates/download"},
              {"POST", "/api/updates/install"},
              {"POST", "/api/updates/preferences"}}) {
            auto res = std::string(method) == "GET" ? cli.Get(path)
                                                    : cli.Post(path, "{}", "application/json");
            EXPECT(res && res->status == 503);
        }

        TEST("updates: cancel/dismiss are no-op 200s without an updater");
        EXPECT(cli.Post("/api/updates/cancel", "", "application/json")->status == 200);
        EXPECT(cli.Post("/api/updates/dismiss", "", "application/json")->status == 200);

        TEST("updates: skip without a version is a 400");
        auto res = cli.Post("/api/updates/skip", "{}", "application/json");
        EXPECT(res && res->status == 400);
    }

    // ---- PIN status ---------------------------------------------------------------
    {
        TEST("GET /api/pin/status: live 4-digit PIN + rotation countdown");
        auto res = cli.Get("/api/pin/status");
        EXPECT(res && res->status == 200);
        if (res) {
            Json j = parseJson(res->body);
            std::string pin = jsonStr(j, "currentPin");
            EXPECT_EQ(pin.size(), size_t{4});
            EXPECT(pin.find_first_not_of("0123456789") == std::string::npos);
            EXPECT(jsonInt(j, "secondsRemaining") > 0);
            EXPECT(jsonInt(j, "secondsRemaining") <= 300);
            EXPECT(jsonStr(j, "state") == "active" || jsonStr(j, "state") == "paired");
        }
    }

    // ---- reverse pairing (path B, operator side) ------------------------------------
    {
        TEST("GET /api/pair/requests: empty before any request");
        auto res = cli.Get("/api/pair/requests");
        EXPECT(res && res->status == 200);
        if (res) {
            Json j = parseJson(res->body);
            EXPECT(j.is_array() && j.empty());
        }
    }
    {
        TEST("a submitted dish request appears with its PIN for the operator");
        submitPairRequest("dev-adm-1", "AdmDev", "192.168.7.9", "9876");
        auto res = cli.Get("/api/pair/requests");
        EXPECT(res && res->status == 200);
        if (res) {
            Json j = parseJson(res->body);
            EXPECT(j.is_array() && j.size() == 1);
            if (j.is_array() && j.size() == 1) {
                EXPECT_EQ(jsonStr(j[0], "deviceId"), std::string("dev-adm-1"));
                EXPECT_EQ(jsonStr(j[0], "pin"), std::string("9876"));
                EXPECT_EQ(jsonStr(j[0], "clientIP"), std::string("192.168.7.9"));
            }
        }
    }
    {
        TEST("POST /api/pair/respond: deny clears the request, pairs nothing");
        auto res = cli.Post("/api/pair/respond", R"({"deviceId":"dev-adm-1","accept":false})",
                            "application/json");
        EXPECT(res && res->status == 200);
        if (res) {
            Json j = parseJson(res->body);
            EXPECT_EQ(jsonBool(j, "ok"), true);
            EXPECT_EQ(jsonBool(j, "accepted"), false);
        }
        EXPECT_EQ(storedKeyHex("dev-adm-1"), std::string(""));
        auto list = cli.Get("/api/pair/requests");
        if (list) EXPECT(parseJson(list->body).empty());
    }
    {
        TEST("POST /api/pair/respond: accept mints + persists a key");
        submitPairRequest("dev-adm-2", "AdmDev2", "192.168.7.10", "1234");
        auto res = cli.Post("/api/pair/respond", R"({"deviceId":"dev-adm-2","accept":true})",
                            "application/json");
        EXPECT(res && res->status == 200);
        if (res) {
            Json j = parseJson(res->body);
            EXPECT_EQ(jsonBool(j, "ok"), true);
            EXPECT_EQ(jsonBool(j, "accepted"), true);
        }
        std::string key = storedKeyHex("dev-adm-2");
        EXPECT_EQ(key.size(), size_t{64});
        EXPECT(key.find_first_not_of("0123456789abcdef") == std::string::npos);
    }
    {
        TEST("POST /api/pair/respond: missing deviceId is a 400");
        auto res = cli.Post("/api/pair/respond", R"({"accept":true})", "application/json");
        EXPECT(res && res->status == 400);

        TEST("POST /api/pair/respond: accept with nothing pending is ok:false");
        auto none = cli.Post("/api/pair/respond", R"({"deviceId":"dev-ghost","accept":true})",
                             "application/json");
        EXPECT(none && none->status == 200);
        if (none) {
            Json j = parseJson(none->body);
            EXPECT_EQ(jsonBool(j, "ok"), false);
            EXPECT_EQ(jsonStr(j, "error"), std::string("no pending request"));
        }
    }

    // ---- devices ---------------------------------------------------------------------
    {
        TEST("GET /api/devices: paired device listed with link state");
        auto res = cli.Get("/api/devices");
        EXPECT(res && res->status == 200);
        if (res) {
            Json j = parseJson(res->body);
            EXPECT(j.is_array() && j.size() == 1);
            if (j.is_array() && j.size() == 1) {
                EXPECT_EQ(jsonStr(j[0], "id"), std::string("dev-adm-2"));
                EXPECT_EQ(jsonStr(j[0], "state"), std::string("paired"));
            }
        }
    }
    {
        TEST("DELETE /api/devices/:id unpairs");
        auto res = cli.Delete("/api/devices/dev-adm-2");
        EXPECT(res && res->status == 200);
        if (res) {
            Json j = parseJson(res->body);
            EXPECT_EQ(jsonBool(j, "ok"), true);
            EXPECT_EQ(jsonInt(j, "sessionsClosed"), 0L);
        }
        EXPECT_EQ(storedKeyHex("dev-adm-2"), std::string(""));

        TEST("DELETE /api/devices/:id twice: 404");
        auto gone = cli.Delete("/api/devices/dev-adm-2");
        EXPECT(gone && gone->status == 404);
    }

    // ---- connections (admin view + kick) -----------------------------------------
    std::string connId;
    {
        TEST("GET /api/connections: empty scaffold first");
        auto res = cli.Get("/api/connections");
        EXPECT(res && res->status == 200);
        if (res) {
            Json j = parseJson(res->body);
            EXPECT(j.contains("connections") && j["connections"].is_array() &&
                   j["connections"].empty());
            EXPECT_EQ(jsonInt(j, "maxControllers"), static_cast<long>(MAX_BACKEND_CONTROLLERS));
        }

        TEST("a live session appears with its controllers");
        uint8_t key[CRYPTO_KEY_SIZE] = {};
        ControllerDescriptor d;
        d.ctrlIdx = 0;
        d.type = CONTROLLER_TYPE_XBOX;
        d.caps = CAP_RUMBLE;
        auto up = svc.upsertSession("dev-conn", "ConnDev", "192.168.7.11", key, {d}, false);
        EXPECT(up.ok);
        connId = up.connectionId;
        auto live = cli.Get("/api/connections");
        EXPECT(live && live->status == 200);
        if (live) {
            Json j = parseJson(live->body);
            EXPECT(j["connections"].is_array() && j["connections"].size() == 1);
            if (j["connections"].size() == 1) {
                const Json& c = j["connections"][0];
                EXPECT_EQ(jsonStr(c, "connectionId"), connId);
                EXPECT_EQ(jsonStr(c, "deviceId"), std::string("dev-conn"));
                EXPECT_EQ(jsonStr(c, "state"), std::string("active"));
                EXPECT(c.contains("controllers") && c["controllers"].is_array() &&
                       c["controllers"].size() == 1);
                if (c["controllers"].size() == 1) {
                    EXPECT_EQ(jsonStr(c["controllers"][0], "state"), std::string("live"));
                    EXPECT_EQ(jsonStr(c["controllers"][0], "controllerType"), std::string("xbox"));
                }
            }
            EXPECT_EQ(jsonInt(j, "totalControllers"), 1L);
        }
    }
    {
        TEST("admin kick: DELETE /api/connections/:id closes WITH close-notify");
        int notifiesBefore = clientPort.closeNotifies;
        auto res = cli.Delete(("/api/connections/" + connId).c_str());
        EXPECT(res && res->status == 200);
        if (res) {
            Json j = parseJson(res->body);
            EXPECT_EQ(jsonBool(j, "ok"), true);
            EXPECT_EQ(jsonInt(j, "controllersRemoved"), 1L);
        }
        EXPECT_EQ(clientPort.closeNotifies, notifiesBefore + 1);

        TEST("kick of an unknown connection: 404");
        auto gone = cli.Delete("/api/connections/conn_ffffffff");
        EXPECT(gone && gone->status == 404);
    }

    // ---- log ring ---------------------------------------------------------------------
    {
        TEST("GET /api/logs: monotonic seq + a just-written marker entry");
        logMsg(LogLevel::INFO, "route-test", "marker-entry-xyzzy");
        auto res = cli.Get("/api/logs");
        EXPECT(res && res->status == 200);
        uint64_t seq = 0;
        if (res) {
            Json j = parseJson(res->body);
            EXPECT(j.contains("entries") && j["entries"].is_array());
            seq = static_cast<uint64_t>(jsonInt(j, "seq"));
            EXPECT(seq > 0);
            EXPECT(res->body.find("marker-entry-xyzzy") != std::string::npos);
        }

        TEST("GET /api/logs?since=<head>: nothing new");
        auto res2 = cli.Get(("/api/logs?since=" + std::to_string(seq)).c_str());
        EXPECT(res2 && res2->status == 200);
        if (res2) {
            Json j = parseJson(res2->body);
            EXPECT(j.contains("entries") && j["entries"].is_array() && j["entries"].empty());
        }
    }

    // ---- SSE endpoint presence -----------------------------------------------------
    {
        TEST("GET /api/events: one multiplexed tick with every event stream");
        std::string sse;
        cli.Get("/api/events", [&sse](const char* data, size_t len) {
            sse.append(data, len);
            // Stop as soon as the last event of the first tick arrived.
            return sse.find("event: pairRequests") == std::string::npos;
        });
        EXPECT(sse.rfind("event: status\ndata: ", 0) == 0);
        EXPECT(sse.find("event: connections\ndata: ") != std::string::npos);
        EXPECT(sse.find("event: devices\ndata: ") != std::string::npos);
        EXPECT(sse.find("event: pin\ndata: ") != std::string::npos);
        EXPECT(sse.find("event: pairRequests\ndata: ") != std::string::npos);
        EXPECT(sse.find("event: update\ndata: ") == std::string::npos); // no updater wired
    }

    server.stop();
    serverThread.join();

    std::cout << "\n=== Test Results ===\n";
    std::cout << "  Passed: " << g_pass << "\n";
    std::cout << "  Failed: " << g_fail << "\n";
    std::cout << "  STATUS: " << (g_fail == 0 ? "ALL PASSED" : "FAILED") << "\n";
    return g_fail == 0 ? 0 : 1;
}
