/*
 * receiver.cpp — UDP receiver thread with encrypted multi-controller protocol
 */
#include "receiver.h"
#include "vigem.h"
#include "crypto.h"

// SIO_UDP_CONNRESET is missing from some MinGW headers
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

// ── Serial allocator helpers ─────────────────────────────────────────────────
static ULONG allocateSerial() {
    std::lock_guard<std::mutex> lk(g_serialMtx);
    for (int i = 0; i < MAX_VIGEM_CONTROLLERS; i++) {
        if (!g_serialInUse[i]) {
            g_serialInUse[i] = true;
            return (ULONG)(i + 1); // serials are 1-based
        }
    }
    return 0; // no slots
}

static void releaseSerial(ULONG serial) {
    if (serial == 0 || serial > (ULONG)MAX_VIGEM_CONTROLLERS) return;
    std::lock_guard<std::mutex> lk(g_serialMtx);
    g_serialInUse[serial - 1] = false;
}

// ── Unplug all controllers for a connection and remove it ────────────────────
// Caller must hold g_connMtx
static void teardownConnection(Connection& conn) {
    for (auto& ctrl : conn.controllers) {
        if (ctrl.active && ctrl.serialNo != 0) {
            unplugTarget(g_busDevice, ctrl.serialNo);
            if (ctrl.submitEvent) CloseHandle(ctrl.submitEvent);
            ctrl.submitEvent = nullptr;
            releaseSerial(ctrl.serialNo);
            ctrl.active = false;
            ctrl.serialNo = 0;
        }
    }
    conn.activeControllerCount = 0;
}

// ── Send heartbeat ACK back to client ────────────────────────────────────────
static void sendHeartbeatAck(SOCKET sock, Connection& conn) {
    // Build inner message: type(2B) + length(2B), no payload
    uint8_t inner[4];
    inner[0] = (uint8_t)(MSG_HEARTBEAT_ACK >> 8);
    inner[1] = (uint8_t)(MSG_HEARTBEAT_ACK);
    inner[2] = 0; inner[3] = 0; // length = 0

    uint8_t ct[4 + AUTH_TAG_SIZE + 16]; // inner + tag + margin
    unsigned long long ctLen = 0;
    // Use counter 0 for server-to-client (server doesn't track its own counter)
    if (encryptPacket(conn.sharedKey, 0, conn.token, inner, 4, ct, &ctLen)) {
        // Build full packet: token(4) + counter(4) + ciphertext
        uint8_t pkt[HEADER_SIZE + sizeof(ct)];
        uint32_t t = conn.token;
        pkt[0] = (uint8_t)(t >> 24); pkt[1] = (uint8_t)(t >> 16);
        pkt[2] = (uint8_t)(t >> 8);  pkt[3] = (uint8_t)(t);
        pkt[4] = 0; pkt[5] = 0; pkt[6] = 0; pkt[7] = 0; // counter 0
        memcpy(pkt + HEADER_SIZE, ct, ctLen);
        sendto(sock, reinterpret_cast<const char*>(pkt), (int)(HEADER_SIZE + ctLen), 0,
               reinterpret_cast<sockaddr*>(&conn.clientAddr), sizeof(conn.clientAddr));
    }
}

// ── Reaper: clean up timed-out connections ───────────────────────────────────
static void reaperLoop() {
    while (g_appRunning && g_wantListen.load()) {
        Sleep(1000);
        auto now = std::chrono::steady_clock::now();
        auto timeout = std::chrono::seconds(HEARTBEAT_INTERVAL_SEC * HEARTBEAT_MISS_MAX);

        std::lock_guard<std::mutex> lk(g_connMtx);
        for (auto it = g_connections.begin(); it != g_connections.end(); ) {
            if (now - it->second.lastPacketTime > timeout) {
                logMsg(LogLevel::INFO, "receiver", "Reaper: timed out connection for " + it->second.deviceName);
                teardownConnection(it->second);
                it = g_connections.erase(it);
            } else {
                ++it;
            }
        }
    }
}

// ── Main receiver loop ──────────────────────────────────────────────────────
void receiverThread() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    SetThreadAffinityMask(GetCurrentThread(), 1ULL);

    while (g_appRunning) {
        while (g_appRunning && !g_wantListen) { Sleep(50); }
        if (!g_appRunning) break;

        int port = g_config.udpPort;

        // Open ViGEm bus (shared across all connections)
        g_busDevice = openVigemBus();
        if (g_busDevice == INVALID_HANDLE_VALUE) {
            logMsg(LogLevel::ERR, "receiver", "Failed to open ViGEm bus");
            g_wantListen = false;
            continue;
        }

        SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == INVALID_SOCKET) {
            logMsg(LogLevel::ERR, "receiver", "Failed to create UDP socket");
            CloseHandle(g_busDevice);
            g_busDevice = INVALID_HANDLE_VALUE;
            g_wantListen = false;
            continue;
        }

        BOOL bNewBehavior = FALSE;
        DWORD dwBytesReturned = 0;
        WSAIoctl(sock, SIO_UDP_CONNRESET, &bNewBehavior, sizeof(bNewBehavior),
                 nullptr, 0, &dwBytesReturned, nullptr, nullptr);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons((u_short)port);
        addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            logMsg(LogLevel::ERR, "receiver", "Failed to bind UDP port " + std::to_string(port));
            closesocket(sock);
            CloseHandle(g_busDevice);
            g_busDevice = INVALID_HANDLE_VALUE;
            g_wantListen = false;
            continue;
        }

        g_udpSock = sock;

        DWORD rcvTimeout = 10;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&rcvTimeout), sizeof(rcvTimeout));
        int rcvBuf = 65536;
        setsockopt(sock, SOL_SOCKET, SO_RCVBUF,
                   reinterpret_cast<const char*>(&rcvBuf), sizeof(rcvBuf));
        int tos = 0xB8;
        setsockopt(sock, IPPROTO_IP, IP_TOS,
                   reinterpret_cast<const char*>(&tos), sizeof(tos));

        logMsg(LogLevel::INFO, "receiver", "Listening on UDP port " + std::to_string(port));
        g_listening = true;
        g_packetCount.store(0, std::memory_order_relaxed);
        g_submitOk.store(0, std::memory_order_relaxed);
        g_submitFail.store(0, std::memory_order_relaxed);
        g_lastLoopUs.store(0, std::memory_order_relaxed);
        g_maxLoopUs.store(0, std::memory_order_relaxed);
        g_decryptFail.store(0, std::memory_order_relaxed);
        g_replayDrop.store(0, std::memory_order_relaxed);
        g_senderIP.store(0);

        // Start reaper thread
        std::thread reaper(reaperLoop);

        while (g_appRunning && g_wantListen) {
            sockaddr_in sender{};
            int slen = sizeof(sender);
            uint8_t buf[256];
            int n = recvfrom(sock, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                             reinterpret_cast<sockaddr*>(&sender), &slen);

            // Minimum packet: header(8) + inner_header(4) + tag(16) = 28 bytes
            if (n < HEADER_SIZE + INNER_HEADER_SIZE + AUTH_TAG_SIZE) continue;

            auto t0 = std::chrono::steady_clock::now();

            // Parse plaintext header
            uint32_t token = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
                             ((uint32_t)buf[2] << 8) | (uint32_t)buf[3];
            uint32_t counter = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) |
                               ((uint32_t)buf[6] << 8) | (uint32_t)buf[7];

            // Look up connection by token
            std::lock_guard<std::mutex> lk(g_connMtx);
            auto it = g_connections.find(token);
            if (it == g_connections.end()) continue;

            Connection& conn = it->second;

            // Replay protection
            if (counter <= conn.lastCounter && conn.lastCounter != 0) {
                g_replayDrop.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            // Decrypt
            uint8_t* ciphertext = buf + HEADER_SIZE;
            size_t ctLen = (size_t)(n - HEADER_SIZE);
            uint8_t plaintext[256];
            unsigned long long ptLen = 0;
            if (!decryptPacket(conn.sharedKey, counter, token, ciphertext, ctLen, plaintext, &ptLen)) {
                g_decryptFail.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            // Update connection state
            conn.lastCounter = counter;
            conn.lastPacketTime = t0;
            conn.clientAddr = sender;

            // Parse inner message
            if (ptLen < INNER_HEADER_SIZE) continue;
            uint16_t msgType = ((uint16_t)plaintext[0] << 8) | (uint16_t)plaintext[1];
            uint16_t msgLen  = ((uint16_t)plaintext[2] << 8) | (uint16_t)plaintext[3];
            if ((size_t)(INNER_HEADER_SIZE + msgLen) > ptLen) continue;
            uint8_t* payload = plaintext + INNER_HEADER_SIZE;

            switch (msgType) {
            case MSG_GAMEPAD_DATA: {
                // payload: controller_index(1B) + XUSB_REPORT(12B) = 13 bytes
                if (msgLen < 13) break;
                uint8_t ctrlIdx = payload[0];
                if (ctrlIdx >= MAX_CONTROLLERS_PER_CONN) break;

                Controller& ctrl = conn.controllers[ctrlIdx];
                if (!ctrl.active) break;

                XUSB_REPORT report;
                memcpy(&report, payload + 1, sizeof(XUSB_REPORT));
                ctrl.lastReport = report;

                bool ok = submitReportFast(g_busDevice, ctrl.serialNo, report, ctrl.submitEvent);

                auto t1 = std::chrono::steady_clock::now();
                uint64_t us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
                g_lastLoopUs.store(us, std::memory_order_relaxed);
                uint64_t prev = g_maxLoopUs.load(std::memory_order_relaxed);
                while (us > prev && !g_maxLoopUs.compare_exchange_weak(prev, us, std::memory_order_relaxed));

                if (ok) g_submitOk.fetch_add(1, std::memory_order_relaxed);
                else    g_submitFail.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            case MSG_HEARTBEAT_PING:
                sendHeartbeatAck(sock, conn);
                break;

            case MSG_CONTROLLER_ADD: {
                // payload: controller_index(1B) + capabilities(2B) = 3 bytes
                if (msgLen < 1) break;
                uint8_t ctrlIdx = payload[0];
                if (ctrlIdx >= MAX_CONTROLLERS_PER_CONN) break;

                Controller& ctrl = conn.controllers[ctrlIdx];
                if (ctrl.active) break; // already exists

                ULONG serial = allocateSerial();
                if (serial == 0) break; // no slots

                if (!pluginTarget(g_busDevice, serial)) {
                    releaseSerial(serial);
                    break;
                }

                ctrl.index = ctrlIdx;
                ctrl.serialNo = serial;
                ctrl.active = true;
                ctrl.submitEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                XUSB_REPORT_INIT(&ctrl.lastReport);
                conn.activeControllerCount++;
                logMsg(LogLevel::INFO, "receiver", "Controller #" + std::to_string(ctrlIdx) + " added (serial " + std::to_string(serial) + ") for " + conn.deviceName);
                break;
            }
            case MSG_CONTROLLER_REMOVE: {
                if (msgLen < 1) break;
                uint8_t ctrlIdx = payload[0];
                if (ctrlIdx >= MAX_CONTROLLERS_PER_CONN) break;

                Controller& ctrl = conn.controllers[ctrlIdx];
                if (!ctrl.active) break;

                logMsg(LogLevel::INFO, "receiver", "Controller #" + std::to_string(ctrlIdx) + " removed from " + conn.deviceName);
                unplugTarget(g_busDevice, ctrl.serialNo);
                if (ctrl.submitEvent) CloseHandle(ctrl.submitEvent);
                ctrl.submitEvent = nullptr;
                releaseSerial(ctrl.serialNo);
                ctrl.active = false;
                ctrl.serialNo = 0;
                conn.activeControllerCount--;
                break;
            }
            default:
                break;
            }

            g_packetCount.fetch_add(1, std::memory_order_relaxed);

            if ((g_packetCount.load(std::memory_order_relaxed) & 0xFF) == 0) {
                g_senderIP.store(sender.sin_addr.s_addr);
            }
        }

        g_listening = false;
        g_udpSock = INVALID_SOCKET;
        closesocket(sock);

        // Tear down all connections
        {
            std::lock_guard<std::mutex> lk(g_connMtx);
            for (auto& [tok, conn] : g_connections) {
                teardownConnection(conn);
            }
            g_connections.clear();
        }

        // Reset serial allocator
        {
            std::lock_guard<std::mutex> lk(g_serialMtx);
            memset(g_serialInUse, 0, sizeof(g_serialInUse));
        }

        reaper.join();
        CloseHandle(g_busDevice);
        g_busDevice = INVALID_HANDLE_VALUE;
    }
}
