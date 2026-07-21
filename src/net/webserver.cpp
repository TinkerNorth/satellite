// SPDX-License-Identifier: LGPL-3.0-or-later

// Server lifecycle + wiring ONLY. Route handlers live in routes_admin.cpp
// (loopback web UI + admin API) and routes_client.cpp (sender-facing HTTPS
// API), with the helpers both share in routes_common.cpp (D10 decomposition).
#include "webserver.h"
#include "routes_admin.h"
#include "routes_client.h"
#include "tls.h"
#include "core/types.h"

// Admin server: web UI + admin API. Plain HTTP, 127.0.0.1, no auth.
void adminHttpThread(SessionService& svc) {
    registerAdminRoutes(g_httpServer, svc);

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

    registerClientRoutes(server, svc);

    g_clientServer = &server;
    logMsg(LogLevel::INFO, "client",
           "Client API (HTTPS) on 0.0.0.0:" + std::to_string(DEFAULT_CLIENT_PORT));
    server.listen("0.0.0.0", DEFAULT_CLIENT_PORT);
    g_clientServer = nullptr;
}
