// SPDX-License-Identifier: LGPL-3.0-or-later
// The one place the wire's byte order is spelled out: big-endian for the
// packet headers, the audio sequence and DNS; little-endian for the pad
// reports.
#include "../src/core/byte_order.h"

#include <cstdint>
#include <iostream>

#include "test_util.h"

using satellite::readBE16;
using satellite::readBE32;
using satellite::readLE16;
using satellite::readLE32;
using satellite::writeBE16;
using satellite::writeBE32;

int main() {
    const uint8_t bytes[4] = {0x01, 0x02, 0x03, 0x04};
    {
        TEST("big-endian reads take the first byte as the most significant");
        EXPECT_EQ(readBE16(bytes), 0x0102u);
        EXPECT_EQ(readBE32(bytes), 0x01020304u);
    }
    {
        TEST("little-endian reads take the first byte as the least significant");
        EXPECT_EQ(readLE16(bytes), 0x0201u);
        EXPECT_EQ(readLE32(bytes), 0x04030201u);
    }
    {
        TEST("a write reads back as itself, high bit and all");
        for (const uint32_t v : {0u, 1u, 0x8000u, 0xFFFFu, 0x80000000u, 0xDEADBEEFu, 0xFFFFFFFFu}) {
            uint8_t out[4] = {};
            writeBE32(out, v);
            EXPECT_EQ(readBE32(out), v);
            if (v <= 0xFFFFu) {
                writeBE16(out, static_cast<uint16_t>(v));
                EXPECT_EQ(readBE16(out), static_cast<uint16_t>(v));
            }
        }
    }
    {
        TEST("a 16-bit write touches exactly two bytes");
        uint8_t out[4] = {0xAA, 0xAA, 0xAA, 0xAA};
        writeBE16(out, 0x1234);
        EXPECT_EQ(out[0], 0x12);
        EXPECT_EQ(out[1], 0x34);
        EXPECT_EQ(out[2], 0xAA);
        EXPECT_EQ(out[3], 0xAA);
    }
    {
        TEST("a 32-bit write lays the bytes out most significant first");
        uint8_t out[4] = {};
        writeBE32(out, 0xCAFEBABE);
        EXPECT_EQ(out[0], 0xCA);
        EXPECT_EQ(out[1], 0xFE);
        EXPECT_EQ(out[2], 0xBA);
        EXPECT_EQ(out[3], 0xBE);
    }

    std::cout << "test_byte_order: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
