// SPDX-License-Identifier: LGPL-3.0-or-later
#include "../src/net/discovery_beacon.h"

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
                      << "  " << #a << " == " << #b << "\n";                                       \
        }                                                                                          \
    } while (0)

static void test_beacon_exact_shape() {
    TEST("buildDiscoveryBeacon — exact JSON shape and field order");
    std::string b = buildDiscoveryBeacon("MyPC", 9876, 9878, 9877, "abc123");
    EXPECT_EQ(b, std::string(R"({"service":"satellite","name":"MyPC","udpPort":9876,)"
                             R"("pairPort":9878,"httpPort":9877,"machineId":"abc123"})"));
}

static void test_beacon_substitutes_fields() {
    TEST("buildDiscoveryBeacon — substitutes name/ports/machineId");
    std::string b = buildDiscoveryBeacon("host-2", 1, 2, 3, "deadbeef");
    EXPECT(b.find(R"("name":"host-2")") != std::string::npos);
    EXPECT(b.find(R"("udpPort":1,)") != std::string::npos);
    EXPECT(b.find(R"("pairPort":2,)") != std::string::npos);
    EXPECT(b.find(R"("httpPort":3,)") != std::string::npos);
    EXPECT(b.find(R"("machineId":"deadbeef")") != std::string::npos);
    EXPECT(b.front() == '{' && b.back() == '}');
}

int main() {
    std::cout << "Running discovery beacon tests...\n\n";
    test_beacon_exact_shape();
    test_beacon_substitutes_fields();

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
