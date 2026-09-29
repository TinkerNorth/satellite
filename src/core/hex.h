// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Lowercase, high nibble first: the one spelling of every key, salt, digest, id
// and MAC address Satellite writes as text.
inline constexpr char HEX_DIGITS[] = "0123456789abcdef";

inline void appendHexByte(std::string& out, uint8_t byte) {
    out.push_back(HEX_DIGITS[byte >> 4]);
    out.push_back(HEX_DIGITS[byte & 0x0F]);
}

inline std::string hexEncode(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; i++) appendHexByte(out, data[i]);
    return out;
}

// The form a MAC address is written in: "02:53:41:54:00:0c" with ':'.
inline std::string hexEncodeSeparated(const uint8_t* data, size_t len, char separator) {
    std::string out;
    out.reserve(len * 3);
    for (size_t i = 0; i < len; i++) {
        if (i != 0) out.push_back(separator);
        appendHexByte(out, data[i]);
    }
    return out;
}
