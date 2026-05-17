// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * discovery.cpp — UDP broadcast beacon for LAN discovery
 *
 * Every 2 seconds, broadcasts a JSON packet on UDP port 9879 (configurable)
 * so that senders on the same LAN can auto-detect this receiver.
 *
 * This is the legacy discovery path. mDNS / Bonjour (net/mdns_responder.cpp,
 * Task 1.6) is the modern path; the broadcast beacon stays on as a fallback
 * for senders that predate the mDNS responder. The beacon can be toggled at
 * runtime via Config::discoveryBroadcastEnabled (web UI Settings → Discovery)
 * and is slated for removal in 2027.
 */
#include "discovery.h"
#include "config.h"

void discoveryThread() {
    if (!netInit()) return;

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        netShutdown();
        return;
    }

    // Enable broadcast
    int bcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&bcast),
               sizeof(bcast));

    // Get host name for beacon
    char hostname[256] = {};
    netGetHostname(hostname, sizeof(hostname));

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = INADDR_BROADCAST;

    // The thread runs for the process lifetime even when the beacon is
    // disabled, so flipping Config::discoveryBroadcastEnabled back on
    // hot-resumes broadcasting without a restart. `announced` tracks the last
    // logged state so each transition is logged exactly once.
    bool announced = false;
    while (g_appRunning) {
        // Read discoveryBroadcastEnabled under g_configMtx — the webserver
        // mutates it from POST /api/config under the same lock. Snapshot it
        // (plus the ports, which the same handler can change) into locals so
        // the rest of the loop iteration works off a consistent view.
        bool enabled = false;
        int discPort = 0, udpPort = 0, pairPort = 0, webPort = 0;
        {
            std::lock_guard<std::mutex> lk(g_configMtx);
            enabled = g_config.discoveryBroadcastEnabled;
            discPort = g_config.discPort;
            udpPort = g_config.udpPort;
            pairPort = g_config.pairPort;
            webPort = g_config.webPort;
        }
        if (enabled != announced) {
            logMsg(LogLevel::INFO, "discovery",
                   enabled ? "Legacy UDP broadcast beacon enabled"
                           : "Legacy UDP broadcast beacon disabled — mDNS responder still active");
            announced = enabled;
        }

        if (enabled) {
            dest.sin_port = htons((uint16_t)discPort);

            // Build beacon JSON
            char beacon[512];
            snprintf(
                beacon, sizeof(beacon),
                R"({"service":"satellite","name":"%s","udpPort":%d,"pairPort":%d,"httpPort":%d})",
                hostname, udpPort, pairPort, webPort);

            sendto(sock, beacon, (int)strlen(beacon), 0, reinterpret_cast<sockaddr*>(&dest),
                   sizeof(dest));
        }

        // Sleep 2 seconds in 100ms increments to allow quick shutdown
        for (int i = 0; i < 20 && g_appRunning; i++) netSleepMs(100);
    }

    closesocket(sock);
    netShutdown();
}
