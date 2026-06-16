// SPDX-License-Identifier: LGPL-3.0-or-later
#include "core/network_info.h"

#include <iostream>
#include <string>
#include <vector>

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

static LocalInterface mkIf(const std::string& name, const std::string& ip, bool physical, bool priv,
                           const std::string& cat = "") {
    LocalInterface f;
    f.name = name;
    f.ipv4 = ip;
    f.physical = physical;
    f.privateIp = priv;
    f.category = cat;
    return f;
}

static void test_topLevelJson() {
    TEST("buildNetworkInfoJson — top-level fields and ports");
    NetworkInfo info;
    info.lanIp = "10.0.0.5";
    info.device = "Ethernet";
    info.category = "public";
    info.selected = "";
    info.allowPublic = false;
    info.udpPort = 9876;
    info.webPort = 9877;
    info.pairPort = 9878;
    info.discPort = 9879;
    info.clientPort = 9443;
    info.mdnsPort = 5353;
    EXPECT_EQ(buildNetworkInfoJson(info),
              std::string("{\"lanIp\":\"10.0.0.5\",\"device\":\"Ethernet\",\"category\":\"public\","
                          "\"selected\":\"\",\"allowPublic\":false,\"ports\":{\"udp\":9876,\"web\":"
                          "9877,\"pair\":9878,\"discovery\":9879,\"client\":9443,\"mdns\":5353},"
                          "\"interfaces\":[]}"));
}

static void test_interfacesArray() {
    TEST("buildNetworkInfoJson — interfaces array with flags");
    NetworkInfo info;
    info.allowPublic = true;
    info.selected = "Ethernet";
    info.interfaces.push_back(mkIf("Ethernet", "10.0.0.5", true, true, "public"));
    info.interfaces.push_back(mkIf("NordLynx", "10.5.0.2", false, true, ""));
    const std::string j = buildNetworkInfoJson(info);
    EXPECT(contains(j, "\"allowPublic\":true"));
    EXPECT(contains(j, "\"selected\":\"Ethernet\""));
    EXPECT(contains(j,
                    "\"interfaces\":[{\"name\":\"Ethernet\",\"ip\":\"10.0.0.5\",\"category\":"
                    "\"public\",\"physical\":true,\"private\":true},{\"name\":\"NordLynx\","
                    "\"ip\":\"10.5.0.2\",\"category\":\"\",\"physical\":false,\"private\":true}]"));
}

static void test_escaping() {
    TEST("buildNetworkInfoJson — device/category JSON-escaped");
    NetworkInfo info;
    info.device = "My \"Cool\" \\Net";
    const std::string j = buildNetworkInfoJson(info);
    EXPECT(contains(j, "\"device\":\"My \\\"Cool\\\" \\\\Net\""));
}

static void test_isPrivateIPv4() {
    TEST("isPrivateIPv4 — RFC1918 ranges only");
    EXPECT(isPrivateIPv4("10.0.0.1"));
    EXPECT(isPrivateIPv4("10.255.255.255"));
    EXPECT(isPrivateIPv4("172.16.0.1"));
    EXPECT(isPrivateIPv4("172.31.255.255"));
    EXPECT(isPrivateIPv4("192.168.1.50"));
    EXPECT(!isPrivateIPv4("172.15.0.1"));
    EXPECT(!isPrivateIPv4("172.32.0.1"));
    EXPECT(!isPrivateIPv4("192.169.0.1"));
    EXPECT(!isPrivateIPv4("169.254.1.1"));
    EXPECT(!isPrivateIPv4("8.8.8.8"));
    EXPECT(!isPrivateIPv4(""));
    EXPECT(!isPrivateIPv4("10.0.0"));
    EXPECT(!isPrivateIPv4("10.0.0.256"));
    EXPECT(!isPrivateIPv4("10.0.0.1.5"));
    EXPECT(!isPrivateIPv4("abc"));
}

static void test_pickAutoInterface() {
    TEST("pickAutoInterface — prefers physical+private, then private, then first");
    std::vector<LocalInterface> empty;
    EXPECT_EQ(pickAutoInterface(empty), -1);

    std::vector<LocalInterface> vpnFirst;
    vpnFirst.push_back(mkIf("NordLynx", "10.5.0.2", false, true));
    vpnFirst.push_back(mkIf("Ethernet", "10.0.0.5", true, true));
    EXPECT_EQ(pickAutoInterface(vpnFirst), 1);

    std::vector<LocalInterface> noPhysicalPrivate;
    noPhysicalPrivate.push_back(mkIf("Public NIC", "8.8.4.4", true, false));
    noPhysicalPrivate.push_back(mkIf("VPN", "10.5.0.2", false, true));
    EXPECT_EQ(pickAutoInterface(noPhysicalPrivate), 1);

    std::vector<LocalInterface> onlyPublic;
    onlyPublic.push_back(mkIf("WAN", "203.0.113.5", true, false));
    EXPECT_EQ(pickAutoInterface(onlyPublic), 0);

    std::vector<LocalInterface> blanks;
    blanks.push_back(mkIf("Disconnected", "", true, false));
    blanks.push_back(mkIf("Ethernet", "192.168.1.2", true, true));
    EXPECT_EQ(pickAutoInterface(blanks), 1);
}

static void test_chooseInterface() {
    TEST("chooseInterface — honors selection, falls back to auto");
    std::vector<LocalInterface> ifaces;
    ifaces.push_back(mkIf("NordLynx", "10.5.0.2", false, true));
    ifaces.push_back(mkIf("Ethernet", "10.0.0.5", true, true));

    EXPECT_EQ(chooseInterface(ifaces, "NordLynx"), 0);
    EXPECT_EQ(chooseInterface(ifaces, "Ethernet"), 1);
    EXPECT_EQ(chooseInterface(ifaces, ""), 1);
    EXPECT_EQ(chooseInterface(ifaces, "Missing"), 1);

    std::vector<LocalInterface> selectedButNoIp;
    selectedButNoIp.push_back(mkIf("Ethernet", "", true, false));
    selectedButNoIp.push_back(mkIf("Wi-Fi", "192.168.0.9", true, true));
    EXPECT_EQ(chooseInterface(selectedButNoIp, "Ethernet"), 1);
}

int main() {
    test_topLevelJson();
    test_interfacesArray();
    test_escaping();
    test_isPrivateIPv4();
    test_pickAutoInterface();
    test_chooseInterface();

    std::cout << "network_info: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
