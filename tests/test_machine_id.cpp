// SPDX-License-Identifier: LGPL-3.0-or-later
#include "../src/net/machine_id.h"

#include <iostream>
#include <string>

#include "test_util.h"

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
    return satellite::test::runMain("test_machine_id", [] {
        std::cout << "Running machine_id tests...\n\n";
        test_accepts_canonical();
        test_rejects_wrong_length();
        test_rejects_non_lowercase_hex();
    });
}
