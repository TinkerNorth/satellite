// SPDX-License-Identifier: LGPL-3.0-or-later
#include "core/network_info.h"

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

static bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

static void test_fullInfo() {
    TEST("buildNetworkInfoJson — full info serializes every field");
    NetworkInfo info;
    info.lanIp = "192.168.1.50";
    info.device = "Wi-Fi";
    info.udpPort = 9876;
    info.webPort = 9877;
    info.pairPort = 9878;
    info.discPort = 9879;
    info.clientPort = 9443;
    info.mdnsPort = 5353;
    EXPECT_EQ(
        buildNetworkInfoJson(info),
        std::string("{\"lanIp\":\"192.168.1.50\",\"device\":\"Wi-Fi\",\"ports\":{\"udp\":9876,"
                    "\"web\":9877,\"pair\":9878,\"discovery\":9879,\"client\":9443,\"mdns\":"
                    "5353}}"));
}

static void test_emptyInfo() {
    TEST("buildNetworkInfoJson — unresolved IP/device render as empty strings");
    NetworkInfo info;
    EXPECT_EQ(buildNetworkInfoJson(info),
              std::string("{\"lanIp\":\"\",\"device\":\"\",\"ports\":{\"udp\":0,\"web\":0,\"pair\":"
                          "0,\"discovery\":0,\"client\":0,\"mdns\":0}}"));
}

static void test_portsIndependent() {
    TEST("buildNetworkInfoJson — each port field is carried independently");
    NetworkInfo a;
    a.udpPort = 1;
    a.webPort = 2;
    a.pairPort = 3;
    a.discPort = 4;
    a.clientPort = 5;
    a.mdnsPort = 6;
    const std::string j = buildNetworkInfoJson(a);
    EXPECT(contains(j, "\"udp\":1"));
    EXPECT(contains(j, "\"web\":2"));
    EXPECT(contains(j, "\"pair\":3"));
    EXPECT(contains(j, "\"discovery\":4"));
    EXPECT(contains(j, "\"client\":5"));
    EXPECT(contains(j, "\"mdns\":6"));
}

static void test_escaping() {
    TEST("buildNetworkInfoJson — device and IP strings are JSON-escaped");
    NetworkInfo info;
    info.lanIp = "10.0.0.1";
    info.device = "My \"Cool\" \\Net";
    const std::string j = buildNetworkInfoJson(info);
    EXPECT(contains(j, "\"device\":\"My \\\"Cool\\\" \\\\Net\""));
    EXPECT(contains(j, "\"lanIp\":\"10.0.0.1\""));

    NetworkInfo ctrl;
    ctrl.device = std::string("a\x01"
                              "b");
    EXPECT(contains(buildNetworkInfoJson(ctrl), "\\u0001"));

    NetworkInfo nl;
    nl.device = "line1\nline2";
    EXPECT(contains(buildNetworkInfoJson(nl), "line1\\nline2"));
}

static void test_mdnsPortStaysAsGiven() {
    TEST("buildNetworkInfoJson — mDNS/DNS port is reported verbatim (read-only field)");
    NetworkInfo info;
    info.mdnsPort = 5353;
    EXPECT(contains(buildNetworkInfoJson(info), "\"mdns\":5353"));
}

int main() {
    test_fullInfo();
    test_emptyInfo();
    test_portsIndependent();
    test_escaping();
    test_mdnsPortStaysAsGiven();

    std::cout << "network_info: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
