// SPDX-License-Identifier: LGPL-3.0-or-later
//
// The byte-level assembly the wire codecs and report packers rest on, at the
// values a wrong shift, mask or sign extension gets wrong: 0x00, 0x7F, 0x80
// and 0xFF in each shifted position, the sign bit of a 16-bit word and the
// top bit of a 32-bit one. They pin behaviour, so they pass before and after
// the code moves to unsigned-only bitwise arithmetic (clang-tidy's
// bugprone-signed-bitwise).
#include "core/byte_order.h"
#include "core/ds4_report.h"
#include "core/touchpad_codec.h"

#include <array>
#include <cstdint>
#include <iostream>

#include "test_util.h"

using namespace satellite;

static void test_loads_take_each_boundary_byte_in_each_position() {
    TEST("bytes: 16- and 32-bit loads assemble 0x00, 0x7F, 0x80 and 0xFF in every position");
    const std::array<uint8_t, 4> b{0x80, 0xFF, 0x7F, 0x00};
    EXPECT_EQ(readLE16(b.data()), uint16_t{0xFF80});
    EXPECT_EQ(readBE16(b.data()), uint16_t{0x80FF});
    EXPECT_EQ(readLE32(b.data()), uint32_t{0x007FFF80});
    EXPECT_EQ(readBE32(b.data()), uint32_t{0x80FF7F00});
    const std::array<uint8_t, 4> top{0x00, 0x00, 0x00, 0x80};
    EXPECT_EQ(readLE32(top.data()), uint32_t{0x80000000});
    EXPECT_EQ(readBE32(top.data()), uint32_t{0x00000080});
}

static void test_a_signed_load_takes_its_sign_from_the_high_byte() {
    TEST("bytes: a little-endian int16 takes its sign from the high byte alone");
    const std::array<uint8_t, 2> lowTop{0x80, 0x00};
    const std::array<uint8_t, 2> highTop{0x00, 0x80};
    const std::array<uint8_t, 2> max{0xFF, 0x7F};
    const std::array<uint8_t, 2> minusOne{0xFF, 0xFF};
    EXPECT_EQ(readLE16s(lowTop.data()), int16_t{128});
    EXPECT_EQ(readLE16s(highTop.data()), int16_t{-32768});
    EXPECT_EQ(readLE16s(max.data()), int16_t{32767});
    EXPECT_EQ(readLE16s(minusOne.data()), int16_t{-1});
}

static void test_stores_split_a_word_top_bit_included() {
    TEST("bytes: 16- and 32-bit stores split a word into its bytes, top bit included");
    std::array<uint8_t, 4> out{};
    writeLE32(out.data(), 0x80FF7F01u);
    EXPECT(out == (std::array<uint8_t, 4>{0x01, 0x7F, 0xFF, 0x80}));
    writeBE32(out.data(), 0x80FF7F01u);
    EXPECT(out == (std::array<uint8_t, 4>{0x80, 0xFF, 0x7F, 0x01}));
    std::array<uint8_t, 2> half{};
    writeLE16(half.data(), 0x80FF);
    EXPECT(half == (std::array<uint8_t, 2>{0xFF, 0x80}));
    writeBE16(half.data(), 0x80FF);
    EXPECT(half == (std::array<uint8_t, 2>{0x80, 0xFF}));
}

static void test_a_ds4_stick_byte_spans_the_int16_range() {
    TEST("ds4: a stick byte maps the int16 ends and centre to 0x00, 0xFF and 0x7F");
    EXPECT_EQ(ds4StickByte(-32768), uint8_t{0x00});
    EXPECT_EQ(ds4StickByte(0), uint8_t{0x7F});
    EXPECT_EQ(ds4StickByte(32767), uint8_t{0xFF});
    EXPECT_EQ(ds4StickByteInverted(-32768), uint8_t{0xFF});
    EXPECT_EQ(ds4StickByteInverted(0), uint8_t{0x80});
    EXPECT_EQ(ds4StickByteInverted(32767), uint8_t{0x00});
}

static void test_a_touch_finger_packs_two_12_bit_coordinates() {
    TEST("ds4: a touch finger packs its 12-bit x and y across three bytes, the lifted bit apart");
    TouchpadFinger far{};
    far.active = true;
    far.x = 32767; // 1919, 0x77F
    far.y = 32767; // 942, 0x3AE
    const std::array<uint8_t, 4> packed = ds4PackTouchFinger(far, 0x7F);
    EXPECT(packed == (std::array<uint8_t, 4>{0x7F, 0x7F, 0xE7, 0x3A}));

    TouchpadFinger lifted{};
    lifted.active = false;
    lifted.x = -32768;
    lifted.y = -32768;
    const std::array<uint8_t, 4> up = ds4PackTouchFinger(lifted, 0x80);
    EXPECT(up == (std::array<uint8_t, 4>{0x80, 0x00, 0x00, 0x00}));
    EXPECT_EQ(touchpadWireToRange(0, DS4_TOUCHPAD_RES_X), 960);
}

int main() {
    return satellite::test::runMain("test_byte_boundaries", [] {
        test_loads_take_each_boundary_byte_in_each_position();
        test_a_signed_load_takes_its_sign_from_the_high_byte();
        test_stores_split_a_word_top_bit_included();
        test_a_ds4_stick_byte_spans_the_int16_range();
        test_a_touch_finger_packs_two_12_bit_coordinates();
    });
}
