// SPDX-License-Identifier: LGPL-3.0-or-later

// The MSG_TOUCHPAD wire coordinate is a centre-origin, resolution-independent
// int16 (-32768 = pad left/top edge, 0 = centre, +32767 = right/bottom edge,
// +x = right, +y = down). Each backend scales it into its device's space.
#pragma once

#include "types.h"

#include <array>
#include <cstdint>

// DS4 hardware touchpad resolution (native units; packed 12-bit). DualSense is
// physically larger but DS4 emulation uses the DS4 figures.
inline const int DS4_TOUCHPAD_RES_X = 1920;
inline const int DS4_TOUCHPAD_RES_Y = 943;

// Map a centre-origin wire coordinate (-32768..32767) to a 0-based device
// coordinate in [0, res-1]. Saturating: out-of-range clamps to the edge.
inline int touchpadWireToRange(int16_t v, int res) {
    if (res <= 1) return 0;
    const int u = static_cast<int>(v) + 32768; // 0..65535
    long long scaled = (static_cast<long long>(u) * res) / 65536;
    if (scaled < 0) return 0;
    if (scaled > res - 1) return res - 1;
    return static_cast<int>(scaled);
}

// Encode one finger into the DS4_TOUCH slot layout (bIsUpTrackingNumN +
// bTouchDataN[3]):
//   [0] tracking byte: bit7 = finger lifted, bits0..6 = tracking id
//   [1..3] two packed 12-bit coordinates (x then y), DS4 native packing
// Plain array so the layout is unit-testable without the ViGEm headers.
inline std::array<uint8_t, 4> ds4PackTouchFinger(const TouchpadFinger& f, uint8_t trackingId) {
    const int x = touchpadWireToRange(f.x, DS4_TOUCHPAD_RES_X);
    const int y = touchpadWireToRange(f.y, DS4_TOUCHPAD_RES_Y);
    std::array<uint8_t, 4> out{};
    out[0] = static_cast<uint8_t>((f.active ? 0x00 : 0x80) | (trackingId & 0x7F));
    out[1] = static_cast<uint8_t>(x & 0xFF);
    out[2] = static_cast<uint8_t>(((y & 0x0F) << 4) | ((x >> 8) & 0x0F));
    out[3] = static_cast<uint8_t>((y >> 4) & 0xFF);
    return out;
}
