// SPDX-License-Identifier: LGPL-3.0-or-later
#include "../src/platform/linux/config.h"
#include "../src/platform/linux/gamepad_adapter.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "test_util.h"

// Per-test tmp dir as XDG_CONFIG_HOME, removed on teardown: isolates tests from
// the real user config and from each other.
struct TempXdg {
    std::string path;
    TempXdg() {
        char tmpl[] = "/tmp/satellite-test-XXXXXX";
        char* d = mkdtemp(tmpl);
        path = (d != nullptr) ? d : "";
        setenv("XDG_CONFIG_HOME", path.c_str(), 1);
    }
    ~TempXdg() {
        if (!path.empty()) {
            std::string cmd = "rm -rf " + path;
            int rc = system(cmd.c_str());
            (void)rc;
        }
        unsetenv("XDG_CONFIG_HOME");
    }
};

static bool fileExists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}
static std::string slurp(const std::string& p) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Config (de)serialization moved to nlohmann/json (core/config_json.h); the
// JSON library is unit-tested by test_json. What this suite must still pin is
// that special characters in persisted strings survive a save/load round-trip
// through the public config API.
static void testConfigEscapingRoundTrip() {
    TempXdg tmp;

    Config out;
    out.networkInterface = "Eth \"quoted\" \\ backslash";
    PairedDevice d;
    d.id = "dev-special";
    d.name = std::string("name\twith\ncontrol\x01 and \"quotes\"", 30);
    d.sharedKeyHex = "00ff";
    out.pairedDevices.push_back(d);
    saveConfig(out);

    Config in = loadConfig();
    TEST("config round-trip — quotes/backslash in networkInterface survive");
    EXPECT_EQ(in.networkInterface, out.networkInterface);
    TEST("config round-trip — quotes/backslash/control in device name survive");
    EXPECT_EQ(in.pairedDevices.size(), size_t{1});
    if (in.pairedDevices.size() == 1) EXPECT_EQ(in.pairedDevices[0].name, d.name);
}

static void testConfigPath() {
    TempXdg tmp;
    TEST("configPath — lives under XDG_CONFIG_HOME/satellite");
    std::string p = configPath();
    EXPECT(p.find(tmp.path) == 0);
    EXPECT(p.find("/satellite/config.json") != std::string::npos);

    TEST("configPath — creates parent dirs");
    EXPECT(fileExists(tmp.path + "/satellite"));
}

static void testAtomicWriteFile() {
    TempXdg tmp;
    std::string path = tmp.path + "/atomic_test.json";
    std::string tmpPath = path + ".tmp";

    TEST("atomicWriteFile — writes the exact bytes");
    EXPECT(atomicWriteFile(path, "hello world"));
    EXPECT(fileExists(path));
    EXPECT_EQ(slurp(path), std::string("hello world"));

    TEST("atomicWriteFile — leaves no .tmp behind");
    EXPECT(!fileExists(tmpPath));

    TEST("atomicWriteFile — replaces existing content");
    EXPECT(atomicWriteFile(path, "second payload, a different length"));
    EXPECT_EQ(slurp(path), std::string("second payload, a different length"));
    EXPECT(!fileExists(tmpPath));

    TEST("atomicWriteFile — preserves embedded NULs and newlines");
    std::string payload("a\0b\nc", 5);
    EXPECT(atomicWriteFile(path, payload));
    EXPECT_EQ(slurp(path), payload);

    TEST("atomicWriteFile — handles empty content");
    EXPECT(atomicWriteFile(path, ""));
    EXPECT(fileExists(path));
    EXPECT_EQ(slurp(path), std::string(""));
}

static void testConfigRoundTrip() {
    TempXdg tmp;

    Config out;
    out.udpPort = 12345;
    out.webPort = 23456;
    out.pairPort = 34567;
    out.discPort = 45678;
    out.autoStart = true;
    out.networkInterface = "Ethernet 2";
    out.allowPublicNetwork = true;
    PairedDevice d;
    d.id = "device-1";
    d.name = "Pixel 7";
    d.lastIP = "192.168.1.42";
    d.pairedAt = "2025-01-15";
    d.sharedKeyHex = "deadbeef00";
    out.pairedDevices.push_back(d);

    saveConfig(out);

    TEST("saveConfig — writes file");
    EXPECT(fileExists(configPath()));

    TEST("saveConfig — leaves no .tmp sibling behind");
    EXPECT(!fileExists(configPath() + ".tmp"));

    Config in = loadConfig();
    TEST("loadConfig — round-trips ports");
    EXPECT_EQ(in.udpPort, out.udpPort);
    EXPECT_EQ(in.webPort, out.webPort);
    EXPECT_EQ(in.pairPort, out.pairPort);
    EXPECT_EQ(in.discPort, out.discPort);

    TEST("loadConfig — round-trips autoStart");
    EXPECT_EQ(in.autoStart, true);

    TEST("loadConfig — round-trips networkInterface and allowPublicNetwork");
    EXPECT_EQ(in.networkInterface, std::string("Ethernet 2"));
    EXPECT_EQ(in.allowPublicNetwork, true);

    TEST("loadConfig — round-trips paired devices");
    EXPECT_EQ(in.pairedDevices.size(), size_t{1});
    if (in.pairedDevices.size() == 1) {
        EXPECT_EQ(in.pairedDevices[0].id, std::string("device-1"));
        EXPECT_EQ(in.pairedDevices[0].name, std::string("Pixel 7"));
        EXPECT_EQ(in.pairedDevices[0].lastIP, std::string("192.168.1.42"));
        EXPECT_EQ(in.pairedDevices[0].pairedAt, std::string("2025-01-15"));
        EXPECT_EQ(in.pairedDevices[0].sharedKeyHex, std::string("deadbeef00"));
    }
}

// An absent discoveryBroadcastEnabled key must default to true so a pre-1.6
// config doesn't silently disable the legacy beacon.
static void testDiscoveryBroadcastConfig() {
    {
        TempXdg tmp;
        Config out;
        out.discoveryBroadcastEnabled = true;
        saveConfig(out);
        Config in = loadConfig();
        TEST("loadConfig — discoveryBroadcastEnabled round-trips true");
        EXPECT_EQ(in.discoveryBroadcastEnabled, true);
    }
    {
        TempXdg tmp;
        Config out;
        out.discoveryBroadcastEnabled = false;
        saveConfig(out);
        Config in = loadConfig();
        TEST("loadConfig — discoveryBroadcastEnabled round-trips false");
        EXPECT_EQ(in.discoveryBroadcastEnabled, false);
    }
    {
        TempXdg tmp;
        // Pre-1.6 config shape: no discoveryBroadcastEnabled key.
        std::ofstream f(configPath());
        f << "{\n  \"udpPort\": 9876,\n  \"pairedDevices\": []\n}\n";
        f.close();
        Config in = loadConfig();
        TEST("loadConfig — absent discoveryBroadcastEnabled key defaults to true");
        EXPECT_EQ(in.discoveryBroadcastEnabled, true);

        TEST("loadConfig — absent networkInterface/allowPublicNetwork keep defaults");
        EXPECT_EQ(in.networkInterface, std::string(""));
        EXPECT_EQ(in.allowPublicNetwork, false);
    }
}

static void testLoadConfigMissingFile() {
    TempXdg tmp;
    TEST("loadConfig — returns defaults when file missing");
    Config c = loadConfig();
    EXPECT(c.udpPort > 0);
    EXPECT(c.webPort > 0);
    EXPECT_EQ(c.autoStart, false);
    EXPECT_EQ(c.pairedDevices.size(), size_t{0});
    // Missing file yields struct defaults — the legacy beacon stays on.
    EXPECT_EQ(c.discoveryBroadcastEnabled, true);
}

static void testAutoStartEnable() {
    TempXdg tmp;
    std::string desktop = tmp.path + "/autostart/satellite.desktop";

    TEST("getAutoStart — false on a clean profile");
    EXPECT_EQ(getAutoStart(), false);

    setAutoStart(true);

    TEST("setAutoStart(true) — creates the .desktop file");
    EXPECT(fileExists(desktop));

    TEST("getAutoStart — true after enable");
    EXPECT_EQ(getAutoStart(), true);

    std::string body = slurp(desktop);
    TEST("setAutoStart(true) — file is a Desktop Entry");
    EXPECT(body.find("[Desktop Entry]") != std::string::npos);
    EXPECT(body.find("Type=Application") != std::string::npos);

    TEST("setAutoStart(true) — Exec line points at the satellite binary");
    EXPECT(body.find("Exec=") != std::string::npos);
    EXPECT(body.find("/satellite") != std::string::npos);

    TEST("setAutoStart(true) — Name is APP_TITLE");
    EXPECT(body.find(std::string("Name=") + APP_TITLE) != std::string::npos);

    TEST("setAutoStart(true) — opts into GNOME autostart");
    EXPECT(body.find("X-GNOME-Autostart-enabled=true") != std::string::npos);
}

static void testAutoStartDisable() {
    TempXdg tmp;
    std::string desktop = tmp.path + "/autostart/satellite.desktop";

    setAutoStart(true);
    EXPECT(fileExists(desktop));

    setAutoStart(false);

    TEST("setAutoStart(false) — removes the .desktop file");
    EXPECT(!fileExists(desktop));

    TEST("getAutoStart — false after disable");
    EXPECT_EQ(getAutoStart(), false);
}

static void testAutoStartIdempotent() {
    TempXdg tmp;
    TEST("setAutoStart(false) — no-op when not previously enabled");
    setAutoStart(false);
    EXPECT_EQ(getAutoStart(), false);

    TEST("setAutoStart(true) twice — still enabled");
    setAutoStart(true);
    setAutoStart(true);
    EXPECT_EQ(getAutoStart(), true);
}

static void testGetExeDir() {
    TEST("getExeDir — returns an absolute path");
    std::string d = getExeDir();
    EXPECT(!d.empty());
    EXPECT(d[0] == '/' || d == ".");

    TEST("getExeDir — directory exists");
    if (d != ".") {
        struct stat st;
        EXPECT(stat(d.c_str(), &st) == 0);
        EXPECT(S_ISDIR(st.st_mode));
    }
}

static void testGetCurrentDate() {
    TEST("getCurrentDate — returns YYYY-MM-DD");
    std::string s = getCurrentDate();
    EXPECT_EQ(s.size(), size_t{10});
    if (s.size() == 10) {
        EXPECT(s[4] == '-');
        EXPECT(s[7] == '-');
    }
}

// SATELLITE_SYSFS_PROXY_DIR redirects the uinput sysfs-proxy writes into a
// tmpdir so the test never touches /tmp/satellite (which a real daemon may use).
struct TempProxyDir {
    std::string path;
    TempProxyDir() {
        char tmpl[] = "/tmp/satellite-proxy-XXXXXX";
        char* d = mkdtemp(tmpl);
        path = (d != nullptr) ? d : "";
        setenv("SATELLITE_SYSFS_PROXY_DIR", path.c_str(), 1);
    }
    ~TempProxyDir() {
        if (!path.empty()) {
            std::string cmd = "rm -rf " + path;
            int rc = system(cmd.c_str());
            (void)rc;
        }
        unsetenv("SATELLITE_SYSFS_PROXY_DIR");
    }
};

static void testSubmitBatteryWritesProxyFile() {
    TempProxyDir tmp;
    GamepadAdapter adapter;

    BatteryReport r;
    r.level = 73;
    r.status = BATTERY_STATUS_DISCHARGING;
    bool ok = adapter.submitBattery(/*serial=*/7, r);

    TEST("submitBattery — returns true on successful proxy write");
    EXPECT(ok);

    std::string path = tmp.path + "/controller7/battery";
    TEST("submitBattery — creates per-controller battery file");
    EXPECT(fileExists(path));

    std::string body = slurp(path);
    TEST("submitBattery — file contains wire-encoded level + status");
    EXPECT(body.find("level=73") != std::string::npos);
    EXPECT(body.find("status=1") != std::string::npos);
}

static void testSubmitBatteryUnknownLevel() {
    TempProxyDir tmp;
    GamepadAdapter adapter;

    BatteryReport r;
    r.level = BATTERY_LEVEL_UNKNOWN; // 0xFF / 255
    r.status = BATTERY_STATUS_UNKNOWN;
    bool ok = adapter.submitBattery(/*serial=*/3, r);

    TEST("submitBattery — accepts BATTERY_LEVEL_UNKNOWN sentinel");
    EXPECT(ok);
    std::string body = slurp(tmp.path + "/controller3/battery");
    EXPECT(body.find("level=255") != std::string::npos);
    EXPECT(body.find("status=0") != std::string::npos);
}

static void testSetLightbarCallbackWritesProxyFile() {
    TempProxyDir tmp;
    GamepadAdapter adapter;

    int innerCalls = 0;
    uint32_t gotSerial = 0;
    uint8_t gotR = 0, gotG = 0, gotB = 0;
    adapter.setLightbarCallback([&](uint32_t serial, uint8_t r, uint8_t g, uint8_t b) {
        ++innerCalls;
        gotSerial = serial;
        gotR = r;
        gotG = g;
        gotB = b;
    });

    // Install alone must not synthesize an invocation — nothing drives the sink
    // until a colour change actually fires.
    TEST("setLightbarCallback — install does not synchronously invoke inner sink");
    EXPECT_EQ(innerCalls, 0);

    adapter.invokeLightbarForTest(/*serial=*/5, 0xFF, 0x80, 0xC0);

    TEST("setLightbarCallback — wrapper forwards to inner sink");
    EXPECT_EQ(innerCalls, 1);
    EXPECT_EQ(gotSerial, uint32_t{5});
    EXPECT_EQ(int(gotR), 0xFF);
    EXPECT_EQ(int(gotG), 0x80);
    EXPECT_EQ(int(gotB), 0xC0);

    std::string path = tmp.path + "/controller5/lightbar";
    TEST("setLightbarCallback — wrapper writes RRGGBB to per-controller lightbar file");
    EXPECT(fileExists(path));
    std::string body = slurp(path);
    EXPECT_EQ(body, std::string("FF80C0\n"));
}

static void testSysfsProxyDirEnvOverride() {
    TempProxyDir tmp;
    TEST("sysfsProxyDir — honours SATELLITE_SYSFS_PROXY_DIR override");
    EXPECT_EQ(GamepadAdapter::sysfsProxyDir(), tmp.path);
}

int main() {
    std::cout << "Running Linux platform tests...\n\n";

    testConfigEscapingRoundTrip();
    testConfigPath();
    testAtomicWriteFile();
    testConfigRoundTrip();
    testDiscoveryBroadcastConfig();
    testLoadConfigMissingFile();
    testAutoStartEnable();
    testAutoStartDisable();
    testAutoStartIdempotent();
    testGetExeDir();
    testGetCurrentDate();
    testSubmitBatteryWritesProxyFile();
    testSubmitBatteryUnknownLevel();
    testSetLightbarCallbackWritesProxyFile();
    testSysfsProxyDirEnvOverride();

    std::cout << "\n=== Test Results ===\n";
    std::cout << "  Passed: " << g_pass << "\n";
    std::cout << "  Failed: " << g_fail << "\n";
    std::cout << "  STATUS: " << (g_fail == 0 ? "ALL PASSED" : "FAILED") << "\n";
    return g_fail == 0 ? 0 : 1;
}
