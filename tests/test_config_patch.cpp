// SPDX-License-Identifier: LGPL-3.0-or-later
// The admin form's POST /api/config body applied to a Config: every key is
// present-only, the port is refused rather than clamped, and the two keys
// with a platform hook behind them report their presence.
#include "../src/core/config_patch.h"

#include <iostream>
#include <string>

#include "test_util.h"

using satellite::applyConfigPatch;
using satellite::configPatchLogLine;
using satellite::ConfigPatchOutcome;
using satellite::Json;

namespace {

Json body(const std::string& text) {
    Json j;
    satellite::jsonParse(text, j);
    return j;
}

// Every boolean the form can set, with the field it lands in.
struct BoolKey {
    const char* key;
    bool Config::* field;
};
const BoolKey BOOL_KEYS[] = {
    {"autoStart", &Config::autoStart},
    {"discoveryBroadcastEnabled", &Config::discoveryBroadcastEnabled},
    {"controllerAudio", &Config::controllerAudio},
    {"controllerAudioMic", &Config::controllerAudioMic},
    {"controllerAudioSpeaker", &Config::controllerAudioSpeaker},
    {"controllerAudioHaptics", &Config::controllerAudioHaptics},
    {"controllerAudioKeepDefaultDevice", &Config::controllerAudioKeepDefaultDevice},
    {"crashReporting", &Config::crashReporting},
};
constexpr int BOOL_KEY_COUNT = static_cast<int>(sizeof(BOOL_KEYS) / sizeof(BOOL_KEYS[0]));

Config withEveryBool(bool value) {
    Config cfg;
    for (const auto& k : BOOL_KEYS) cfg.*(k.field) = value;
    return cfg;
}

int countTrue(const Config& cfg) {
    int n = 0;
    for (const auto& k : BOOL_KEYS) n += cfg.*(k.field) ? 1 : 0;
    return n;
}

std::string oneKey(const char* key, const char* value) {
    return std::string("{\"") + key + "\":" + value + "}";
}

} // namespace

int main() {
    {
        TEST("an empty body changes nothing and rejects nothing");
        Config cfg;
        cfg.udpPort = 4321;
        cfg.networkInterface = "eth9";
        const ConfigPatchOutcome out = applyConfigPatch(body("{}"), cfg);
        EXPECT_EQ(cfg.udpPort, 4321);
        EXPECT_EQ(cfg.networkInterface, std::string("eth9"));
        EXPECT(!out.udpPortRejected);
        EXPECT(!out.autoStartPresent);
        EXPECT(!out.crashReportingPresent);
    }
    {
        TEST("udpPort at either end of the unprivileged range is applied");
        Config cfg;
        EXPECT(!applyConfigPatch(body(R"({"udpPort":1024})"), cfg).udpPortRejected);
        EXPECT_EQ(cfg.udpPort, 1024);
        EXPECT(!applyConfigPatch(body(R"({"udpPort":65535})"), cfg).udpPortRejected);
        EXPECT_EQ(cfg.udpPort, 65535);
    }
    {
        TEST("udpPort outside the range is rejected, not clamped, and the stored port stays");
        for (const char* text : {R"({"udpPort":1023})", R"({"udpPort":65536})", R"({"udpPort":0})",
                                 R"({"udpPort":-5})"}) {
            Config cfg;
            cfg.udpPort = 9876;
            EXPECT(applyConfigPatch(body(text), cfg).udpPortRejected);
            EXPECT_EQ(cfg.udpPort, 9876);
        }
    }
    {
        TEST("a wrong-typed udpPort is ignored like an absent one");
        Config cfg;
        cfg.udpPort = 9876;
        EXPECT(!applyConfigPatch(body(R"({"udpPort":"1234"})"), cfg).udpPortRejected);
        EXPECT_EQ(cfg.udpPort, 9876);
    }
    {
        TEST("each boolean key flips only its own field, and only when present");
        for (const auto& k : BOOL_KEYS) {
            Config cfg = withEveryBool(true);
            applyConfigPatch(body(oneKey(k.key, "false")), cfg);
            EXPECT(!(cfg.*(k.field)));
            EXPECT_EQ(countTrue(cfg), BOOL_KEY_COUNT - 1);

            Config back = withEveryBool(false);
            applyConfigPatch(body(oneKey(k.key, "true")), back);
            EXPECT(back.*(k.field));
            EXPECT_EQ(countTrue(back), 1);
        }
    }
    {
        TEST("a wrong-typed boolean is ignored");
        Config cfg = withEveryBool(true);
        applyConfigPatch(body(R"({"autoStart":0,"crashReporting":"no"})"), cfg);
        EXPECT_EQ(countTrue(cfg), BOOL_KEY_COUNT);
    }
    {
        TEST("autoStart and crashReporting report their presence, so the caller runs the hooks");
        Config cfg;
        const ConfigPatchOutcome both =
            applyConfigPatch(body(R"({"autoStart":true,"crashReporting":false})"), cfg);
        EXPECT(both.autoStartPresent);
        EXPECT(both.crashReportingPresent);
        EXPECT(cfg.autoStart);
        EXPECT(!cfg.crashReporting);

        const ConfigPatchOutcome neither =
            applyConfigPatch(body(R"({"controllerAudio":true})"), cfg);
        EXPECT(!neither.autoStartPresent);
        EXPECT(!neither.crashReportingPresent);
    }
    {
        TEST("networkInterface applies when present, an empty string included");
        Config cfg;
        cfg.networkInterface = "eth0";
        applyConfigPatch(body(R"({"networkInterface":"wlan0"})"), cfg);
        EXPECT_EQ(cfg.networkInterface, std::string("wlan0"));
        applyConfigPatch(body(R"({"networkInterface":""})"), cfg);
        EXPECT_EQ(cfg.networkInterface, std::string());
    }
    {
        TEST("keys the form does not own are left alone");
        Config cfg;
        cfg.webPort = 9877;
        cfg.updateChannel = "beta";
        applyConfigPatch(body(R"({"webPort":1,"updateChannel":"stable","pairedDevices":[]})"), cfg);
        EXPECT_EQ(cfg.webPort, 9877);
        EXPECT_EQ(cfg.updateChannel, std::string("beta"));
        EXPECT(cfg.pairedDevices.empty());
    }
    {
        TEST("the log line names the settings in force, and says when a port was refused");
        Config cfg;
        cfg.udpPort = 9876;
        cfg.autoStart = true;
        cfg.discoveryBroadcastEnabled = false;
        cfg.controllerAudio = true;
        cfg.controllerAudioMic = false;
        cfg.controllerAudioSpeaker = true;
        cfg.controllerAudioHaptics = false;
        const std::string line = configPatchLogLine(cfg, false);
        EXPECT_EQ(line, std::string("Config updated: udpPort=9876 autoStart=true broadcast=false "
                                    "controllerAudio=true mic=false speaker=true haptics=false"));
        EXPECT_EQ(configPatchLogLine(cfg, true), line + " (udpPort out of range, ignored)");
    }

    std::cout << "test_config_patch: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
