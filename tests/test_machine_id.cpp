// SPDX-License-Identifier: LGPL-3.0-or-later
#include "../src/net/machine_id.h"

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

static void test_accepts_canonical() {
    TEST("isValidMachineId: accepts a 32-char lowercase-hex id");
    EXPECT(isValidMachineId("0123456789abcdef0123456789abcdef"));
    EXPECT(isValidMachineId("ffffffffffffffffffffffffffffffff"));
    EXPECT(isValidMachineId("00000000000000000000000000000000"));
}

static void test_rejects_wrong_length() {
    TEST("isValidMachineId: rejects wrong length");
    EXPECT(!isValidMachineId(""));
    EXPECT(!isValidMachineId("0123456789abcdef0123456789abcde"));   // 31
    EXPECT(!isValidMachineId("0123456789abcdef0123456789abcdef0")); // 33
}

static void test_rejects_non_lowercase_hex() {
    TEST("isValidMachineId: rejects uppercase and non-hex chars (regenerate on junk)");
    EXPECT(!isValidMachineId("0123456789ABCDEF0123456789abcdef")); // uppercase
    EXPECT(!isValidMachineId("0123456789abcdef0123456789abcdeg")); // 'g'
    EXPECT(!isValidMachineId("0123456789abcdef0123456789abcde ")); // trailing space
    EXPECT(!isValidMachineId("zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz")); // all non-hex
}

int main() {
    std::cout << "Running machine_id tests...\n\n";
    test_accepts_canonical();
    test_rejects_wrong_length();
    test_rejects_non_lowercase_hex();

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
