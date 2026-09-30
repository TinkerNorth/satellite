// SPDX-License-Identifier: LGPL-3.0-or-later
// core/hex.h: bytes as lowercase hex, high nibble first, the separated form a
// MAC address takes, and the big-endian form of a 32-bit session token.
#include "../src/core/hex.h"

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>

#include "test_util.h"

namespace {

void test_hexEncode_isLowercaseHighNibbleFirst() {
    TEST("hexEncode: lowercase, high nibble first");
    const uint8_t in[] = {0x00, 0x0f, 0xa5, 0xff};
    EXPECT_EQ(hexEncode(in, sizeof(in)), std::string("000fa5ff"));
}

void test_hexEncode_ofNothingIsEmpty() {
    TEST("hexEncode: no bytes is an empty string");
    const uint8_t in[] = {0xab};
    EXPECT_EQ(hexEncode(in, 0), std::string(""));
}

void test_hexEncode_matchesPrintfForEveryByte() {
    TEST("hexEncode: every byte value matches printf's %02x");
    uint8_t all[256];
    for (int i = 0; i < 256; i++) all[i] = static_cast<uint8_t>(i);
    const std::string encoded = hexEncode(all, sizeof(all));
    EXPECT_EQ(encoded.size(), size_t{512});
    int mismatches = 0;
    for (int i = 0; i < 256; i++) {
        char ref[3];
        EXPECT_EQ(std::snprintf(ref, sizeof(ref), "%02x", i), 2);
        if (encoded.compare(static_cast<size_t>(i) * 2, 2, ref) != 0) mismatches++;
    }
    EXPECT_EQ(mismatches, 0);
}

void test_hexEncodeSeparated_separatesBytesButNotTheEnds() {
    TEST("hexEncodeSeparated: the separator sits between bytes, never at either end");
    const uint8_t mac[] = {0x02, 0x53, 0x41, 0x54, 0x00, 0x0c};
    EXPECT_EQ(hexEncodeSeparated(mac, sizeof(mac), ':'), std::string("02:53:41:54:00:0c"));
}

void test_hexEncodeSeparated_ofOneByteHasNoSeparator() {
    TEST("hexEncodeSeparated: a single byte has nothing to separate");
    const uint8_t one[] = {0xab};
    EXPECT_EQ(hexEncodeSeparated(one, sizeof(one), ':'), std::string("ab"));
}

void test_hexEncodeSeparated_ofNothingIsEmpty() {
    TEST("hexEncodeSeparated: no bytes is an empty string");
    const uint8_t in[] = {0xab};
    EXPECT_EQ(hexEncodeSeparated(in, 0, ':'), std::string(""));
}

void test_hexEncodeBE32_isEightDigitsMostSignificantFirst() {
    TEST("hexEncodeBE32: eight digits, zero-padded, most significant first");
    EXPECT_EQ(hexEncodeBE32(0x0007a1b2u), std::string("0007a1b2"));
}

void test_hexEncodeBE32_matchesPrintfForEveryDigitInEveryPosition() {
    TEST("hexEncodeBE32: every digit in every position matches printf's %08x");
    int mismatches = 0;
    for (int position = 0; position < 8; position++) {
        for (uint32_t digit = 0; digit < 16; digit++) {
            const uint32_t value = digit << (4 * position);
            char ref[9];
            EXPECT_EQ(std::snprintf(ref, sizeof(ref), "%08x", value), 8);
            if (hexEncodeBE32(value) != ref) mismatches++;
        }
    }
    EXPECT_EQ(mismatches, 0);
}

void test_hexEncodeBE32_matchesPrintfForWholeValues() {
    TEST("hexEncodeBE32: whole values match printf's %08x");
    for (const uint32_t value : {0u, 1u, 0x12345678u, 0x80000001u, 0xdeadbeefu, 0xffffffffu}) {
        char ref[9];
        EXPECT_EQ(std::snprintf(ref, sizeof(ref), "%08x", value), 8);
        EXPECT_EQ(hexEncodeBE32(value), std::string(ref));
    }
}

} // namespace

int main() {
    std::cout << "Running hex encoder tests...\n\n";
    test_hexEncode_isLowercaseHighNibbleFirst();
    test_hexEncode_ofNothingIsEmpty();
    test_hexEncode_matchesPrintfForEveryByte();
    test_hexEncodeSeparated_separatesBytesButNotTheEnds();
    test_hexEncodeSeparated_ofOneByteHasNoSeparator();
    test_hexEncodeSeparated_ofNothingIsEmpty();
    test_hexEncodeBE32_isEightDigitsMostSignificantFirst();
    test_hexEncodeBE32_matchesPrintfForEveryDigitInEveryPosition();
    test_hexEncodeBE32_matchesPrintfForWholeValues();

    std::cout << "test_hex: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
