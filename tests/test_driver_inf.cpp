// SPDX-License-Identifier: LGPL-3.0-or-later
#include "../src/core/driver_inf.h"

#include <iostream>
#include <string>

#include "test_util.h"

using satellite::infTextToNarrow;
using satellite::parseInfDriverVersion;

static std::string utf16le(const std::string& ascii) {
    std::string out("\xFF\xFE", 2);
    for (char c : ascii) {
        out.push_back(c);
        out.push_back('\0');
    }
    return out;
}

static void testPlainInf() {
    TEST("DriverVer line with aligned spacing parses the version");
    const std::string inf = "[Version]\r\nSignature   = \"$WINDOWS NT$\"\r\nClass       = "
                            "HIDClass\r\nDriverVer   = 08/21/2026,1.4.7.12\r\nPnpLockdown = 1\r\n";
    EXPECT_EQ(parseInfDriverVersion(inf), std::string("1.4.7.12"));

    TEST("DriverVer without spaces around = parses");
    EXPECT_EQ(parseInfDriverVersion("DriverVer=08/30/2022,1.21.442.0\n"),
              std::string("1.21.442.0"));

    TEST("spaces after the comma are skipped");
    EXPECT_EQ(parseInfDriverVersion("DriverVer = 01/01/2020,   2.0.1.0"), std::string("2.0.1.0"));

    TEST("a trailing comment or CR does not leak into the version");
    EXPECT_EQ(parseInfDriverVersion("DriverVer = 01/01/2020,3.1.0.5 ; pinned\r\n"),
              std::string("3.1.0.5"));
}

static void testEncodings() {
    TEST("UTF-8 BOM is stripped");
    EXPECT_EQ(parseInfDriverVersion("\xEF\xBB\xBF[Version]\nDriverVer = 08/21/2026,1.4.7.12\n"),
              std::string("1.4.7.12"));

    TEST("UTF-16LE INF (Windows-authored) parses");
    EXPECT_EQ(parseInfDriverVersion(utf16le("[Version]\r\nDriverVer = 08/21/2026,1.4.7.12\r\n")),
              std::string("1.4.7.12"));

    TEST("infTextToNarrow leaves plain text alone");
    EXPECT_EQ(infTextToNarrow("abc"), std::string("abc"));
}

static void testNegatives() {
    TEST("missing DriverVer yields empty");
    EXPECT_EQ(parseInfDriverVersion("[Version]\nSignature = \"$WINDOWS NT$\"\n"), std::string(""));

    TEST("empty input yields empty");
    EXPECT_EQ(parseInfDriverVersion(""), std::string(""));

    TEST("DriverVer with a date but no version yields empty");
    EXPECT_EQ(parseInfDriverVersion("DriverVer = 08/21/2026\n"), std::string(""));

    TEST("DriverVer with a non-numeric version yields empty");
    EXPECT_EQ(parseInfDriverVersion("DriverVer = 08/21/2026,abc\n"), std::string(""));

    TEST("a key that merely starts with DriverVer is not matched");
    EXPECT_EQ(parseInfDriverVersion("DriverVerbose = 08/21/2026,9.9.9.9\n"), std::string(""));

    TEST("a commented-out DriverVer is ignored");
    EXPECT_EQ(
        parseInfDriverVersion("; DriverVer = 08/21/2026,9.9.9.9\nDriverVer = 08/21/2026,1.2.3.4\n"),
        std::string("1.2.3.4"));
}

int main() {
    std::cout << "Running driver INF tests...\n\n";
    testPlainInf();
    testEncodings();
    testNegatives();
    std::cout << "\n=== Test Results ===\n";
    std::cout << "  Passed: " << g_pass << "\n";
    std::cout << "  Failed: " << g_fail << "\n";
    std::cout << "  STATUS: " << (g_fail == 0 ? "ALL PASSED" : "FAILED") << "\n";
    return g_fail == 0 ? 0 : 1;
}
