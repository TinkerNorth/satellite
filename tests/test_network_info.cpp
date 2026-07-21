// SPDX-License-Identifier: LGPL-3.0-or-later
#include "core/network_info.h"

#include <iostream>
#include <string>
#include <vector>

#include "test_util.h"

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

static void test_isPrivateIPv4_ranges() {
    TEST("isPrivateIPv4: 10/8");
    EXPECT(isPrivateIPv4("10.0.0.0"));
    EXPECT(isPrivateIPv4("10.0.0.1"));
    EXPECT(isPrivateIPv4("10.128.64.7"));
    EXPECT(isPrivateIPv4("10.255.255.255"));

    TEST("isPrivateIPv4: 172.16/12 boundaries");
    EXPECT(isPrivateIPv4("172.16.0.0"));
    EXPECT(isPrivateIPv4("172.16.5.5"));
    EXPECT(isPrivateIPv4("172.20.0.1"));
    EXPECT(isPrivateIPv4("172.31.255.255"));
    EXPECT(!isPrivateIPv4("172.15.255.255"));
    EXPECT(!isPrivateIPv4("172.32.0.0"));
    EXPECT(!isPrivateIPv4("172.0.0.1"));
    EXPECT(!isPrivateIPv4("172.255.0.1"));

    TEST("isPrivateIPv4: 192.168/16 boundaries");
    EXPECT(isPrivateIPv4("192.168.0.0"));
    EXPECT(isPrivateIPv4("192.168.1.50"));
    EXPECT(isPrivateIPv4("192.168.255.255"));
    EXPECT(!isPrivateIPv4("192.167.255.255"));
    EXPECT(!isPrivateIPv4("192.169.0.0"));

    TEST("isPrivateIPv4: public / special are not private");
    EXPECT(!isPrivateIPv4("0.0.0.0"));
    EXPECT(!isPrivateIPv4("255.255.255.255"));
    EXPECT(!isPrivateIPv4("127.0.0.1"));
    EXPECT(!isPrivateIPv4("169.254.1.1"));
    EXPECT(!isPrivateIPv4("8.8.8.8"));
    EXPECT(!isPrivateIPv4("1.1.1.1"));
    EXPECT(!isPrivateIPv4("203.0.113.5"));
    EXPECT(!isPrivateIPv4("11.0.0.1"));
    EXPECT(!isPrivateIPv4("9.255.255.255"));
}

static void test_isPrivateIPv4_malformed() {
    TEST("isPrivateIPv4: malformed inputs read as not-private, never crash");
    EXPECT(!isPrivateIPv4(""));
    EXPECT(!isPrivateIPv4("10"));
    EXPECT(!isPrivateIPv4("10.0"));
    EXPECT(!isPrivateIPv4("10.0.0"));
    EXPECT(!isPrivateIPv4("10.0.0.0.0"));
    EXPECT(!isPrivateIPv4("10.0.0.256"));
    EXPECT(!isPrivateIPv4("256.0.0.1"));
    EXPECT(!isPrivateIPv4("10.0.0.999"));
    EXPECT(!isPrivateIPv4("10.0.0.a"));
    EXPECT(!isPrivateIPv4("abc"));
    EXPECT(!isPrivateIPv4("10.0.0."));
    EXPECT(!isPrivateIPv4(".10.0.0"));
    EXPECT(!isPrivateIPv4("10..0.0"));
    EXPECT(!isPrivateIPv4("10.0.0.1 "));
    EXPECT(!isPrivateIPv4(" 10.0.0.1"));
    EXPECT(!isPrivateIPv4("10.0.0.-1"));
    EXPECT(!isPrivateIPv4("10:0:0:1"));

    TEST("isPrivateIPv4: leading zeros parse as decimal");
    EXPECT(isPrivateIPv4("010.0.0.1"));
    EXPECT(isPrivateIPv4("192.168.001.001"));
}

static void test_pickAuto_priority() {
    TEST("pickAutoInterface: physical+private wins over VPN-private regardless of order");
    std::vector<LocalInterface> vpnFirst;
    vpnFirst.push_back(mkIf("NordLynx", "10.5.0.2", false, true));
    vpnFirst.push_back(mkIf("Ethernet", "10.0.0.5", true, true));
    EXPECT_EQ(pickAutoInterface(vpnFirst), 1);

    std::vector<LocalInterface> ethFirst;
    ethFirst.push_back(mkIf("Ethernet", "10.0.0.5", true, true));
    ethFirst.push_back(mkIf("NordLynx", "10.5.0.2", false, true));
    EXPECT_EQ(pickAutoInterface(ethFirst), 0);

    TEST("pickAutoInterface: private (even VPN) beats physical-public");
    std::vector<LocalInterface> v;
    v.push_back(mkIf("WAN", "203.0.113.9", true, false));
    v.push_back(mkIf("NordLynx", "10.5.0.2", false, true));
    EXPECT_EQ(pickAutoInterface(v), 1);

    TEST("pickAutoInterface: physical-public beats non-physical-public");
    std::vector<LocalInterface> w;
    w.push_back(mkIf("WeirdVirtual", "203.0.113.7", false, false));
    w.push_back(mkIf("WAN", "203.0.113.9", true, false));
    EXPECT_EQ(pickAutoInterface(w), 1);

    TEST("pickAutoInterface: falls through to first usable when nothing better");
    std::vector<LocalInterface> x;
    x.push_back(mkIf("OnlyOne", "203.0.113.9", false, false));
    EXPECT_EQ(pickAutoInterface(x), 0);
}

static void test_pickAuto_order_and_empty() {
    TEST("pickAutoInterface: empty list and all-blank IPs yield -1");
    std::vector<LocalInterface> empty;
    EXPECT_EQ(pickAutoInterface(empty), -1);
    std::vector<LocalInterface> blanks;
    blanks.push_back(mkIf("A", "", true, true));
    blanks.push_back(mkIf("B", "", false, false));
    EXPECT_EQ(pickAutoInterface(blanks), -1);

    TEST("pickAutoInterface: blank-IP candidates are skipped");
    std::vector<LocalInterface> mixed;
    mixed.push_back(mkIf("Disconnected", "", true, true));
    mixed.push_back(mkIf("Ethernet", "192.168.1.2", true, true));
    EXPECT_EQ(pickAutoInterface(mixed), 1);

    TEST("pickAutoInterface: first among equals (default/first)");
    std::vector<LocalInterface> twoEqual;
    twoEqual.push_back(mkIf("Eth1", "10.0.0.2", true, true));
    twoEqual.push_back(mkIf("Eth2", "10.0.0.3", true, true));
    EXPECT_EQ(pickAutoInterface(twoEqual), 0);

    std::vector<LocalInterface> twoVpn;
    twoVpn.push_back(mkIf("wg0", "10.7.0.2", false, true));
    twoVpn.push_back(mkIf("tun0", "10.8.0.2", false, true));
    EXPECT_EQ(pickAutoInterface(twoVpn), 0);
}

static void test_chooseInterface() {
    TEST("chooseInterface: exact name match selected");
    std::vector<LocalInterface> ifaces;
    ifaces.push_back(mkIf("NordLynx", "10.5.0.2", false, true));
    ifaces.push_back(mkIf("Ethernet", "10.0.0.5", true, true));
    ifaces.push_back(mkIf("Wi-Fi", "192.168.0.9", true, true));
    EXPECT_EQ(chooseInterface(ifaces, "NordLynx"), 0);
    EXPECT_EQ(chooseInterface(ifaces, "Ethernet"), 1);
    EXPECT_EQ(chooseInterface(ifaces, "Wi-Fi"), 2);

    TEST("chooseInterface: empty selection and misses fall back to auto");
    EXPECT_EQ(chooseInterface(ifaces, ""), 1);
    EXPECT_EQ(chooseInterface(ifaces, "Missing"), 1);

    TEST("chooseInterface: name match is case-sensitive");
    EXPECT_EQ(chooseInterface(ifaces, "ethernet"), 1);

    TEST("chooseInterface: selected with no IP falls back to auto");
    std::vector<LocalInterface> noip;
    noip.push_back(mkIf("Ethernet", "", true, false));
    noip.push_back(mkIf("Wi-Fi", "192.168.0.9", true, true));
    EXPECT_EQ(chooseInterface(noip, "Ethernet"), 1);

    TEST("chooseInterface: no usable interface yields -1");
    std::vector<LocalInterface> none;
    EXPECT_EQ(chooseInterface(none, "Ethernet"), -1);
}

static void test_buildJson_empty() {
    TEST("buildNetworkInfoJson: all-default produces canonical empty shape");
    NetworkInfo info;
    EXPECT_EQ(buildNetworkInfoJson(info),
              std::string("{\"lanIp\":\"\",\"device\":\"\",\"category\":\"\",\"selected\":\"\","
                          "\"allowPublic\":false,\"ports\":{\"udp\":0,\"web\":0,"
                          "\"discovery\":0,\"client\":0,\"mdns\":0},\"firewall\":{\"supported\":"
                          "false,\"state\":\"\"},\"interfaces\":[]}"));
}

static void test_buildJson_topLevel() {
    TEST("buildNetworkInfoJson: top-level fields + ports, no interfaces");
    NetworkInfo info;
    info.lanIp = "10.0.0.5";
    info.device = "Ethernet 2";
    info.category = "private";
    info.selected = "Ethernet 2";
    info.allowPublic = true;
    info.udpPort = 9876;
    info.webPort = 9877;
    info.discPort = 9879;
    info.clientPort = 9443;
    info.mdnsPort = 5353;
    EXPECT_EQ(buildNetworkInfoJson(info),
              std::string("{\"lanIp\":\"10.0.0.5\",\"device\":\"Ethernet 2\",\"category\":"
                          "\"private\",\"selected\":\"Ethernet 2\",\"allowPublic\":true,\"ports\":{"
                          "\"udp\":9876,\"web\":9877,\"discovery\":9879,\"client\":"
                          "9443,\"mdns\":5353},\"firewall\":{\"supported\":false,\"state\":\"\"},"
                          "\"interfaces\":[]}"));
}

static void test_buildJson_interfaces() {
    TEST("buildNetworkInfoJson: full interfaces array exact");
    NetworkInfo info;
    info.lanIp = "10.0.0.5";
    info.device = "Ethernet 2";
    info.category = "public";
    info.interfaces.push_back(mkIf("Ethernet 2", "10.0.0.5", true, true, "public"));
    info.interfaces.push_back(mkIf("NordLynx", "10.5.0.2", false, true, ""));
    EXPECT(contains(buildNetworkInfoJson(info),
                    "\"interfaces\":[{\"name\":\"Ethernet 2\",\"ip\":\"10.0.0.5\",\"category\":"
                    "\"public\",\"physical\":true,\"private\":true},{\"name\":\"NordLynx\",\"ip\":"
                    "\"10.5.0.2\",\"category\":\"\",\"physical\":false,\"private\":true}]"));

    TEST("buildNetworkInfoJson: single interface, flags reflected");
    NetworkInfo one;
    one.interfaces.push_back(mkIf("Wi-Fi", "192.168.1.7", true, true, "private"));
    const std::string j = buildNetworkInfoJson(one);
    EXPECT(contains(j, "\"interfaces\":[{\"name\":\"Wi-Fi\",\"ip\":\"192.168.1.7\",\"category\":"
                       "\"private\",\"physical\":true,\"private\":true}]"));
}

static void test_buildJson_flags() {
    TEST("buildNetworkInfoJson: allowPublic true/false and categories serialize");
    NetworkInfo a;
    a.allowPublic = true;
    EXPECT(contains(buildNetworkInfoJson(a), "\"allowPublic\":true"));
    NetworkInfo b;
    b.allowPublic = false;
    EXPECT(contains(buildNetworkInfoJson(b), "\"allowPublic\":false"));

    for (const std::string& cat : {std::string("public"), std::string("private"),
                                   std::string("domain"), std::string("unknown")}) {
        NetworkInfo c;
        c.category = cat;
        EXPECT(contains(buildNetworkInfoJson(c), "\"category\":\"" + cat + "\""));
    }
}

static void test_buildJson_escaping() {
    TEST("buildNetworkInfoJson: escapes quotes/backslash/newline/control in strings");
    NetworkInfo info;
    info.device = "My \"Cool\" \\Net";
    info.selected = "a\"b";
    info.category = "x\\y";
    info.lanIp = "1.2.3.4";
    const std::string j = buildNetworkInfoJson(info);
    EXPECT(contains(j, "\"device\":\"My \\\"Cool\\\" \\\\Net\""));
    EXPECT(contains(j, "\"selected\":\"a\\\"b\""));
    EXPECT(contains(j, "\"category\":\"x\\\\y\""));

    NetworkInfo nl;
    nl.device = "line1\nline2";
    EXPECT(contains(buildNetworkInfoJson(nl), "line1\\nline2"));

    NetworkInfo ctrl;
    ctrl.device = std::string("a\x01"
                              "b");
    EXPECT(contains(buildNetworkInfoJson(ctrl), "\\u0001"));

    TEST("buildNetworkInfoJson: escapes interface name and category");
    NetworkInfo ifc;
    ifc.interfaces.push_back(mkIf("Tap \"VPN\"", "10.9.0.2", false, true, "pu\\b"));
    const std::string ij = buildNetworkInfoJson(ifc);
    EXPECT(contains(ij, "\"name\":\"Tap \\\"VPN\\\"\""));
    EXPECT(contains(ij, "\"category\":\"pu\\\\b\""));
}

static void test_buildJson_firewall() {
    TEST("buildNetworkInfoJson: firewall supported+state serialize");
    NetworkInfo a;
    a.firewallSupported = true;
    a.firewallState = "wrong-profile";
    EXPECT(contains(buildNetworkInfoJson(a),
                    "\"firewall\":{\"supported\":true,\"state\":\"wrong-profile\"}"));

    NetworkInfo b;
    EXPECT(contains(buildNetworkInfoJson(b), "\"firewall\":{\"supported\":false,\"state\":\"\"}"));
}

int main() {
    test_isPrivateIPv4_ranges();
    test_isPrivateIPv4_malformed();
    test_pickAuto_priority();
    test_pickAuto_order_and_empty();
    test_chooseInterface();
    test_buildJson_empty();
    test_buildJson_topLevel();
    test_buildJson_interfaces();
    test_buildJson_flags();
    test_buildJson_escaping();
    test_buildJson_firewall();

    std::cout << "network_info: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
