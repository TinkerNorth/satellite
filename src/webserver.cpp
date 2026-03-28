/*
 * webserver.cpp — HTTP server thread with all API routes
 */
#include "webserver.h"
#include "crypto.h"
#include "config.h"
#include "vigem.h"
#include "core/session_service.h"

// ── Auth middleware helper ───────────────────────────────────────────────────
static bool requireAuth(const httplib::Request& req, httplib::Response& res) {
    if (!isConfigured(g_config)) return true;

    // 1) Session cookie (web UI)
    auto token = getSessionFromCookie(req);
    if (validateSession(token)) return true;

    // 2) deviceId-based auth (paired sender devices)
    //    Check X-Device-Id header first, then fall back to body JSON
    std::string deviceId;
    auto hdr = req.headers.find("X-Device-Id");
    if (hdr != req.headers.end()) {
        deviceId = hdr->second;
    }
    if (deviceId.empty() && !req.body.empty()) {
        deviceId = jsonGetString(req.body, "deviceId");
    }
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

// ── Build connections JSON from SessionService snapshot ──────────────────────
static std::string buildConnectionsJson(SessionService& svc) {
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
        json += "\",\"deviceId\":\"" + jsonEscape(cs.deviceId) +
                "\",\"deviceName\":\"" + jsonEscape(cs.deviceName) +
                "\",\"senderIP\":\"" + jsonEscape(cs.clientIP) + "\"";

        json += ",\"connectedAtEpoch\":" + std::to_string(cs.connectedAtEpoch);

        json += ",\"controllers\":[";
        bool cfirst = true;
        for (const auto& ctrl : cs.controllers) {
            if (!cfirst) json += ",";
            cfirst = false;
            json += "{\"controllerIndex\":" + std::to_string(ctrl.index) +
                    ",\"vigemSerialNo\":" + std::to_string(ctrl.serial) +
                    ",\"vigemPluggedIn\":" + (ctrl.serial > 0 ? "true" : "false") + "}";
        }
        json += "],\"activeControllerCount\":" + std::to_string(cs.activeControllerCount) + "}";
    }
    json += "],\"totalControllers\":" + std::to_string(snap.totalControllers) +
            ",\"maxControllers\":" + std::to_string(snap.maxControllers) +
            ",\"vigemAvailable\":" + (snap.vigemAvailable ? "true" : "false") + "}";
    return json;
}

void httpThread(SessionService& svc) {
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
    g_httpServer.Get("/logs", serveIndex);

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
        logMsg(LogLevel::INFO, "web", "Initial setup completed for user: " + username);
    });

    g_httpServer.Post("/api/auth/login", [](const httplib::Request& req, httplib::Response& res) {
        auto username = jsonGetString(req.body, "username");
        auto password = jsonGetString(req.body, "password");
        if (!verifyCredentials(g_config, username, password)) {
            logMsg(LogLevel::WARN, "web", "Failed login attempt for user: " + username);
            res.status = 401;
            res.set_content(R"({"error":"invalid credentials"})", "application/json");
            return;
        }
        auto token = createSession();
        res.set_header("Set-Cookie", "session=" + token + "; HttpOnly; Path=/; SameSite=Strict");
        res.set_content(R"({"ok":true})", "application/json");
        logMsg(LogLevel::INFO, "web", "User logged in: " + username);
    });

    g_httpServer.Post("/api/auth/logout", [](const httplib::Request& req, httplib::Response& res) {
        auto token = getSessionFromCookie(req);
        if (!token.empty()) removeSession(token);
        res.set_header("Set-Cookie", "session=; HttpOnly; Path=/; Max-Age=0");
        res.set_content(R"({"ok":true})", "application/json");
    });

    // ── Protected routes ────────────────────────────────────────────────
    g_httpServer.Get("/api/vigem/status", [&svc](const httplib::Request& req, httplib::Response& res) {
        if (!requireAuth(req, res)) return;
        bool installed = isVigemInstalled();
        bool available = svc.isViGEmAvailable();
        char json[128];
        snprintf(json, sizeof(json), R"({"installed":%s,"available":%s})",
                 installed ? "true" : "false", available ? "true" : "false");
        res.set_content(json, "application/json");
    });

    g_httpServer.Get("/api/status", [&svc](const httplib::Request& req, httplib::Response& res) {
        if (!requireAuth(req, res)) return;
        char senderIP[INET_ADDRSTRLEN] = "none";
        uint32_t ipRaw = g_senderIP.load(std::memory_order_relaxed);
        if (ipRaw != 0) {
            in_addr ia; ia.s_addr = ipRaw;
            inet_ntop(AF_INET, &ia, senderIP, sizeof(senderIP));
        }
        bool vigemUp = svc.isViGEmAvailable();
        bool vigemInstalled = isVigemInstalled();
        char json[512];
        snprintf(
            json, sizeof(json),
            R"({"listening":%s,"packets":%llu,"senderIP":"%s","udpPort":%d,"webPort":%d,"autoStart":%s,"vigemInstalled":%s,"vigemAvailable":%s})",
            g_listening.load() ? "true" : "false", (unsigned long long)g_packetCount.load(),
            senderIP, g_config.udpPort, g_config.webPort,
            g_config.autoStart ? "true" : "false",
            vigemInstalled ? "true" : "false",
            vigemUp ? "true" : "false");
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
        logMsg(LogLevel::INFO, "web", "Config updated: udpPort=" + std::to_string(g_config.udpPort) + " autoStart=" + std::string(g_config.autoStart ? "true" : "false"));
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
    g_httpServer.Get("/api/debug", [&svc](const httplib::Request& req, httplib::Response& res) {
        if (!requireAuth(req, res)) return;
        char senderIP[INET_ADDRSTRLEN] = "none";
        uint32_t ipRaw = g_senderIP.load(std::memory_order_relaxed);
        if (ipRaw != 0) {
            in_addr ia; ia.s_addr = ipRaw;
            inet_ntop(AF_INET, &ia, senderIP, sizeof(senderIP));
        }
        uint64_t maxUs = g_maxLoopUs.exchange(0, std::memory_order_relaxed);
        char json[1024];
        bool vigemUp = svc.isViGEmAvailable();
        bool vigemInst = isVigemInstalled();
        snprintf(json, sizeof(json),
                 R"({"listening":%s,"packets":%llu,"submitOk":%llu,"submitFail":%llu,)"
                 R"("lastLoopUs":%llu,"maxLoopUs":%llu,"senderIP":"%s","udpPort":%d,)"
                 R"("decryptFail":%llu,"replayDrop":%llu,)"
                 R"("vigemInstalled":%s,"vigemAvailable":%s})",
                 g_listening.load() ? "true" : "false",
                 (unsigned long long)g_packetCount.load(),
                 (unsigned long long)g_submitOk.load(),
                 (unsigned long long)g_submitFail.load(),
                 (unsigned long long)g_lastLoopUs.load(),
                 (unsigned long long)maxUs, senderIP, g_config.udpPort,
                 (unsigned long long)g_decryptFail.load(),
                 (unsigned long long)g_replayDrop.load(),
                 vigemInst ? "true" : "false",
                 vigemUp ? "true" : "false");
        res.set_content(json, "application/json");
    });

    // ── Connection management endpoints ──────────────────────────────

    // POST /api/connections — open a new connection for a paired device
    g_httpServer.Post("/api/connections", [&svc](const httplib::Request& req, httplib::Response& res) {
        if (!requireAuth(req, res)) return;

        auto deviceId = jsonGetString(req.body, "deviceId");
        if (deviceId.empty()) {
            logMsg(LogLevel::WARN, "web", "POST /api/connections: missing deviceId");
            res.status = 400;
            res.set_content(R"({"error":"missing deviceId"})", "application/json");
            return;
        }

        // Find paired device
        PairedDevice* found = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_configMtx);
            for (auto& d : g_config.pairedDevices) {
                if (d.id == deviceId) { found = &d; break; }
            }
        }
        if (!found) {
            logMsg(LogLevel::WARN, "web", "POST /api/connections: device not paired (id=" + deviceId + ")");
            res.status = 403;
            res.set_content(R"({"error":"device not paired"})", "application/json");
            return;
        }

        // Auto-start the receiver if not already listening
        if (!g_wantListen) {
            g_wantListen = true;
            logMsg(LogLevel::INFO, "web", "Auto-starting receiver for incoming connection");
        }

        // Decode shared key
        uint8_t sharedKey[CRYPTO_KEY_SIZE];
        if (!hexDecode(found->sharedKeyHex, sharedKey, CRYPTO_KEY_SIZE)) {
            logMsg(LogLevel::ERR, "web", "POST /api/connections: invalid shared key for " + found->name);
            res.status = 500;
            res.set_content(R"({"error":"invalid shared key"})", "application/json");
            return;
        }

        // Delegate to SessionService (handles stale cleanup, token gen, slot counting)
        auto result = svc.openSession(found->id, found->name, found->lastIP, sharedKey);
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
    });

    // GET /api/connections — list active connections
    g_httpServer.Get("/api/connections", [&svc](const httplib::Request& req, httplib::Response& res) {
        if (!requireAuth(req, res)) return;
        res.set_content(buildConnectionsJson(svc), "application/json");
    });

    // DELETE /api/connections/:id — close a connection
    g_httpServer.Delete(R"(/api/connections/(\w+))",
        [&svc](const httplib::Request& req, httplib::Response& res) {
        if (!requireAuth(req, res)) return;
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

        // Delegate teardown entirely to SessionService
        int removed = svc.closeSession(token);
        if (removed < 0) {
            res.status = 404;
            res.set_content(R"({"error":"connection not found"})", "application/json");
            return;
        }

        res.set_content("{\"ok\":true,\"controllersRemoved\":" + std::to_string(removed) + "}",
                        "application/json");
    });

    // ── Log endpoint ────────────────────────────────────────────────
    g_httpServer.Get("/api/logs", [](const httplib::Request& req, httplib::Response& res) {
        if (!requireAuth(req, res)) return;

        // Optional ?since=<seq> parameter — return only entries after that sequence
        uint64_t since = 0;
        if (req.has_param("since")) {
            since = strtoull(req.get_param_value("since").c_str(), nullptr, 10);
        }

        std::lock_guard<std::mutex> lk(g_logMtx);

        // How many entries exist in the ring
        int count = (int)std::min(g_logSeq, (uint64_t)LOG_RING_SIZE);
        uint64_t oldestSeq = g_logSeq - count; // seq of the oldest entry in ring

        std::string json = "{\"seq\":" + std::to_string(g_logSeq) + ",\"entries\":[";
        bool first = true;

        for (int i = 0; i < count; i++) {
            uint64_t entrySeq = oldestSeq + i;
            if (entrySeq <= since) continue; // client already has this

            int idx = (g_logHead - count + i + LOG_RING_SIZE) % LOG_RING_SIZE;
            const auto& e = g_logRing[idx];

            if (!first) json += ",";
            first = false;

            auto epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                e.timestamp.time_since_epoch()).count();
            const char* lvl = (e.level == LogLevel::ERR) ? "error"
                            : (e.level == LogLevel::WARN) ? "warn" : "info";

            json += "{\"seq\":" + std::to_string(entrySeq) +
                    ",\"ts\":" + std::to_string(epoch_ms) +
                    ",\"level\":\"" + lvl +
                    "\",\"source\":\"" + jsonEscape(e.source) +
                    "\",\"message\":\"" + jsonEscape(e.message) + "\"}";
        }
        json += "]}";
        res.set_content(json, "application/json");
    });

    // ── SSE: Server-Sent Events for real-time updates ────────────────
    g_httpServer.Get("/api/events", [&svc](const httplib::Request& req, httplib::Response& res) {
        if (!isConfigured(g_config)) return;
        auto token = getSessionFromCookie(req);
        if (!validateSession(token)) {
            res.status = 401;
            return;
        }

        res.set_header("Cache-Control", "no-cache");
        res.set_header("X-Accel-Buffering", "no");
        res.set_chunked_content_provider("text/event-stream",
            [&svc](size_t /*offset*/, httplib::DataSink& sink) {
                while (g_appRunning) {
                    // Build status snapshot
                    char senderIP[INET_ADDRSTRLEN] = "none";
                    uint32_t ipRaw = g_senderIP.load(std::memory_order_relaxed);
                    if (ipRaw != 0) {
                        in_addr ia; ia.s_addr = ipRaw;
                        inet_ntop(AF_INET, &ia, senderIP, sizeof(senderIP));
                    }

                    std::string connJson = buildConnectionsJson(svc);

                    uint64_t logSeqNow;
                    {
                        std::lock_guard<std::mutex> lk2(g_logMtx);
                        logSeqNow = g_logSeq;
                    }

                    bool vigemUp = svc.isViGEmAvailable();
                    bool vigemInst = isVigemInstalled();
                    char statusBuf[1024];
                    snprintf(statusBuf, sizeof(statusBuf),
                        R"({"listening":%s,"packets":%llu,"senderIP":"%s","udpPort":%d,)"
                        R"("autoStart":%s,"vigemInstalled":%s,"vigemAvailable":%s,)"
                        R"("submitOk":%llu,"submitFail":%llu,)"
                        R"("lastLoopUs":%llu,"decryptFail":%llu,"replayDrop":%llu,)"
                        R"("logSeq":%llu})",
                        g_listening.load() ? "true" : "false",
                        (unsigned long long)g_packetCount.load(),
                        senderIP, g_config.udpPort,
                        g_config.autoStart ? "true" : "false",
                        vigemInst ? "true" : "false",
                        vigemUp ? "true" : "false",
                        (unsigned long long)g_submitOk.load(),
                        (unsigned long long)g_submitFail.load(),
                        (unsigned long long)g_lastLoopUs.load(),
                        (unsigned long long)g_decryptFail.load(),
                        (unsigned long long)g_replayDrop.load(),
                        (unsigned long long)logSeqNow);

                    std::string event = "event: status\ndata: ";
                    event += statusBuf;
                    event += "\n\n";

                    event += "event: connections\ndata: ";
                    event += connJson;
                    event += "\n\n";

                    if (!sink.write(event.c_str(), event.size())) return false;
                    // Sleep 1 second between updates
                    for (int i = 0; i < 10 && g_appRunning; i++) Sleep(100);
                }
                return false;
            });
    });

    logMsg(LogLevel::INFO, "web", "Web server starting on 0.0.0.0:" + std::to_string(g_config.webPort));
    g_httpServer.listen("0.0.0.0", g_config.webPort);
}
