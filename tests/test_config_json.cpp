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
    in.controllerAudio = false;
    in.controllerAudioMic = false;
    in.controllerAudioSpeaker = false;
    in.controllerAudioKeepDefaultDevice = false;

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
    EXPECT_EQ(out.controllerAudio, in.controllerAudio);
    EXPECT_EQ(out.controllerAudioMic, in.controllerAudioMic);
    EXPECT_EQ(out.controllerAudioSpeaker, in.controllerAudioSpeaker);
    EXPECT_EQ(out.controllerAudioKeepDefaultDevice, in.controllerAudioKeepDefaultDevice);

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

// Controller audio decides whether a plug installs a kernel USB transport, so
// "absent means default" has to mean the documented default (on) and an
// explicit false has to survive a save/load round-trip. Getting the absent case
// wrong either turns audio off for everyone on upgrade or turns it on for a
// user who switched it off.
static void test_controller_audio_default_and_persistence() {
    TEST("controllerAudio defaults to on");
    const Config defaults;
    EXPECT(defaults.controllerAudio);

    TEST("a config written before controller audio existed loads as on");
    Config legacy;
    parseConfigInto(R"({"udpPort":9876,"autoStart":true})", legacy);
    EXPECT(legacy.controllerAudio);

    TEST("an explicit false survives serialize -> parse");
    Config off;
    off.controllerAudio = false;
    Config back;
    parseConfigInto(serializeConfig(off), back);
    EXPECT(!back.controllerAudio);

    TEST("an explicit true survives too, and the key is actually written");
    Config on;
    on.controllerAudio = true;
    const std::string text = serializeConfig(on);
    EXPECT(text.find("\"controllerAudio\"") != std::string::npos);
    Config backOn;
    backOn.controllerAudio = false; // start from the opposite value
    parseConfigInto(text, backOn);
    EXPECT(backOn.controllerAudio);

    TEST("a non-boolean value is ignored rather than coerced");
    Config junk;
    parseConfigInto(R"({"controllerAudio":"no"})", junk);
    EXPECT(junk.controllerAudio);
}

// The split adds two keys under the master switch. The dangerous case is
// upgrade: a config written before the split has neither key, and reading
// either as "off" would silently kill a direction the user never turned off.
static void test_controller_audio_split_directions() {
    TEST("both directions default to on");
    const Config defaults;
    EXPECT(defaults.controllerAudioMic);
    EXPECT(defaults.controllerAudioSpeaker);

    TEST("a config predating the split loads with both directions on");
    Config legacy;
    parseConfigInto(R"({"udpPort":9876,"controllerAudio":true})", legacy);
    EXPECT(legacy.controllerAudio);
    EXPECT(legacy.controllerAudioMic);
    EXPECT(legacy.controllerAudioSpeaker);

    TEST("the directions are independent of each other");
    Config micOnly;
    parseConfigInto(R"({"controllerAudioSpeaker":false})", micOnly);
    EXPECT(micOnly.controllerAudioMic);
    EXPECT(!micOnly.controllerAudioSpeaker);

    Config speakerOnly;
    parseConfigInto(R"({"controllerAudioMic":false})", speakerOnly);
    EXPECT(!speakerOnly.controllerAudioMic);
    EXPECT(speakerOnly.controllerAudioSpeaker);

    TEST("the directions are independent of the master switch");
    Config masterOff;
    parseConfigInto(R"({"controllerAudio":false})", masterOff);
    EXPECT(!masterOff.controllerAudio);
    EXPECT(masterOff.controllerAudioMic);
    EXPECT(masterOff.controllerAudioSpeaker);

    TEST("both keys are actually written, and survive a round-trip");
    Config in;
    in.controllerAudioMic = false;
    in.controllerAudioSpeaker = true;
    const std::string text = serializeConfig(in);
    EXPECT(text.find("\"controllerAudioMic\"") != std::string::npos);
    EXPECT(text.find("\"controllerAudioSpeaker\"") != std::string::npos);
    Config out;
    out.controllerAudioMic = true;
    out.controllerAudioSpeaker = false;
    parseConfigInto(text, out);
    EXPECT(!out.controllerAudioMic);
    EXPECT(out.controllerAudioSpeaker);

    TEST("non-boolean values are ignored rather than coerced");
    Config junk;
    parseConfigInto(R"({"controllerAudioMic":"no","controllerAudioSpeaker":0})", junk);
    EXPECT(junk.controllerAudioMic);
    EXPECT(junk.controllerAudioSpeaker);

    TEST("keeping the default playback device defaults to on");
    EXPECT(Config{}.controllerAudioKeepDefaultDevice);

    TEST("a config predating the guard loads with it on");
    Config preGuard;
    parseConfigInto(R"({"controllerAudio":true,"controllerAudioMic":false})", preGuard);
    EXPECT(preGuard.controllerAudioKeepDefaultDevice);

    TEST("an explicit opt-out survives a round-trip and is written");
    Config optOut;
    optOut.controllerAudioKeepDefaultDevice = false;
    const std::string guardText = serializeConfig(optOut);
    EXPECT(guardText.find("\"controllerAudioKeepDefaultDevice\"") != std::string::npos);
    Config guardBack;
    parseConfigInto(guardText, guardBack);
    EXPECT(!guardBack.controllerAudioKeepDefaultDevice);

    TEST("the guard is independent of the direction switches");
    Config mix;
    parseConfigInto(R"({"controllerAudioKeepDefaultDevice":false})", mix);
    EXPECT(!mix.controllerAudioKeepDefaultDevice);
    EXPECT(mix.controllerAudioMic);
    EXPECT(mix.controllerAudioSpeaker);
    EXPECT(mix.controllerAudio);
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
    test_controller_audio_default_and_persistence();
    test_controller_audio_split_directions();
    test_last_check_epoch_64bit();

    std::cout << "config_json: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
