// SPDX-License-Identifier: LGPL-3.0-or-later

// Hot loop is allocation-free and single-lock per gamepad packet; keep it so.
#include "receiver.h"
#include "inner_dispatch.h"
#include "crypto.h"
#include "session_crypto.h"
#include "core/byte_order.h"
#include "core/session_service.h"
#include "adapters/client_adapter.h"
#include "app/wire_stats.h"

#include <chrono>
#include <cstring>
#include <thread>

#ifdef _WIN32
#include <avrt.h> // MMCSS: AvSetMmThreadCharacteristics for the RX thread
#endif

using satellite::g_wire;
using satellite::readBE32;

namespace {

// Minimum packet: header(8) + inner_header(4) + tag(16) = 28 bytes.
constexpr int MIN_DATAGRAM_BYTES = HEADER_SIZE + INNER_HEADER_SIZE + AUTH_TAG_SIZE;
constexpr int REBIND_RETRY_MS = 1000;
constexpr int RECV_TIMEOUT_MS = 10;
constexpr int RECV_BUFFER_BYTES = 65536;
// DSCP EF (46) << 2: the lowest-latency class a router honours.
constexpr int TOS_EXPEDITED = 0xB8;
// The gamepad fast path needs ctrlIdx plus a whole report.
constexpr uint16_t GAMEPAD_DATA_MIN_LEN = 13;

// Session maintenance off the hot path: the motor-state tick every
// RUMBLE_REFRESH_MS / 2 (so a held level is re-sent within one refresh
// interval of becoming due), the idle-input re-publish on the same tick, the
// reaper once a second.
void maintenanceLoop(SessionService& svc) {
    const int tickMs = RUMBLE_REFRESH_MS / 2;
    int sinceReapMs = 0;
    while (g_appRunning) {
        netSleepMs(tickMs);
        svc.refreshRumble();
        svc.refreshIdleInput();
        sinceReapMs += tickMs;
        if (sinceReapMs < 1000) continue;
        sinceReapMs = 0;
        const int reaped = svc.reapTimedOut();
        if (reaped > 0) {
            g_wire.sessionsReaped.fetch_add(static_cast<uint64_t>(reaped),
                                            std::memory_order_relaxed);
        }
    }
}

#ifdef _WIN32
// Register with MMCSS under the "Games" task, the OS-sanctioned low-latency
// scheduling class. Unlike a hand-set TIME_CRITICAL priority, the scheduler
// manages it so it can't starve the rest of the system. Fall back to
// TIME_CRITICAL only if MMCSS is unavailable. Reverted at thread exit.
HANDLE raiseReceiverPriority() {
    DWORD mmcssTaskIndex = 0;
    HANDLE mmcssHandle = AvSetMmThreadCharacteristicsW(L"Games", &mmcssTaskIndex);
    if (mmcssHandle == nullptr) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    }
    SetThreadAffinityMask(GetCurrentThread(), 1ULL);
    return mmcssHandle;
}

void restoreReceiverPriority(HANDLE mmcssHandle) {
    if (mmcssHandle != nullptr) AvRevertMmThreadCharacteristics(mmcssHandle);
}
#endif

int configuredUdpPort() {
    std::lock_guard<std::mutex> lk(g_configMtx);
    return g_config.udpPort;
}

// A bound, tuned receive socket, or INVALID_SOCKET with the failure logged
// once per outage; the caller retries until the port is free.
SOCKET openReceiverSocket(int port, bool& bindErrorLogged) {
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        if (!bindErrorLogged) {
            logMsg(LogLevel::ERR, "receiver", "Failed to create UDP socket; retrying");
            bindErrorLogged = true;
        }
        return INVALID_SOCKET;
    }
    netDisableUdpConnReset(sock);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        if (!bindErrorLogged) {
            logMsg(LogLevel::ERR, "receiver",
                   "Failed to bind UDP port " + std::to_string(port) + "; retrying");
            bindErrorLogged = true;
        }
        closesocket(sock);
        return INVALID_SOCKET;
    }
    bindErrorLogged = false;

    netSetRecvTimeoutMs(sock, RECV_TIMEOUT_MS);
    const int rcvBuf = RECV_BUFFER_BYTES;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvBuf), sizeof(rcvBuf));
    const int tos = TOS_EXPEDITED;
    setsockopt(sock, IPPROTO_IP, IP_TOS, reinterpret_cast<const char*>(&tos), sizeof(tos));
    return sock;
}

// The dashboard's counters start over with every bind.
void resetReceiverCounters() {
    g_packetCount.store(0, std::memory_order_relaxed);
    g_submitOk.store(0, std::memory_order_relaxed);
    g_submitFail.store(0, std::memory_order_relaxed);
    g_lastLoopUs.store(0, std::memory_order_relaxed);
    g_maxLoopUs.store(0, std::memory_order_relaxed);
    g_decryptFail.store(0, std::memory_order_relaxed);
    g_replayDrop.store(0, std::memory_order_relaxed);
    g_senderIP.store(0);
    g_wire.reset();
}

// Hot path only: loop latency and the submit outcome, gamepad frames alone.
// The high-water mark is per-thread: the cross-thread CAS on g_maxLoopUs is
// skipped on the ~99% of packets below the running peak. The atomic still
// reflects the global max since every thread raising its own mark pushes it.
inline void recordGamepadLoop(std::chrono::steady_clock::time_point t0, bool ok,
                              uint64_t& localMaxUs) {
    const auto t1 = std::chrono::steady_clock::now();
    const uint64_t us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
    g_lastLoopUs.store(us, std::memory_order_relaxed);
    if (us > localMaxUs) {
        localMaxUs = us;
        uint64_t prev = g_maxLoopUs.load(std::memory_order_relaxed);
        while (us > prev &&
               !g_maxLoopUs.compare_exchange_weak(prev, us, std::memory_order_relaxed)) {}
    }
    if (ok) {
        g_submitOk.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_submitFail.fetch_add(1, std::memory_order_relaxed);
    }
}

// One datagram, in place: the outer header names the session and the counter,
// the payload is decrypted where it landed, and the inner header names the
// message. Every drop counts once. One function on purpose: this is the hot
// path, and its steps read best in the order the bytes are consumed.
inline void handleDatagram(SessionService& svc, uint8_t* buf, int n, const sockaddr_in& sender,
                           uint64_t& localMaxUs) {
    const auto t0 = std::chrono::steady_clock::now();
    const uint32_t token = readBE32(buf);
    const uint32_t counter = readBE32(buf + 4);

    // Look up connection key (brief lock).
    uint8_t key[CRYPTO_KEY_SIZE];
    uint32_t lastCounter;
    bool seenCounter = false;
    if (!svc.getDecryptInfo(token, key, lastCounter, &seenCounter)) {
        g_wire.rxUnknownToken.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Replay protection.
    if (seenCounter && counter <= lastCounter) {
        g_replayDrop.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // In-place decrypt: libsodium chacha20-poly1305 supports `m == c`
    // overlap, saving a second datagram-sized stack buffer.
    uint8_t* plaintext = buf + HEADER_SIZE;
    const auto ctLen = static_cast<size_t>(n - HEADER_SIZE);
    unsigned long long ptLen = 0;
    if (!decryptPacket(key, CRYPTO_DIR_CLIENT_TO_SERVER, counter, token, plaintext, ctLen,
                       plaintext, &ptLen)) {
        g_decryptFail.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Sender address as uint32 (network byte order): no inet_ntop, no
    // std::string alloc on the hot path. SessionService refreshes the
    // human-readable cache only when this value changes.
    const uint32_t senderIPv4 = sender.sin_addr.s_addr;
    const uint16_t senderPort = ntohs(sender.sin_port);

    InnerHeader inner;
    if (!parseInnerHeader(plaintext, static_cast<size_t>(ptLen), inner)) {
        g_wire.rxMalformed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint8_t* payload = plaintext + INNER_HEADER_SIZE;

    // Fast path: MSG_GAMEPAD_DATA hits the fused single-lock entry.
    // Every other kind takes the cold two-lock path (sub-Hz, so the
    // extra acquire is invisible).
    DispatchResult dr;
    if (inner.msgType == MSG_GAMEPAD_DATA && inner.msgLen >= GAMEPAD_DATA_MIN_LEN) {
        const uint8_t ctrlIdx = payload[0];
        GamepadReport report;
        std::memcpy(&report, payload + 1, sizeof(GamepadReport));
        dr.wasGamepadData = true;
        dr.gamepadOk =
            svc.handleGamepadDataAndUpdate(token, counter, senderIPv4, senderPort, ctrlIdx, report);
    } else {
        svc.updatePostDecryptV4(token, counter, senderIPv4, senderPort);
        dr = dispatchInnerMessage(svc, token, inner.msgType, payload, inner.msgLen);
        g_wire.recordInbound(inner.msgType, dr.handled);
    }
    if (dr.wasGamepadData) recordGamepadLoop(t0, dr.gamepadOk, localMaxUs);

    g_packetCount.fetch_add(1, std::memory_order_relaxed);
    if ((g_packetCount.load(std::memory_order_relaxed) & 0xFF) == 0) {
        g_senderIP.store(senderIPv4);
    }
}

// Receives until the app stops. One MTU-sized datagram per read (header +
// ciphertext + tag): it used to be 256, which fit every control message; the
// audio streams need room for a 20 ms Opus packet, and a short read would
// truncate the tag and fail the AEAD rather than degrade.
void receiveLoop(SessionService& svc, SOCKET sock) {
    uint64_t localMaxUs = 0;
    while (g_appRunning) {
        sockaddr_in sender{};
        socklen_t slen = sizeof(sender);
        uint8_t buf[UDP_DATAGRAM_MAX_BYTES];
        const int n = static_cast<int>(recvfrom(sock, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                                                reinterpret_cast<sockaddr*>(&sender), &slen));
        if (n < MIN_DATAGRAM_BYTES) {
            // A timeout or a transient error reads as -1 and is not a runt.
            if (n >= 0) g_wire.rxRunt.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        handleDatagram(svc, buf, n, sender, localMaxUs);
    }
}

// Close sessions while the socket is still usable so the best-effort
// close-notify (reason=shutdown) can ride out before teardown.
void stopListening(SessionService& svc, ClientAdapter& client, SOCKET sock, std::thread& reaper) {
    g_listening = false;
    svc.closeAllSessions(CLOSE_REASON_SHUTDOWN);
    client.setSocket(INVALID_SOCKET);
    closesocket(sock);
    reaper.join();
}

} // namespace

void receiverThread(SessionService& svc, ClientAdapter& client) {
#ifdef _WIN32
    HANDLE mmcssHandle = raiseReceiverPriority();
#endif

    // Outer loop exists only to re-bind: on a socket/bind failure, log once,
    // wait, and retry until the UDP port is available.
    bool bindErrorLogged = false;
    while (g_appRunning) {
        const int port = configuredUdpPort();
        SOCKET sock = openReceiverSocket(port, bindErrorLogged);
        if (sock == INVALID_SOCKET) {
            netSleepMs(REBIND_RETRY_MS);
            continue;
        }
        client.setSocket(sock);
        logMsg(LogLevel::INFO, "receiver", "Listening on UDP port " + std::to_string(port));
        g_listening = true;
        resetReceiverCounters();

        std::thread reaper(maintenanceLoop, std::ref(svc));
        receiveLoop(svc, sock);
        stopListening(svc, client, sock, reaper);
    }

#ifdef _WIN32
    restoreReceiverPriority(mmcssHandle);
#endif
}
