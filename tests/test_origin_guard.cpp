// SPDX-License-Identifier: LGPL-3.0-or-later
#include "../src/net/origin_guard.h"

#include <iostream>
#include <string>

#include "test_util.h"

using satellite::isLoopbackHost;
using satellite::isLoopbackOrigin;

static void test_host_accepts_loopback() {
    TEST("isLoopbackHost — accepts loopback hosts");
    EXPECT(isLoopbackHost("127.0.0.1"));
    EXPECT(isLoopbackHost("localhost"));
    EXPECT(isLoopbackHost("::1"));
    EXPECT(isLoopbackHost("127.0.0.1:9877"));
    EXPECT(isLoopbackHost("localhost:80"));
    EXPECT(isLoopbackHost("[::1]:9877"));
    EXPECT(isLoopbackHost("[::1]"));
}

static void test_host_rejects_nonloopback() {
    TEST("isLoopbackHost — rejects non-loopback hosts");
    EXPECT(!isLoopbackHost("evil.com"));
    EXPECT(!isLoopbackHost("127.0.0.1.evil.com"));
    EXPECT(!isLoopbackHost("localhost.evil.com"));
    EXPECT(!isLoopbackHost("192.168.1.5"));
    EXPECT(!isLoopbackHost("10.0.0.1"));
    EXPECT(!isLoopbackHost("0.0.0.0"));
    EXPECT(!isLoopbackHost("169.254.169.254"));
    EXPECT(!isLoopbackHost(""));
    EXPECT(!isLoopbackHost("notlocalhost"));
    EXPECT(!isLoopbackHost("127.0.0.1@evil.com"));
    EXPECT(!isLoopbackHost("evil.com:127.0.0.1"));
    EXPECT(!isLoopbackHost(" 127.0.0.1"));
    EXPECT(!isLoopbackHost(" localhost"));
    EXPECT(!isLoopbackHost("127.0.0.1.evil.com:9877"));
    EXPECT(!isLoopbackHost("[::1].evil.com"));
    EXPECT(!isLoopbackHost("[fe80::1]"));
    EXPECT(!isLoopbackHost("[evil.com]"));
    EXPECT(!isLoopbackHost("[::1"));
    EXPECT(!isLoopbackHost("LOCALHOST"));
    EXPECT(!isLoopbackHost("127.0.0.2"));
    EXPECT(!isLoopbackHost("::2"));
    EXPECT(!isLoopbackHost("[::1]extra"));
    EXPECT(!isLoopbackHost("[::1]@evil.com"));
    EXPECT(!isLoopbackHost("evil.com:"));
    EXPECT(!isLoopbackHost("::1:9877"));
}

static void test_origin_accepts_loopback() {
    TEST("isLoopbackOrigin — accepts loopback origins");
    EXPECT(isLoopbackOrigin("http://127.0.0.1:8080"));
    EXPECT(isLoopbackOrigin("https://localhost"));
    EXPECT(isLoopbackOrigin("http://[::1]:8080"));
}

static void test_origin_rejects_nonloopback() {
    TEST("isLoopbackOrigin — rejects non-loopback / malformed origins");
    EXPECT(!isLoopbackOrigin("http://evil.com"));
    EXPECT(!isLoopbackOrigin("http://127.0.0.1.evil.com"));
    EXPECT(!isLoopbackOrigin("null"));
    EXPECT(!isLoopbackOrigin(""));
    EXPECT(!isLoopbackOrigin("127.0.0.1"));
    EXPECT(!isLoopbackOrigin("http:127.0.0.1"));
    EXPECT(!isLoopbackOrigin("http://localhost.evil.com"));
    EXPECT(!isLoopbackOrigin("https://192.168.1.5"));
}

int main() {
    std::cout << "Running origin guard tests...\n\n";
    test_host_accepts_loopback();
    test_host_rejects_nonloopback();
    test_origin_accepts_loopback();
    test_origin_rejects_nonloopback();

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
