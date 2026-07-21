// SPDX-License-Identifier: LGPL-3.0-or-later
#include "core/config_json.h"

#include <cstdint>
#include <iostream>
#include <string>

#include "test_util.h"

using namespace satellite;

static void test_full_round_trip() {
    TEST("serializeConfig/parseConfigInto: every field survives a round-trip");
    Config in;
    in.udpPort = 11111;
    in.webPort = 22222;
    in.discPort = 44444;
    in.discoveryBroadcastEnabled = false;
    in.autoStart = true;
    in.updateChannel = UPDATE_CHANNEL_PRERELEASE;
    in.autoCheck = false;
    in.autoDownload = true;
    in.autoInstall = true;
    in.updateCheckIntervalHours = 6;
    in.lastCheckEpoch = 1700000000;
    in.lastSeenVersion = "1.2.3";
    in.skipVersion = "1.2.4";
    in.networkInterface = "eth0";
    in.allowPublicNetwork = true;

    PairedDevice d0;
    d0.id = "dev-0";
    d0.name = "Living Room";
    d0.lastIP = "192.168.1.50";
    d0.pairedAt = "2026-06-19";
    d0.sharedKeyHex = "abc123def456";
    in.pairedDevices.push_back(d0);

    PairedDevice d1;
    d1.id = "dev-1";
    d1.name = "Office";
    d1.lastIP = "10.0.0.2";
    d1.pairedAt = "2026-06-18";
    d1.sharedKeyHex = "0011223344556677";
    in.pairedDevices.push_back(d1);

    Config out;
    parseConfigInto(serializeConfig(in), out);

    EXPECT_EQ(out.udpPort, in.udpPort);
    EXPECT_EQ(out.webPort, in.webPort);
    EXPECT_EQ(out.discPort, in.discPort);
    EXPECT_EQ(out.discoveryBroadcastEnabled, in.discoveryBroadcastEnabled);
    EXPECT_EQ(out.autoStart, in.autoStart);
    EXPECT_EQ(out.updateChannel, in.updateChannel);
    EXPECT_EQ(out.autoCheck, in.autoCheck);
    EXPECT_EQ(out.autoDownload, in.autoDownload);
    EXPECT_EQ(out.autoInstall, in.autoInstall);
    EXPECT_EQ(out.updateCheckIntervalHours, in.updateCheckIntervalHours);
    EXPECT_EQ(out.lastCheckEpoch, in.lastCheckEpoch);
    EXPECT_EQ(out.lastSeenVersion, in.lastSeenVersion);
    EXPECT_EQ(out.skipVersion, in.skipVersion);
    EXPECT_EQ(out.networkInterface, in.networkInterface);
    EXPECT_EQ(out.allowPublicNetwork, in.allowPublicNetwork);

    EXPECT_EQ(out.pairedDevices.size(), in.pairedDevices.size());
    if (out.pairedDevices.size() == 2) {
        EXPECT_EQ(out.pairedDevices[0].id, d0.id);
        EXPECT_EQ(out.pairedDevices[0].name, d0.name);
        EXPECT_EQ(out.pairedDevices[0].lastIP, d0.lastIP);
        EXPECT_EQ(out.pairedDevices[0].pairedAt, d0.pairedAt);
        EXPECT_EQ(out.pairedDevices[0].sharedKeyHex, d0.sharedKeyHex);
        EXPECT_EQ(out.pairedDevices[1].id, d1.id);
        EXPECT_EQ(out.pairedDevices[1].sharedKeyHex, d1.sharedKeyHex);
    }
}

static void test_special_chars_in_name() {
    TEST("device name with quotes/backslash/newline/control byte survives round-trip");
    Config in;
    PairedDevice d;
    d.id = "dev-x";
    d.name = std::string("a\"b\\c\nd\x01"
                         "e");
    d.sharedKeyHex = "deadbeef";
    in.pairedDevices.push_back(d);

    Config out;
    parseConfigInto(serializeConfig(in), out);

    EXPECT_EQ(out.pairedDevices.size(), (size_t)1);
    if (!out.pairedDevices.empty()) { EXPECT_EQ(out.pairedDevices[0].name, d.name); }
}

static void test_tolerant_parsing() {
    TEST("empty/garbage/truncated input does not throw and keeps defaults");
    const Config defaults;

    for (const std::string& bad :
         {std::string(""), std::string("not json"), std::string("{\"udpPort\":"),
          std::string("[1,2,3]"), std::string("12345"), std::string("null")}) {
        Config cfg;
        bool threw = false;
        try {
            parseConfigInto(bad, cfg);
        } catch (...) { threw = true; }
        EXPECT(!threw);
        EXPECT_EQ(cfg.udpPort, defaults.udpPort);
        EXPECT_EQ(cfg.autoCheck, defaults.autoCheck);
        EXPECT_EQ(cfg.pairedDevices.size(), (size_t)0);
    }
}

static void test_present_overrides_absent_keeps_default() {
    TEST("present key overrides; absent keys keep struct defaults");
    Config cfg;
    parseConfigInto(R"({"udpPort":1234})", cfg);
    EXPECT_EQ(cfg.udpPort, 1234);
    EXPECT(cfg.autoCheck);
    EXPECT_EQ(cfg.webPort, DEFAULT_WEB_PORT);
    EXPECT_EQ(cfg.updateChannel, std::string(UPDATE_CHANNEL_STABLE));
}

static void test_paired_device_missing_id_skipped() {
    TEST("a pairedDevices entry missing id is skipped");
    Config cfg;
    parseConfigInto(R"({"pairedDevices":[{"name":"NoId"},{"id":"keep","name":"Keep"}]})", cfg);
    EXPECT_EQ(cfg.pairedDevices.size(), (size_t)1);
    if (!cfg.pairedDevices.empty()) {
        EXPECT_EQ(cfg.pairedDevices[0].id, std::string("keep"));
        EXPECT_EQ(cfg.pairedDevices[0].name, std::string("Keep"));
    }
}

static void test_shared_key_on_disk_name() {
    TEST("on-disk key is sharedKey mapping to PairedDevice::sharedKeyHex");
    Config in;
    PairedDevice d;
    d.id = "dev-k";
    d.sharedKeyHex = "cafef00d";
    in.pairedDevices.push_back(d);
    const std::string text = serializeConfig(in);
    EXPECT(text.find("\"sharedKey\"") != std::string::npos);
    EXPECT(text.find("\"sharedKeyHex\"") == std::string::npos);

    Config out;
    parseConfigInto(R"({"pairedDevices":[{"id":"dev-k","sharedKey":"cafef00d"}]})", out);
    EXPECT_EQ(out.pairedDevices.size(), (size_t)1);
    if (!out.pairedDevices.empty()) {
        EXPECT_EQ(out.pairedDevices[0].sharedKeyHex, std::string("cafef00d"));
    }
}

// Configs written by pre-protocol-1 builds persisted a "pairPort" key (the
// deleted plaintext pairing listener). The tolerant jsonTry* accessors never
// look for it, so a legacy file must load with every other field intact and
// the unknown key silently ignored — an upgrade must never eat user config.
static void test_legacy_pair_port_key_ignored() {
    TEST("legacy config containing the removed pairPort key still loads");
    Config cfg;
    parseConfigInto(R"({
        "udpPort": 9876,
        "webPort": 9877,
        "pairPort": 9878,
        "discPort": 9879,
        "autoStart": true,
        "networkInterface": "eth0",
        "pairedDevices": [
            {"id": "dev-legacy", "name": "Old Phone", "lastIP": "192.168.1.7",
             "pairedAt": "2025-03-01", "sharedKey": "aa55aa55"}
        ]
    })",
                    cfg);
    EXPECT_EQ(cfg.udpPort, 9876);
    EXPECT_EQ(cfg.webPort, 9877);
    EXPECT_EQ(cfg.discPort, 9879);
    EXPECT_EQ(cfg.autoStart, true);
    EXPECT_EQ(cfg.networkInterface, std::string("eth0"));
    EXPECT_EQ(cfg.pairedDevices.size(), (size_t)1);
    if (!cfg.pairedDevices.empty()) {
        EXPECT_EQ(cfg.pairedDevices[0].id, std::string("dev-legacy"));
        EXPECT_EQ(cfg.pairedDevices[0].sharedKeyHex, std::string("aa55aa55"));
    }

    TEST("re-serializing a migrated legacy config drops the pairPort key");
    EXPECT(serializeConfig(cfg).find("pairPort") == std::string::npos);
}

static void test_last_check_epoch_64bit() {
    TEST("lastCheckEpoch round-trips a value larger than 2^31 (not truncated)");
    Config in;
    in.lastCheckEpoch = (int64_t)5000000000LL;
    Config out;
    parseConfigInto(serializeConfig(in), out);
    EXPECT_EQ(out.lastCheckEpoch, (int64_t)5000000000LL);

    Config direct;
    parseConfigInto(R"({"lastCheckEpoch":5000000000})", direct);
    EXPECT_EQ(direct.lastCheckEpoch, (int64_t)5000000000LL);
}

int main() {
    test_full_round_trip();
    test_special_chars_in_name();
    test_tolerant_parsing();
    test_present_overrides_absent_keeps_default();
    test_paired_device_missing_id_skipped();
    test_shared_key_on_disk_name();
    test_legacy_pair_port_key_ignored();
    test_last_check_epoch_64bit();

    std::cout << "config_json: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
