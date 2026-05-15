// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * receiver.cpp — UDP receiver thread (thin infrastructure layer)
 *
 * Responsibilities: UDP socket, recv loop, decrypt, delegate to SessionService.
 * All business logic (connections, controllers, teardown) lives in SessionService.
 */
#include "receiver.h"
#include "crypto.h"
#include "core/session_service.h"
#include "adapters/client_adapter.h"

// ── Reaper: delegates timeout cleanup to SessionService ──────────────────────
static void reaperLoop(SessionService& svc) {
    while (g_appRunning && g_wantListen.load()) {
        netSleepMs(1000);
        svc.reapTimedOut();
    }
}

// ── Main receiver loop ──────────────────────────────────────────────────────
void receiverThread(SessionService& svc, ClientAdapter& client) {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    SetThreadAffinityMask(GetCurrentThread(), 1ULL);
#endif

    while (g_appRunning) {
        while (g_appRunning && !g_wantListen) { netSleepMs(50); }
        if (!g_appRunning) break;

        int port = g_config.udpPort;

        SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == INVALID_SOCKET) {
            logMsg(LogLevel::ERR, "receiver", "Failed to create UDP socket");
            g_wantListen = false;
            continue;
        }

        netDisableUdpConnReset(sock);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons((uint16_t)port);
        addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            logMsg(LogLevel::ERR, "receiver", "Failed to bind UDP port " + std::to_string(port));
            closesocket(sock);
            g_wantListen = false;
            continue;
        }

        client.setSocket(sock); // Give the client adapter the socket for sending

        netSetRecvTimeoutMs(sock, 10);
        int rcvBuf = 65536;
        setsockopt(sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvBuf),
                   sizeof(rcvBuf));
        int tos = 0xB8;
        setsockopt(sock, IPPROTO_IP, IP_TOS, reinterpret_cast<const char*>(&tos), sizeof(tos));

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
        std::thread reaper(reaperLoop, std::ref(svc));

        while (g_appRunning && g_wantListen) {
            sockaddr_in sender{};
            socklen_t slen = sizeof(sender);
            uint8_t buf[256];
            int n = (int)recvfrom(sock, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                                  reinterpret_cast<sockaddr*>(&sender), &slen);

            // Minimum packet: header(8) + inner_header(4) + tag(16) = 28 bytes
            if (n < HEADER_SIZE + INNER_HEADER_SIZE + AUTH_TAG_SIZE) continue;

            auto t0 = std::chrono::steady_clock::now();

            // Parse plaintext header
            uint32_t token = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
                             ((uint32_t)buf[2] << 8) | (uint32_t)buf[3];
            uint32_t counter = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) |
                               ((uint32_t)buf[6] << 8) | (uint32_t)buf[7];

            // Look up connection key for decryption (brief lock)
            uint8_t key[CRYPTO_KEY_SIZE];
            uint32_t lastCounter;
            if (!svc.getDecryptInfo(token, key, lastCounter)) continue;

            // Replay protection
            if (counter <= lastCounter && lastCounter != 0) {
                g_replayDrop.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            // Decrypt
            uint8_t* ciphertext = buf + HEADER_SIZE;
            auto ctLen = static_cast<size_t>(n - HEADER_SIZE);
            uint8_t plaintext[256];
            unsigned long long ptLen = 0;
            if (!decryptPacket(key, counter, token, ciphertext, ctLen, plaintext, &ptLen)) {
                g_decryptFail.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            // Update connection state post-decrypt (counter, timestamp, address)
            char clientIP[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &sender.sin_addr, clientIP, sizeof(clientIP));
            uint16_t clientPort = ntohs(sender.sin_port);
            svc.updatePostDecrypt(token, counter, clientIP, clientPort);

            // Parse inner message
            if (ptLen < (unsigned long long)INNER_HEADER_SIZE) continue;
            uint16_t msgType = ((uint16_t)plaintext[0] << 8) | (uint16_t)plaintext[1];
            uint16_t msgLen = ((uint16_t)plaintext[2] << 8) | (uint16_t)plaintext[3];
            if ((size_t)(INNER_HEADER_SIZE + msgLen) > ptLen) continue;
            uint8_t* payload = plaintext + INNER_HEADER_SIZE;

            switch (msgType) {
            case MSG_GAMEPAD_DATA: {
                if (msgLen < 13) break;
                uint8_t ctrlIdx = payload[0];
                GamepadReport report;
                memcpy(&report, payload + 1, sizeof(GamepadReport));

                bool ok = svc.handleGamepadData(token, ctrlIdx, report);

                auto t1 = std::chrono::steady_clock::now();
                uint64_t us =
                    (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0)
                        .count();
                g_lastLoopUs.store(us, std::memory_order_relaxed);
                uint64_t prev = g_maxLoopUs.load(std::memory_order_relaxed);
                while (us > prev &&
                       !g_maxLoopUs.compare_exchange_weak(prev, us, std::memory_order_relaxed)) {}

                if (ok) {
                    g_submitOk.fetch_add(1, std::memory_order_relaxed);
                } else {
                    g_submitFail.fetch_add(1, std::memory_order_relaxed);
                }
                break;
            }
            case MSG_HEARTBEAT_PING:
                svc.handleHeartbeat(token);
                break;

            case MSG_CONTROLLER_ADD: {
                if (msgLen < 1) break;
                uint8_t ctrlIdx = payload[0];
                svc.handleControllerAdd(token, ctrlIdx);
                break;
            }
            case MSG_CONTROLLER_REMOVE: {
                if (msgLen < 1) break;
                uint8_t ctrlIdx = payload[0];
                svc.handleControllerRemove(token, ctrlIdx);
                break;
            }
            case MSG_CONTROLLER_TYPE: {
                if (msgLen < 2) break;
                uint8_t ctrlIdx = payload[0];
                uint8_t ctrlType = payload[1];
                svc.handleControllerType(token, ctrlIdx, ctrlType);
                break;
            }
            case MSG_MOTION: {
                // Wire payload: ctrlIdx(1) + 6×i16(12) + u32(4) = 17 bytes
                if (msgLen < 17) break;
                uint8_t ctrlIdx = payload[0];
                MotionReport report;
                // The wire is little-endian fixed-point — same convention as
                // GamepadReport. memcpy mirrors handleGamepadData.
                memcpy(&report, payload + 1, sizeof(MotionReport));
                svc.handleMotionData(token, ctrlIdx, report);
                break;
            }
            case MSG_BATTERY: {
                // Wire payload: ctrlIdx(1) + level(1) + status(1) = 3 bytes
                if (msgLen < 3) break;
                uint8_t ctrlIdx = payload[0];
                BatteryReport report;
                report.level = payload[1];
                report.status = payload[2];
                svc.handleBatteryUpdate(token, ctrlIdx, report);
                break;
            }
            case MSG_TOUCHPAD: {
                // Wire payload: ctrlIdx(1) + flags(1) + finger0(1+2+2) +
                // finger1(1+2+2) = 12 bytes.
                if (msgLen < 12) break;
                uint8_t ctrlIdx = payload[0];
                uint8_t flags = payload[1];
                TouchpadReport report;
                report.finger0.active = (flags & 0x01) != 0;
                report.finger1.active = (flags & 0x02) != 0;
                report.buttonPressed = (flags & 0x04) != 0;
                report.finger0.trackingId = payload[2];
                report.finger0.x = static_cast<int16_t>(static_cast<uint16_t>(payload[3]) |
                                                        (static_cast<uint16_t>(payload[4]) << 8));
                report.finger0.y = static_cast<int16_t>(static_cast<uint16_t>(payload[5]) |
                                                        (static_cast<uint16_t>(payload[6]) << 8));
                report.finger1.trackingId = payload[7];
                report.finger1.x = static_cast<int16_t>(static_cast<uint16_t>(payload[8]) |
                                                        (static_cast<uint16_t>(payload[9]) << 8));
                report.finger1.y = static_cast<int16_t>(static_cast<uint16_t>(payload[10]) |
                                                        (static_cast<uint16_t>(payload[11]) << 8));
                svc.handleTouchpadData(token, ctrlIdx, report);
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
        closesocket(sock);
        client.setSocket(INVALID_SOCKET);

        // Tear down all connections via SessionService
        svc.closeAllSessions();

        reaper.join();
    }
}
