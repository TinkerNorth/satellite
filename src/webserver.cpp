/*
 * webserver.cpp — HTTP server thread with all API routes
 */
#include "webserver.h"
#include "crypto.h"
#include "config.h"
#include "vigem.h"

// ── Auth middleware helper ───────────────────────────────────────────────────
static bool requireAuth(const httplib::Request& req, httplib::Response& res) {
    if (!isConfigured(g_config)) return true;
    auto token = getSessionFromCookie(req);
    if (validateSession(token)) return true;
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

void httpThread() {
    // Serve static files from web/
    g_httpServer.set_mount_point("/", g_webDir);

    // ── Root redirect ────────────────────────────────────────────────────
    g_httpServer.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_redirect("/dashboard");
    });

    // ── SPA routing: serve index.html for /setup, /login, /dashboard ────
    auto serveIndex = [](const httplib::Request&, httplib::Response& res) {
        std::string html = readFile(g_webDir + "/index.html");
        if (html.empty()) {
            res.status = 404;
            return;
        }
        res.set_content(html, "text/html");
    };
    g_httpServer.Get("/setup", serveIndex);
    g_httpServer.Get("/login", serveIndex);
    g_httpServer.Get("/dashboard", serveIndex);
    g_httpServer.Get("/debug", serveIndex);

    // ── Auth routes (no auth required) ──────────────────────────────────
    g_httpServer.Get("/api/auth/status", [](const httplib::Request& req, httplib::Response& res) {
        bool configured = isConfigured(g_config);
        bool authenticated = false;
        if (configured) {
            auto token = getSessionFromCookie(req);
            authenticated = validateSession(token);
        }
        char json[128];
        snprintf(json, sizeof(json), R"({"configured":%s,"authenticated":%s})",
                 configured ? "true" : "false", authenticated ? "true" : "false");
        res.set_content(json, "application/json");
    });

    g_httpServer.Post("/api/auth/setup", [](const httplib::Request& req, httplib::Response& res) {
        if (isConfigured(g_config)) {
            res.status = 400;
            res.set_content(R"({"error":"already configured"})", "application/json");
            return;
        }
        auto username = jsonGetString(req.body, "username");
        auto password = jsonGetString(req.body, "password");
        if (username.empty() || password.size() < 4) {
            res.status = 400;
            res.set_content(R"({"error":"username required, password min 4 chars"})",
                            "application/json");
            return;
        }
        std::lock_guard<std::mutex> lk(g_configMtx);
        if (!setupCredentials(g_config, username, password)) {
            res.status = 500;
            res.set_content(R"({"error":"encryption failed"})", "application/json");
            return;
        }
        saveConfig(g_config);
        auto token = createSession();
        res.set_header("Set-Cookie", "session=" + token + "; HttpOnly; Path=/; SameSite=Strict");
        res.set_content(R"({"ok":true})", "application/json");
    });

    g_httpServer.Post("/api/auth/login", [](const httplib::Request& req, httplib::Response& res) {
        auto username = jsonGetString(req.body, "username");
        auto password = jsonGetString(req.body, "password");
        if (!verifyCredentials(g_config, username, password)) {
            res.status = 401;
            res.set_content(R"({"error":"invalid credentials"})", "application/json");
            return;
        }
        auto token = createSession();
        res.set_header("Set-Cookie", "session=" + token + "; HttpOnly; Path=/; SameSite=Strict");
        res.set_content(R"({"ok":true})", "application/json");
    });

    g_httpServer.Post("/api/auth/logout", [](const httplib::Request& req, httplib::Response& res) {
        auto token = getSessionFromCookie(req);
        if (!token.empty()) removeSession(token);
        res.set_header("Set-Cookie", "session=; HttpOnly; Path=/; Max-Age=0");
        res.set_content(R"({"ok":true})", "application/json");
    });

    // ── Protected routes ────────────────────────────────────────────────
    g_httpServer.Get("/api/vigem/status", [](const httplib::Request& req, httplib::Response& res) {
        if (!requireAuth(req, res)) return;
        bool installed = isVigemInstalled();
        char json[64];
        snprintf(json, sizeof(json), R"({"installed":%s})", installed ? "true" : "false");
        res.set_content(json, "application/json");
    });

    g_httpServer.Get("/api/status", [](const httplib::Request& req, httplib::Response& res) {
        if (!requireAuth(req, res)) return;
        std::string senderIP;
        {
            std::lock_guard<std::mutex> lk(g_senderMtx);
            senderIP = g_senderIP;
        }
        char json[512];
        snprintf(
            json, sizeof(json),
            R"({"listening":%s,"packets":%llu,"senderIP":"%s","udpPort":%d,"webPort":%d,"autoStart":%s})",
            g_listening.load() ? "true" : "false", (unsigned long long)g_packetCount.load(),
            senderIP.c_str(), g_config.udpPort, g_config.webPort,
            g_config.autoStart ? "true" : "false");
        res.set_content(json, "application/json");
    });

    g_httpServer.Post("/api/start", [](const httplib::Request& req, httplib::Response& res) {
        if (!requireAuth(req, res)) return;
        g_wantListen = true;
        res.set_content(R"({"ok":true})", "application/json");
    });

    g_httpServer.Post("/api/stop", [](const httplib::Request& req, httplib::Response& res) {
        if (!requireAuth(req, res)) return;
        g_wantListen = false;
        res.set_content(R"({"ok":true})", "application/json");
    });

    g_httpServer.Post("/api/config", [](const httplib::Request& req, httplib::Response& res) {
        if (!requireAuth(req, res)) return;
        auto body = req.body;
        std::lock_guard<std::mutex> lk(g_configMtx);
        auto pPort = body.find("\"udpPort\"");
        if (pPort != std::string::npos) {
            auto colon = body.find(':', pPort);
            if (colon != std::string::npos) {
                int port = atoi(body.c_str() + colon + 1);
                if (port >= 1024 && port <= 65535) g_config.udpPort = port;
            }
        }
        g_config.autoStart = body.find("\"autoStart\":true") != std::string::npos ||
                             body.find("\"autoStart\": true") != std::string::npos;
        setAutoStart(g_config.autoStart);
        saveConfig(g_config);
        res.set_content(R"({"ok":true})", "application/json");
    });

    // ── PIN & device pairing routes ─────────────────────────────────────
    g_httpServer.Post("/api/pin/generate", [](const httplib::Request& req, httplib::Response& res) {
        if (!requireAuth(req, res)) return;
        auto pin = generatePin();
        res.set_content("{\"pin\":\"" + pin + "\"}", "application/json");
    });

    g_httpServer.Get("/api/devices", [](const httplib::Request& req, httplib::Response& res) {
        if (!requireAuth(req, res)) return;
        std::string json = "[";
        std::lock_guard<std::mutex> lk(g_configMtx);
        for (size_t i = 0; i < g_config.pairedDevices.size(); i++) {
            const auto& d = g_config.pairedDevices[i];
            json += "{\"id\":\"" + jsonEscape(d.id) + "\",\"name\":\"" + jsonEscape(d.name) +
                    "\",\"lastIP\":\"" + jsonEscape(d.lastIP) + "\",\"pairedAt\":\"" +
                    jsonEscape(d.pairedAt) + "\"}";
            if (i + 1 < g_config.pairedDevices.size()) json += ",";
        }
        json += "]";
        res.set_content(json, "application/json");
    });

    g_httpServer.Post(
        "/api/devices/remove", [](const httplib::Request& req, httplib::Response& res) {
            if (!requireAuth(req, res)) return;
            auto deviceId = jsonGetString(req.body, "id");
            std::lock_guard<std::mutex> lk(g_configMtx);
            auto& devs = g_config.pairedDevices;
            devs.erase(std::remove_if(devs.begin(), devs.end(),
                                      [&](const PairedDevice& d) { return d.id == deviceId; }),
                       devs.end());
            saveConfig(g_config);
            res.set_content(R"({"ok":true})", "application/json");
        });

    // ── Debug telemetry endpoint ────────────────────────────────────
    g_httpServer.Get("/api/debug", [](const httplib::Request& req, httplib::Response& res) {
        if (!requireAuth(req, res)) return;
        std::string senderIP;
        {
            std::lock_guard<std::mutex> lk(g_senderMtx);
            senderIP = g_senderIP;
        }
        uint64_t maxUs = g_maxLoopUs.exchange(0, std::memory_order_relaxed); // reset on read
        char json[512];
        snprintf(json, sizeof(json),
                 R"({"listening":%s,"packets":%llu,"submitOk":%llu,"submitFail":%llu,)"
                 R"("lastLoopUs":%llu,"maxLoopUs":%llu,"senderIP":"%s","udpPort":%d})",
                 g_listening.load() ? "true" : "false",
                 (unsigned long long)g_packetCount.load(),
                 (unsigned long long)g_submitOk.load(),
                 (unsigned long long)g_submitFail.load(),
                 (unsigned long long)g_lastLoopUs.load(),
                 (unsigned long long)maxUs, senderIP.c_str(), g_config.udpPort);
        res.set_content(json, "application/json");
    });

    g_httpServer.listen("127.0.0.1", g_config.webPort);
}
