// SPDX-License-Identifier: LGPL-3.0-or-later

// MSG_TOUCHPAD wire coordinate: centre-origin, resolution-independent int16
// (-32768 = left/top edge, 0 = centre, +32767 = right/bottom; +x right, +y down).
#pragma once

#include "core/bits.h"
#include "types.h"

#include <array>
#include <cstdint>

// DS4 hardware touchpad resolution. DualSense is larger but DS4 emulation uses
// the DS4 figures.
inline const int DS4_TOUCHPAD_RES_X = 1920;
inline const int DS4_TOUCHPAD_RES_Y = 943;

inline int touchpadWireToRange(int16_t v, int res) {
    if (res <= 1) return 0;
    const int u = static_cast<int>(v) + 32768; // 0..65535
    long long scaled = (static_cast<long long>(u) * res) / 65536;
    if (scaled < 0) return 0;
    if (scaled > res - 1) return res - 1;
    return static_cast<int>(scaled);
}

// DS4_TOUCH slot layout: [0] = bit7 finger lifted, bits0..6 tracking id;
// [1..3] two packed 12-bit coords (x then y). Plain array so it's testable
// without the ViGEm headers.
inline std::array<uint8_t, 4> ds4PackTouchFinger(const TouchpadFinger& f, uint8_t trackingId) {
    const int x = touchpadWireToRange(f.x, DS4_TOUCHPAD_RES_X);
    const int y = touchpadWireToRange(f.y, DS4_TOUCHPAD_RES_Y);
    std::array<uint8_t, 4> out{};
    // touchpadWireToRange keeps both in 0..res-1, so neither has a sign bit to carry.
    const uint32_t ux = static_cast<uint32_t>(x);
    const uint32_t uy = static_cast<uint32_t>(y);
    out[0] = satellite::bits::lowByte((f.active ? 0x00U : 0x80U) |
                                      (satellite::bits::u32(trackingId) & 0x7FU));
    out[1] = satellite::bits::lowByte(ux);
    out[2] = satellite::bits::lowByte(((uy & 0x0FU) << 4U) | ((ux >> 8U) & 0x0FU));
    out[3] = satellite::bits::lowByte(uy >> 4U);
    return out;
}
