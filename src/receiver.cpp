/*
 * receiver.cpp — UDP receiver thread
 */
#include "receiver.h"
#include "vigem.h"

void receiverThread() {
    // Highest thread priority — this is the hot input path
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

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
            if (pluginTarget(busDevice, s)) {
                serialNo = s;
                break;
            }
        }
        if (serialNo == 0) {
            CloseHandle(busDevice);
            g_wantListen = false;
            continue;
        }

        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
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
        addr.sin_port = htons((u_short)port);
        addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            closesocket(sock);
            unplugTarget(busDevice, serialNo);
            CloseHandle(busDevice);
            WSACleanup();
            g_wantListen = false;
            continue;
        }

        // Blocking socket with short timeout so we can check g_wantListen
        DWORD rcvTimeout = 10; // 10 ms — wakes instantly on data, checks flags on timeout
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&rcvTimeout), sizeof(rcvTimeout));

        // Enlarge receive buffer to absorb bursts (64 KB)
        int rcvBuf = 65536;
        setsockopt(sock, SOL_SOCKET, SO_RCVBUF,
                   reinterpret_cast<const char*>(&rcvBuf), sizeof(rcvBuf));

        g_listening = true;
        g_packetCount = 0;
        g_submitOk = 0;
        g_submitFail = 0;
        g_lastLoopUs = 0;
        g_maxLoopUs = 0;
        {
            std::lock_guard<std::mutex> lk(g_senderMtx);
            g_senderIP = "none";
        }

        XUSB_REPORT report;
        XUSB_REPORT_INIT(&report);

        // Pre-allocate overlapped event for submit (avoids CreateEvent/CloseHandle per packet)
        HANDLE submitEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

        while (g_appRunning && g_wantListen) {
            sockaddr_in sender{};
            int slen = sizeof(sender);
            char buf[64];
            int n =
                recvfrom(sock, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&sender), &slen);

            if (n == sizeof(XUSB_REPORT)) {
                auto t0 = std::chrono::steady_clock::now();

                memcpy(&report, buf, sizeof(XUSB_REPORT));
                bool ok = submitReportFast(busDevice, serialNo, report, submitEvent);

                auto t1 = std::chrono::steady_clock::now();
                uint64_t us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                                  t1 - t0)
                                  .count();
                g_lastLoopUs.store(us, std::memory_order_relaxed);
                uint64_t prev = g_maxLoopUs.load(std::memory_order_relaxed);
                while (us > prev &&
                       !g_maxLoopUs.compare_exchange_weak(prev, us, std::memory_order_relaxed))
                    ;

                if (ok)
                    g_submitOk.fetch_add(1, std::memory_order_relaxed);
                else
                    g_submitFail.fetch_add(1, std::memory_order_relaxed);

                g_packetCount++;

                if ((g_packetCount & 0xFF) == 0) {
                    char ip[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &sender.sin_addr, ip, sizeof(ip));
                    std::lock_guard<std::mutex> lk(g_senderMtx);
                    g_senderIP = ip;
                }
            }
            // On timeout (WSAETIMEDOUT) or WSAEWOULDBLOCK: just loop back — no Sleep needed
        }

        CloseHandle(submitEvent);

        g_listening = false;
        closesocket(sock);
        unplugTarget(busDevice, serialNo);
        CloseHandle(busDevice);
        WSACleanup();
    }
}
