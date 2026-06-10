// SPDX-License-Identifier: LGPL-3.0-or-later

// Hot loop is allocation-free and single-lock per gamepad packet — keep it that way.
#include "receiver.h"
#include "inner_dispatch.h"
#include "crypto.h"
#include "session_crypto.h"
#include "core/session_service.h"
#include "adapters/client_adapter.h"

#ifdef _WIN32
#include <avrt.h> // MMCSS: AvSetMmThreadCharacteristics for the RX thread
#endif

// dispatchInnerMessage (inner-message parser + length guards) lives in
// net/inner_dispatch.cpp — socket-free so the guards can be unit tested.

static void reaperLoop(SessionService& svc) {
    while (g_appRunning) {
        netSleepMs(1000);
        svc.reapTimedOut();
    }
}

void receiverThread(SessionService& svc, ClientAdapter& client) {
#ifdef _WIN32
    // Register with MMCSS (Multimedia Class Scheduler) under the "Games" task —
    // the OS-sanctioned low-latency scheduling class. It boosts this thread
    // while registered and, unlike a hand-set TIME_CRITICAL priority, lets the
    // scheduler manage it so it can't starve the rest of the system. Fall back
    // to TIME_CRITICAL only if MMCSS is unavailable (service disabled or the
    // "Games" task profile missing). Reverted at thread exit.
    DWORD mmcssTaskIndex = 0;
    HANDLE mmcssHandle = AvSetMmThreadCharacteristicsW(L"Games", &mmcssTaskIndex);
    if (mmcssHandle == nullptr) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    }
    SetThreadAffinityMask(GetCurrentThread(), 1ULL);
#endif

    // Outer loop exists only to re-bind: on a socket/bind failure, log once,
    // wait, and retry until the UDP port is available.
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

        client.setSocket(sock);

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

        std::thread reaper(reaperLoop, std::ref(svc));

        // Per-thread high-water-mark: skip the cross-thread CAS on g_maxLoopUs
        // on the ~99% of packets below the running peak. The atomic still
        // reflects the global max since every thread raising its own mark
        // pushes the atomic up.
        uint64_t localMaxUs = 0;

        while (g_appRunning) {
            sockaddr_in sender{};
            socklen_t slen = sizeof(sender);
            uint8_t buf[256];
            int n = (int)recvfrom(sock, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                                  reinterpret_cast<sockaddr*>(&sender), &slen);

            // Minimum packet: header(8) + inner_header(4) + tag(16) = 28 bytes
            if (n < HEADER_SIZE + INNER_HEADER_SIZE + AUTH_TAG_SIZE) continue;

            auto t0 = std::chrono::steady_clock::now();

            uint32_t token = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
                             ((uint32_t)buf[2] << 8) | (uint32_t)buf[3];
            uint32_t counter = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) |
                               ((uint32_t)buf[6] << 8) | (uint32_t)buf[7];

            // Look up connection key (brief lock).
            uint8_t key[CRYPTO_KEY_SIZE];
            uint32_t lastCounter;
            if (!svc.getDecryptInfo(token, key, lastCounter)) continue;

            // Replay protection.
            if (counter <= lastCounter && lastCounter != 0) {
                g_replayDrop.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            // In-place decrypt (plaintext = ctLen - 16 bytes, fits the
            // ciphertext span): libsodium chacha20-poly1305 supports `m == c`
            // overlap, saving a second 256-byte stack buffer.
            uint8_t* ciphertext = buf + HEADER_SIZE;
            auto ctLen = static_cast<size_t>(n - HEADER_SIZE);
            unsigned long long ptLen = 0;
            if (!decryptPacket(key, CRYPTO_DIR_CLIENT_TO_SERVER, counter, token, ciphertext, ctLen,
                               ciphertext, &ptLen)) {
                g_decryptFail.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            uint8_t* plaintext = ciphertext; // alias after in-place decrypt

            // Sender address as uint32 (network byte order) — no inet_ntop, no
            // std::string alloc on the hot path. SessionService refreshes the
            // human-readable cache only when this value changes.
            const uint32_t senderIPv4 = sender.sin_addr.s_addr;
            const uint16_t senderPort = ntohs(sender.sin_port);

            if (ptLen < (unsigned long long)INNER_HEADER_SIZE) continue;
            uint16_t msgType = ((uint16_t)plaintext[0] << 8) | (uint16_t)plaintext[1];
            uint16_t msgLen = ((uint16_t)plaintext[2] << 8) | (uint16_t)plaintext[3];
            if ((size_t)(INNER_HEADER_SIZE + msgLen) > ptLen) continue;
            uint8_t* payload = plaintext + INNER_HEADER_SIZE;

            // Fast path: MSG_GAMEPAD_DATA hits the fused single-lock entry.
            // Every other kind takes the cold two-lock path — sub-Hz, so the
            // extra acquire is invisible.
            DispatchResult dr;
            if (msgType == MSG_GAMEPAD_DATA && msgLen >= 13) {
                uint8_t ctrlIdx = payload[0];
                GamepadReport report;
                std::memcpy(&report, payload + 1, sizeof(GamepadReport));
                dr.wasGamepadData = true;
                dr.gamepadOk = svc.handleGamepadDataAndUpdate(token, counter, senderIPv4,
                                                              senderPort, ctrlIdx, report);
            } else {
                svc.updatePostDecryptV4(token, counter, senderIPv4, senderPort);
                dr = dispatchInnerMessage(svc, token, msgType, payload, msgLen);
            }

            // Hot path only: record loop latency + submit-outcome telemetry.
            if (dr.wasGamepadData) {
                auto t1 = std::chrono::steady_clock::now();
                uint64_t us =
                    (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0)
                        .count();
                g_lastLoopUs.store(us, std::memory_order_relaxed);
                // Touch the cross-thread CAS only when we beat our own record.
                if (us > localMaxUs) {
                    localMaxUs = us;
                    uint64_t prev = g_maxLoopUs.load(std::memory_order_relaxed);
                    while (us > prev && !g_maxLoopUs.compare_exchange_weak(
                                            prev, us, std::memory_order_relaxed)) {}
                }

                if (dr.gamepadOk) {
                    g_submitOk.fetch_add(1, std::memory_order_relaxed);
                } else {
                    g_submitFail.fetch_add(1, std::memory_order_relaxed);
                }
            }

            g_packetCount.fetch_add(1, std::memory_order_relaxed);

            if ((g_packetCount.load(std::memory_order_relaxed) & 0xFF) == 0) {
                g_senderIP.store(senderIPv4);
            }
        }

        g_listening = false;

        // Close sessions while the socket is still usable so the best-effort
        // close-notify (reason=shutdown) can ride out before teardown.
        svc.closeAllSessions(CLOSE_REASON_SHUTDOWN);

        closesocket(sock);
        client.setSocket(INVALID_SOCKET);

        reaper.join();
    }

#ifdef _WIN32
    if (mmcssHandle != nullptr) AvRevertMmThreadCharacteristics(mmcssHandle);
#endif
}
