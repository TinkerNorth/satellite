// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * receiver.cpp — UDP receiver thread (thin infrastructure layer)
 *
 * Responsibilities: UDP socket, recv loop, decrypt, delegate to SessionService.
 * All business logic (connections, controllers, teardown) lives in SessionService.
 */
#include "receiver.h"
#include "inner_dispatch.h"
#include "crypto.h"
#include "core/session_service.h"
#include "adapters/client_adapter.h"

// dispatchInnerMessage (the decrypted inner-message parser + length guards)
// lives in net/inner_dispatch.cpp — a portable, socket-free TU so the guards
// can be unit tested with raw byte buffers (tests/test_receiver.cpp).

// ── Reaper: delegates timeout cleanup to SessionService ──────────────────────
static void reaperLoop(SessionService& svc) {
    while (g_appRunning) {
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

    // The receiver runs for the whole app lifetime — there is no start/stop.
    // The outer loop exists only to re-bind: on a socket/bind failure it logs
    // once, waits, and retries until the UDP port becomes available.
    bool bindErrorLogged = false;

    while (g_appRunning) {
        int port = g_config.udpPort;

        SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == INVALID_SOCKET) {
            if (!bindErrorLogged) {
                logMsg(LogLevel::ERR, "receiver", "Failed to create UDP socket — retrying");
                bindErrorLogged = true;
            }
            netSleepMs(1000);
            continue;
        }

        netDisableUdpConnReset(sock);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons((uint16_t)port);
        addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            if (!bindErrorLogged) {
                logMsg(LogLevel::ERR, "receiver",
                       "Failed to bind UDP port " + std::to_string(port) + " — retrying");
                bindErrorLogged = true;
            }
            closesocket(sock);
            netSleepMs(1000);
            continue;
        }
        bindErrorLogged = false;

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

        while (g_appRunning) {
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

            DispatchResult dr = dispatchInnerMessage(svc, token, msgType, payload, msgLen);

            // Gamepad data is the hot path — record loop latency + submit
            // outcome telemetry. Other message types skip this block.
            if (dr.wasGamepadData) {
                auto t1 = std::chrono::steady_clock::now();
                uint64_t us =
                    (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0)
                        .count();
                g_lastLoopUs.store(us, std::memory_order_relaxed);
                uint64_t prev = g_maxLoopUs.load(std::memory_order_relaxed);
                while (us > prev &&
                       !g_maxLoopUs.compare_exchange_weak(prev, us, std::memory_order_relaxed)) {}

                if (dr.gamepadOk) {
                    g_submitOk.fetch_add(1, std::memory_order_relaxed);
                } else {
                    g_submitFail.fetch_add(1, std::memory_order_relaxed);
                }
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
