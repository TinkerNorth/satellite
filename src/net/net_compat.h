// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * net_compat.h — Cross-platform socket compatibility shim.
 *
 * Papers over the handful of differences between Winsock and BSD sockets
 * so that files under src/net/ can compile unchanged on Windows and POSIX
 * (macOS, Linux). Header-only: all helpers are `inline`.
 *
 * Rules:
 *   - On Windows, this header must be included before <windows.h> so
 *     that <winsock2.h> is seen first (avoiding the winsock1 auto-pull
 *     that <windows.h> performs otherwise).
 *   - POSIX builds get: SOCKET=int, INVALID_SOCKET=-1, SOCKET_ERROR=-1,
 *     and an inline `closesocket()` that calls ::close().
 */
#pragma once

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <cerrno>
#endif

#include <chrono>
#include <cstddef>
#include <thread>

#ifndef _WIN32
using SOCKET = int;
#ifndef INVALID_SOCKET
constexpr int INVALID_SOCKET = -1;
#endif
#ifndef SOCKET_ERROR
constexpr int SOCKET_ERROR = -1;
#endif

inline int closesocket(SOCKET s) { return ::close(s); }
#endif

// ── Process-wide network subsystem init/shutdown ─────────────────────────────
// WSAStartup/WSACleanup on Windows; no-op elsewhere.
inline bool netInit() {
#ifdef _WIN32
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
    return true;
#endif
}

inline void netShutdown() {
#ifdef _WIN32
    WSACleanup();
#endif
}

// ── Per-socket helpers ──────────────────────────────────────────────────────
inline bool netSetNonBlocking(SOCKET s, bool nonblock) {
#ifdef _WIN32
    u_long v = nonblock ? 1 : 0;
    return ioctlsocket(s, FIONBIO, &v) == 0;
#else
    int flags = ::fcntl(s, F_GETFL, 0);
    if (flags < 0) return false;
    flags = nonblock ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return ::fcntl(s, F_SETFL, flags) == 0;
#endif
}

inline void netSetRecvTimeoutMs(SOCKET s, unsigned ms) {
#ifdef _WIN32
    DWORD t = ms;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
#else
    timeval tv;
    tv.tv_sec = static_cast<time_t>(ms / 1000);
    tv.tv_usec = static_cast<suseconds_t>((ms % 1000) * 1000);
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

// Suppress Winsock's "connection reset" errors on UDP recvfrom following an
// ICMP Port Unreachable reply from a downed peer. No-op on POSIX, where
// recvfrom() is not affected by that quirk.
inline void netDisableUdpConnReset(SOCKET s) {
#ifdef _WIN32
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
    BOOL bNewBehavior = FALSE;
    DWORD dwBytesReturned = 0;
    ::WSAIoctl(s, SIO_UDP_CONNRESET, &bNewBehavior, sizeof(bNewBehavior), nullptr, 0,
               &dwBytesReturned, nullptr, nullptr);
#else
    (void)s;
#endif
}

inline void netSleepMs(unsigned ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// Fill `buf` with this host's short name. Returns true on success.
inline bool netGetHostname(char* buf, std::size_t len) {
#ifdef _WIN32
    DWORD sz = static_cast<DWORD>(len);
    return ::GetComputerNameA(buf, &sz) != 0;
#else
    return ::gethostname(buf, len) == 0;
#endif
}
