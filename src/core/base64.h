// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

// Standard base64 (RFC 4648, with padding), the encoding the key files carry
// on every platform. Decoding stops at the first character outside the
// alphabet, so the padding ends it.

#include <cstdint>
#include <string>
#include <vector>

#include "core/bits.h"

namespace satellite {

inline constexpr char BASE64_DIGITS[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

inline std::string base64Encode(const std::vector<uint8_t>& data) {
    std::string out;
    // Only the low 14 bits of the accumulator are ever read, so it may wrap.
    uint32_t val = 0;
    int pending = -6;
    for (uint8_t c : data) {
        val = (val << 8U) | bits::u32(c);
        pending += 8;
        while (pending >= 0) {
            out.push_back(BASE64_DIGITS[(val >> static_cast<unsigned>(pending)) & 0x3FU]);
            pending -= 6;
        }
    }
    if (pending > -6) {
        out.push_back(BASE64_DIGITS[((val << 8U) >> static_cast<unsigned>(pending + 8)) & 0x3FU]);
    }
    while ((out.size() % 4) != 0U) out.push_back('=');
    return out;
}

inline std::vector<uint8_t> base64Decode(const std::string& s) {
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; i++) T[(int)BASE64_DIGITS[i]] = i;
    std::vector<uint8_t> out;
    // Only the low 12 bits of the accumulator are ever read, so it may wrap.
    uint32_t val = 0;
    int pending = -8;
    for (char c : s) {
        if (T[(unsigned char)c] == -1) break;
        val = (val << 6U) | static_cast<uint32_t>(T[(unsigned char)c]);
        pending += 6;
        if (pending >= 0) {
            out.push_back(bits::lowByte(val >> static_cast<unsigned>(pending)));
            pending -= 8;
        }
    }
    return out;
}

} // namespace satellite
