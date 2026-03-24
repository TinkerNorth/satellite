/*
 * discovery.cpp — UDP broadcast beacon for LAN discovery
 *
 * Every 2 seconds, broadcasts a JSON packet on UDP port 9879 (configurable)
 * so that senders on the same LAN can auto-detect this receiver.
 */
#include "discovery.h"
#include "config.h"

void discoveryThread() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) return;

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) { WSACleanup(); return; }

    // Enable broadcast
    BOOL bcast = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (const char*)&bcast, sizeof(bcast));

    // Get computer name for beacon
    char hostname[256] = {};
    DWORD hsize = sizeof(hostname);
    GetComputerNameA(hostname, &hsize);

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = INADDR_BROADCAST;

    while (g_appRunning) {
        dest.sin_port = htons((u_short)g_config.discPort);

        // Build beacon JSON
        char beacon[512];
        snprintf(beacon, sizeof(beacon),
            R"({"service":"controller-forward","name":"%s","udpPort":%d,"pairPort":%d,"webPort":%d})",
            hostname, g_config.udpPort, g_config.pairPort, g_config.webPort);

        sendto(sock, beacon, (int)strlen(beacon), 0,
               (sockaddr*)&dest, sizeof(dest));

        // Sleep 2 seconds in 100ms increments to allow quick shutdown
        for (int i = 0; i < 20 && g_appRunning; i++) Sleep(100);
    }

    closesocket(sock);
    WSACleanup();
}

