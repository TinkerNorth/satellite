// SPDX-License-Identifier: LGPL-3.0-or-later

// Legacy UDP broadcast beacon, fallback for senders that predate the mDNS
// responder. Toggleable at runtime; slated for removal in 2027.
#include "discovery.h"
#include "config.h"
#include "discovery_beacon.h"
#include "machine_id.h"

void discoveryThread() {
    if (!netInit()) return;

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        netShutdown();
        return;
    }

    int bcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&bcast),
               sizeof(bcast));

    char hostname[256] = {};
    netGetHostname(hostname, sizeof(hostname));

    // Stable per-install id: outlives DHCP lease changes, so the dish keys its
    // remembered-satellite entry on this instead of the mutable IP.
    const std::string machineId = ensureMachineId();

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = INADDR_BROADCAST;

    // Thread runs for the process lifetime even when disabled, so flipping the
    // config flag hot-resumes broadcasting.
    bool announced = false;
    while (g_appRunning) {
        // Snapshot under g_configMtx; the webserver mutates these from
        // POST /api/config under the same lock.
        bool enabled = false;
        int discPort = 0, udpPort = 0;
        {
            std::lock_guard<std::mutex> lk(g_configMtx);
            enabled = g_config.discoveryBroadcastEnabled;
            discPort = g_config.discPort;
            udpPort = g_config.udpPort;
        }
        if (enabled != announced) {
            logMsg(LogLevel::INFO, "discovery",
                   enabled ? "Legacy UDP broadcast beacon enabled"
                           : "Legacy UDP broadcast beacon disabled; mDNS responder still active");
            announced = enabled;
        }

        if (enabled) {
            dest.sin_port = htons((uint16_t)discPort);

            // pairPort / httpPort both carry the single HTTPS client API port.
            const std::string beacon = buildDiscoveryBeacon(hostname, udpPort, DEFAULT_CLIENT_PORT,
                                                            DEFAULT_CLIENT_PORT, machineId);

            sendto(sock, beacon.c_str(), (int)beacon.size(), 0, reinterpret_cast<sockaddr*>(&dest),
                   sizeof(dest));
        }

        // 2 s in 100ms slices so shutdown is noticed promptly.
        for (int i = 0; i < 20 && g_appRunning; i++) netSleepMs(100);
    }

    closesocket(sock);
    netShutdown();
}
