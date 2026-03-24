/*
 * receiver.cpp — UDP receiver thread
 */
#include "receiver.h"
#include "vigem.h"

void receiverThread() {
    while (g_appRunning) {
        while (g_appRunning && !g_wantListen) { Sleep(50); }
        if (!g_appRunning) break;

        int port = g_config.udpPort;

        HANDLE busDevice = openVigemBus();
        if (busDevice == INVALID_HANDLE_VALUE) {
            g_wantListen = false;
            continue;
        }

        ULONG serialNo = 0;
        for (ULONG s = 1; s <= 16; ++s) {
            if (pluginTarget(busDevice, s)) { serialNo = s; break; }
        }
        if (serialNo == 0) {
            CloseHandle(busDevice);
            g_wantListen = false;
            continue;
        }

        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
            unplugTarget(busDevice, serialNo);
            CloseHandle(busDevice);
            g_wantListen = false;
            continue;
        }

        SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == INVALID_SOCKET) {
            unplugTarget(busDevice, serialNo);
            CloseHandle(busDevice);
            WSACleanup();
            g_wantListen = false;
            continue;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons((u_short)port);
        addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
            closesocket(sock);
            unplugTarget(busDevice, serialNo);
            CloseHandle(busDevice);
            WSACleanup();
            g_wantListen = false;
            continue;
        }

        u_long nonBlock = 1;
        ioctlsocket(sock, FIONBIO, &nonBlock);

        g_listening = true;
        g_packetCount = 0;
        { std::lock_guard<std::mutex> lk(g_senderMtx); g_senderIP = "none"; }

        XUSB_REPORT report;
        XUSB_REPORT_INIT(&report);

        while (g_appRunning && g_wantListen) {
            sockaddr_in sender{};
            int slen = sizeof(sender);
            char buf[64];
            int n = recvfrom(sock, buf, sizeof(buf), 0, (sockaddr*)&sender, &slen);

            if (n == sizeof(XUSB_REPORT)) {
                memcpy(&report, buf, sizeof(XUSB_REPORT));
                submitReport(busDevice, serialNo, report);
                g_packetCount++;

                if ((g_packetCount & 0xFF) == 0) {
                    char ip[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &sender.sin_addr, ip, sizeof(ip));
                    std::lock_guard<std::mutex> lk(g_senderMtx);
                    g_senderIP = ip;
                }
            } else if (n == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
                // real error — keep going
            } else if (n == SOCKET_ERROR) {
                Sleep(1);
            }
        }

        g_listening = false;
        closesocket(sock);
        unplugTarget(busDevice, serialNo);
        CloseHandle(busDevice);
        WSACleanup();
    }
}

