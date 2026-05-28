// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * core/ipv4_util.h -- Pure-C++ IPv4 dotted-quad <-> uint32 codec.
 *
 * Header-only and deliberately winsock-free so the portable test target
 * can link without ws2_32 (otherwise the cross-platform test suite would
 * have to drag a socket library in just to format an IP).
 *
 * Both helpers operate in NETWORK byte order, matching the layout of
 * `sockaddr_in::sin_addr::s_addr` -- this is what receiver.cpp already
 * has in hand after recvfrom, so passing the uint32 straight through
 * skips the per-packet inet_ntop + std::string allocation the previous
 * "format on every packet" path paid.
 *
 * The hot path uses these via SessionService::handleGamepadDataAndUpdate
 * (one call per UDP packet at ~250 Hz), so they are intentionally tiny
 * and allocation-free in the common case.
 */
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace satellite {

// Parse "a.b.c.d" into uint32_t in NETWORK byte order. Returns 0 on any
// malformed input (empty string, non-digit, octet > 255, wrong number of
// parts). Note: 0 is ambiguous with the literal address "0.0.0.0", but
// that's not a meaningful sender IP for our use case -- the connection
// just behaves as "address unknown until next packet" until the V4 hot
// path refreshes it.
inline uint32_t parseIPv4Nbo(const std::string& s) {
    uint8_t parts[4] = {0, 0, 0, 0};
    int idx = 0;
    int val = 0;
    bool any = false;
    for (char c : s) {
        if (c >= '0' && c <= '9') {
            val = val * 10 + (c - '0');
            if (val > 255) return 0;
            any = true;
        } else if (c == '.') {
            if (!any || idx >= 3) return 0;
            parts[idx++] = static_cast<uint8_t>(val);
            val = 0;
            any = false;
        } else {
            return 0;
        }
    }
    if (!any || idx != 3) return 0;
    parts[idx] = static_cast<uint8_t>(val);
    // Network byte order: leftmost octet sits in the low byte of the
    // uint32, so a memcpy into sockaddr_in.sin_addr matches what
    // inet_pton would have written.
    return static_cast<uint32_t>(parts[0]) | (static_cast<uint32_t>(parts[1]) << 8) |
           (static_cast<uint32_t>(parts[2]) << 16) | (static_cast<uint32_t>(parts[3]) << 24);
}

// Format a NETWORK-byte-order uint32 as "a.b.c.d". Always succeeds;
// returned string is at most 15 chars (small-string-optimised on every
// modern std::string impl, so no heap touch in steady state).
inline std::string formatIPv4Nbo(uint32_t nbo) {
    const uint8_t a = static_cast<uint8_t>(nbo & 0xFF);
    const uint8_t b = static_cast<uint8_t>((nbo >> 8) & 0xFF);
    const uint8_t c = static_cast<uint8_t>((nbo >> 16) & 0xFF);
    const uint8_t d = static_cast<uint8_t>((nbo >> 24) & 0xFF);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", a, b, c, d);
    return std::string(buf);
}

} // namespace satellite
