// SPDX-License-Identifier: LGPL-3.0-or-later
#include "../src/core/ipv4_util.h"

#include <iostream>
#include <string>

static int g_pass = 0;
static int g_fail = 0;
static std::string g_currentTest;

#define TEST(name)                                                                                 \
    do { g_currentTest = (name); } while (0)

#define EXPECT(cond)                                                                               \
    do {                                                                                           \
        if (cond) {                                                                                \
            g_pass++;                                                                              \
        } else {                                                                                   \
            g_fail++;                                                                              \
            std::cerr << "  FAIL [" << g_currentTest << "] " << __FILE__ << ":" << __LINE__        \
                      << "  " << #cond << "\n";                                                    \
        }                                                                                          \
    } while (0)

#define EXPECT_EQ(a, b)                                                                            \
    do {                                                                                           \
        auto _a = (a);                                                                             \
        auto _b = (b);                                                                             \
        if (_a == _b) {                                                                            \
            g_pass++;                                                                              \
        } else {                                                                                   \
            g_fail++;                                                                              \
            std::cerr << "  FAIL [" << g_currentTest << "] " << __FILE__ << ":" << __LINE__        \
                      << "  " << #a << " == " << #b << "  (got " << _a << " vs " << _b << ")\n";   \
        }                                                                                          \
    } while (0)

using satellite::formatIPv4Nbo;
using satellite::parseIPv4Nbo;

static void test_parse_valid() {
    TEST("parseIPv4Nbo — network byte order matches sockaddr layout");
    // 1.2.3.4 → leftmost octet in the low byte.
    EXPECT_EQ(parseIPv4Nbo("1.2.3.4"), 0x04030201u);
    EXPECT_EQ(parseIPv4Nbo("0.0.0.0"), 0x00000000u);
    EXPECT_EQ(parseIPv4Nbo("255.255.255.255"), 0xFFFFFFFFu);
    EXPECT_EQ(parseIPv4Nbo("192.168.1.10"), 0x0A01A8C0u);
}

static void test_parse_malformed_returns_zero() {
    TEST("parseIPv4Nbo — malformed input returns 0");
    EXPECT_EQ(parseIPv4Nbo(""), 0u);
    EXPECT_EQ(parseIPv4Nbo("1.2.3"), 0u);        // too few octets
    EXPECT_EQ(parseIPv4Nbo("1.2.3.4.5"), 0u);    // too many octets
    EXPECT_EQ(parseIPv4Nbo("256.0.0.1"), 0u);    // octet > 255
    EXPECT_EQ(parseIPv4Nbo("999.1.1.1"), 0u);    // overflow mid-parse
    EXPECT_EQ(parseIPv4Nbo("1.2.3."), 0u);       // trailing dot, empty octet
    EXPECT_EQ(parseIPv4Nbo(".1.2.3"), 0u);       // leading dot
    EXPECT_EQ(parseIPv4Nbo("1..2.3"), 0u);       // empty middle octet
    EXPECT_EQ(parseIPv4Nbo("a.b.c.d"), 0u);      // non-numeric
    EXPECT_EQ(parseIPv4Nbo("1.2.3.x"), 0u);      // trailing junk
    EXPECT_EQ(parseIPv4Nbo("192.168.1.1 "), 0u); // trailing space
}

static void test_format() {
    TEST("formatIPv4Nbo — renders network-order uint32 as dotted quad");
    EXPECT_EQ(formatIPv4Nbo(0x04030201u), std::string("1.2.3.4"));
    EXPECT_EQ(formatIPv4Nbo(0x00000000u), std::string("0.0.0.0"));
    EXPECT_EQ(formatIPv4Nbo(0xFFFFFFFFu), std::string("255.255.255.255"));
    EXPECT_EQ(formatIPv4Nbo(0x0A01A8C0u), std::string("192.168.1.10"));
}

static void test_round_trip() {
    TEST("parse/format round-trip is identity for every octet value");
    const char* samples[] = {"0.0.0.0", "1.2.3.4", "10.0.0.1", "172.16.254.1", "255.255.255.255"};
    for (const char* s : samples) { EXPECT_EQ(formatIPv4Nbo(parseIPv4Nbo(s)), std::string(s)); }
}

int main() {
    std::cout << "Running ipv4_util tests...\n\n";
    test_parse_valid();
    test_parse_malformed_returns_zero();
    test_format();
    test_round_trip();

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
