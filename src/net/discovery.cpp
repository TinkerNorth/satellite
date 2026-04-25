// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * discovery.cpp — UDP broadcast beacon for LAN discovery
 *
 * Every 2 seconds, broadcasts a JSON packet on UDP port 9879 (configurable)
 * so that senders on the same LAN can auto-detect this receiver.
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

    while (g_appRunning) {
        dest.sin_port = htons((uint16_t)g_config.discPort);

        // Build beacon JSON
        char beacon[512];
        snprintf(beacon, sizeof(beacon),
                 R"({"service":"satellite","name":"%s","udpPort":%d,"pairPort":%d,"httpPort":%d})",
                 hostname, g_config.udpPort, g_config.pairPort, g_config.webPort);

        sendto(sock, beacon, (int)strlen(beacon), 0, reinterpret_cast<sockaddr*>(&dest),
               sizeof(dest));

        // Sleep 2 seconds in 100ms increments to allow quick shutdown
        for (int i = 0; i < 20 && g_appRunning; i++) netSleepMs(100);
    }

    closesocket(sock);
    netShutdown();
}
