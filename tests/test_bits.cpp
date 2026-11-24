// SPDX-License-Identifier: LGPL-3.0-or-later
//
// core/bits.h, each helper at the values a signed shift, mask or sign
// extension gets wrong: 0x00, 0x7F, 0x80 and 0xFF, the sign bit of a 16-bit
// word and the top bit of a 32- and 64-bit one.
#include "core/bits.h"

#include <array>
#include <cstdint>
#include <iostream>

#include "test_util.h"

namespace bits = satellite::bits;

static void test_a_byte_widens_keeping_its_top_bit() {
    TEST("bits: a byte widens to an unsigned word that keeps its top bit");
    EXPECT_EQ(bits::u32(uint8_t{0x80}) << 24U, 0x80000000U);
    EXPECT_EQ(bits::u32(uint8_t{0xFF}) << 24U, 0xFF000000U);
    EXPECT_EQ(bits::u32(uint16_t{0x8000}) << 16U, 0x80000000U);
    EXPECT_EQ(bits::u64(uint8_t{0x80}) << 56U, 0x8000000000000000ULL);
}

static void test_loads_assemble_every_boundary_byte() {
    TEST("bits: loads assemble every boundary byte in every position");
    const std::array<uint8_t, 4> b{0x80, 0xFF, 0x7F, 0x00};
    EXPECT_EQ(bits::loadLe16(b.data()), 0xFF80U);
    EXPECT_EQ(bits::loadBe16(b.data()), 0x80FFU);
    EXPECT_EQ(bits::loadLe32(b.data()), 0x007FFF80U);
    EXPECT_EQ(bits::loadBe32(b.data()), 0x80FF7F00U);
    const std::array<uint8_t, 4> top{0x00, 0x00, 0x00, 0x80};
    EXPECT_EQ(bits::loadLe32(top.data()), 0x80000000U);
}

static void test_packed_12_bit_fields() {
    TEST("bits: two 12-bit fields split across three bytes at their extremes");
    const std::array<uint8_t, 3> ones{0xFF, 0xFF, 0xFF};
    EXPECT_EQ(bits::loadPacked12Low(ones.data()), 0x0FFFU);
    EXPECT_EQ(bits::loadPacked12High(ones.data() + 1), 0x0FFFU);
    const std::array<uint8_t, 3> mixed{0x80, 0x7F, 0x80};
    EXPECT_EQ(bits::loadPacked12Low(mixed.data()), 0x0F80U);
    EXPECT_EQ(bits::loadPacked12High(mixed.data() + 1), 0x0807U);
}

static void test_stores_split_a_word() {
    TEST("bits: stores split a word into its bytes, top bit included");
    std::array<uint8_t, 4> out{};
    bits::storeLe32(out.data(), 0x80FF7F01U);
    EXPECT(out == (std::array<uint8_t, 4>{0x01, 0x7F, 0xFF, 0x80}));
    bits::storeBe32(out.data(), 0x80FF7F01U);
    EXPECT(out == (std::array<uint8_t, 4>{0x80, 0xFF, 0x7F, 0x01}));
    std::array<uint8_t, 2> half{};
    bits::storeLe16(half.data(), 0x80FFU);
    EXPECT(half == (std::array<uint8_t, 2>{0xFF, 0x80}));
    bits::storeBe16(half.data(), 0x80FFU);
    EXPECT(half == (std::array<uint8_t, 2>{0x80, 0xFF}));
}

static void test_the_signed_view_is_twos_complement() {
    TEST("bits: the signed view is the two's-complement reading");
    EXPECT_EQ(bits::asInt16(0x0000U), int16_t{0});
    EXPECT_EQ(bits::asInt16(0x7FFFU), int16_t{32767});
    EXPECT_EQ(bits::asInt16(0x8000U), int16_t{-32768});
    EXPECT_EQ(bits::asInt16(0xFFFFU), int16_t{-1});
    EXPECT_EQ(bits::asInt32(0x80000000U), INT32_MIN);
    EXPECT_EQ(bits::asInt32(0xFFFFFFFFU), int32_t{-1});
    EXPECT_EQ(bits::asUint16(-1), uint16_t{0xFFFF});
    EXPECT_EQ(bits::asUint32(-2), 0xFFFFFFFEU);
}

static void test_narrowing_and_nibbles() {
    TEST("bits: narrowing keeps the low part, nibbles split and join");
    EXPECT_EQ(bits::lowByte(0x12345680U), uint8_t{0x80});
    EXPECT_EQ(bits::low16(0x1234FF80U), uint16_t{0xFF80});
    EXPECT_EQ(bits::byteOf(0x80FF7F01U, 3U), uint8_t{0x80});
    EXPECT_EQ(bits::byteOf(uint64_t{0xFF00000000000000ULL}, 7U), uint8_t{0xFF});
    EXPECT_EQ(bits::highNibble(0x80), uint8_t{0x08});
    EXPECT_EQ(bits::lowNibble(0x7F), uint8_t{0x0F});
    EXPECT_EQ(bits::fromNibbles(0x0F, 0x00), uint8_t{0xF0});
    EXPECT(bits::anySet(0x80U, 0x80U));
    EXPECT(!bits::anySet(0x7FU, 0x80U));
}

static void test_packing_two_ids() {
    TEST("bits: two ids pack into one key, a high id past the sign bit included");
    EXPECT_EQ(bits::packHigh16Low16(0x8000, 0xFFFF), bits::asInt32(0x8000FFFFU));
    EXPECT_EQ(bits::packHigh16Low16(1, -1), int32_t{0x0001FFFF});
    EXPECT_EQ(bits::packHigh32Low32(-1, 0), int64_t{-4294967296LL});
    EXPECT_EQ(bits::packHigh32Low32(0, -1), int64_t{0xFFFFFFFFLL});
}

static void test_pow2() {
    TEST("bits: pow2 is the doubling a shift used to spell");
    EXPECT_EQ(bits::pow2(0U), int64_t{1});
    EXPECT_EQ(bits::pow2(6U), int64_t{64});
    EXPECT_EQ(bits::pow2(15U), int64_t{32768});
    EXPECT_EQ(bits::pow2(62U), int64_t{4611686018427387904LL});
}

int main() {
    test_a_byte_widens_keeping_its_top_bit();
    test_loads_assemble_every_boundary_byte();
    test_packed_12_bit_fields();
    test_stores_split_a_word();
    test_the_signed_view_is_twos_complement();
    test_narrowing_and_nibbles();
    test_packing_two_ids();
    test_pow2();

    std::cout << "\n=== Test Results ===\n";
    std::cout << "  Passed: " << g_pass << "\n";
    std::cout << "  Failed: " << g_fail << "\n";
    if (g_fail > 0) {
        std::cout << "  STATUS: FAIL\n";
        return 1;
    }
    std::cout << "  STATUS: ALL PASSED\n";
    return 0;
}
