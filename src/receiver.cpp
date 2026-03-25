/*
 * receiver.cpp — UDP receiver thread
 */
#include "receiver.h"
#include "vigem.h"

// SIO_UDP_CONNRESET is missing from some MinGW headers
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

void receiverThread() {
    // Highest thread priority — this is the hot input path
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    // Pin to a single CPU core to avoid L1/L2 cache thrash from core migration
    SetThreadAffinityMask(GetCurrentThread(), 1ULL);

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

        SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == INVALID_SOCKET) {
            unplugTarget(busDevice, serialNo);
            CloseHandle(busDevice);
            g_wantListen = false;
            continue;
        }

        // Disable SIO_UDP_CONNRESET — prevents recvfrom failing with WSAECONNRESET
        // when the sender disappears and an ICMP port-unreachable is received
        BOOL bNewBehavior = FALSE;
        DWORD dwBytesReturned = 0;
        WSAIoctl(sock, SIO_UDP_CONNRESET, &bNewBehavior, sizeof(bNewBehavior),
                 nullptr, 0, &dwBytesReturned, nullptr, nullptr);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons((u_short)port);
        addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            closesocket(sock);
            unplugTarget(busDevice, serialNo);
            CloseHandle(busDevice);
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

        // DSCP EF — marks outbound packets (rumble/feedback) as voice-priority
        // Wi-Fi WMM maps this to AC_VO, skipping ahead of bulk traffic
        int tos = 0xB8;  // DSCP EF (46) << 2
        setsockopt(sock, IPPROTO_IP, IP_TOS,
                   reinterpret_cast<const char*>(&tos), sizeof(tos));

        g_listening = true;
        g_packetCount.store(0, std::memory_order_relaxed);
        g_submitOk.store(0, std::memory_order_relaxed);
        g_submitFail.store(0, std::memory_order_relaxed);
        g_lastLoopUs.store(0, std::memory_order_relaxed);
        g_maxLoopUs.store(0, std::memory_order_relaxed);
        g_senderIP.store(0);  // 0.0.0.0 = no sender yet

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

                g_packetCount.fetch_add(1, std::memory_order_relaxed);

                // Lock-free sender IP update every 256 packets
                if ((g_packetCount.load(std::memory_order_relaxed) & 0xFF) == 0) {
                    g_senderIP.store(sender.sin_addr.s_addr);
                }
            }
            // On timeout (WSAETIMEDOUT) or WSAEWOULDBLOCK: just loop back — no Sleep needed
        }

        CloseHandle(submitEvent);

        g_listening = false;
        closesocket(sock);
        unplugTarget(busDevice, serialNo);
        CloseHandle(busDevice);
    }
}
