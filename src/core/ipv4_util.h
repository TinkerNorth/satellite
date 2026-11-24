// SPDX-License-Identifier: LGPL-3.0-or-later

// Deliberately winsock-free so the portable test target links without ws2_32.
// Both helpers use NETWORK byte order, matching sockaddr_in::sin_addr::s_addr,
// so the uint32 from recvfrom passes straight through. Hot path (one call per
// UDP packet), so kept tiny and allocation-free.
#pragma once

#include "core/bits.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace satellite {

// Parse "a.b.c.d" into NETWORK-byte-order uint32; 0 on any malformed input.
// 0 is ambiguous with "0.0.0.0" but that's not a meaningful sender IP; the
// connection reads as "address unknown" until the next packet refreshes it.
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
    // Network byte order: leftmost octet in the low byte, so a memcpy into
    // sockaddr_in.sin_addr matches what inet_pton would have written.
    return static_cast<uint32_t>(parts[0]) | (static_cast<uint32_t>(parts[1]) << 8U) |
           (static_cast<uint32_t>(parts[2]) << 16U) | (static_cast<uint32_t>(parts[3]) << 24U);
}

// Format a NETWORK-byte-order uint32 as "a.b.c.d". <=15 chars, so SSO means no
// heap touch.
inline std::string formatIPv4Nbo(uint32_t nbo) {
    const uint8_t a = bits::byteOf(nbo, 0U);
    const uint8_t b = bits::byteOf(nbo, 1U);
    const uint8_t c = bits::byteOf(nbo, 2U);
    const uint8_t d = bits::byteOf(nbo, 3U);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", a, b, c, d);
    return std::string(buf);
}

} // namespace satellite
