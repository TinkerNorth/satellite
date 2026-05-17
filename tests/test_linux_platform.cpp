// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * tests/test_linux_platform.cpp — Unit tests for the Linux platform port.
 *
 * Self-contained: no external test framework required.
 * Covers the bits of platform/linux/ that are testable without root or
 * real I/O against /dev/uinput / D-Bus / a display server:
 *   - JSON helpers (jsonEscape, jsonGetString)
 *   - Config persistence round-trip (loadConfig / saveConfig)
 *   - XDG autostart .desktop file create / remove (setAutoStart / getAutoStart)
 *   - Path helpers (getExeDir, getCurrentDate, configPath)
 *
 * Each test points XDG_CONFIG_HOME at a fresh tmp dir so it never touches
 * the developer's real ~/.config/satellite or ~/.config/autostart.
 */
#include "../src/platform/linux/config.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

// ── Test harness (mirrors tests/test_session_service.cpp) ────────────────────
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

// ── Hermetic XDG_CONFIG_HOME ────────────────────────────────────────────────
// Each test creates its own tmp dir under /tmp and points XDG_CONFIG_HOME at
// it; on teardown we recursively remove it. This isolates the tests from the
// real user config and from each other.
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

// ── jsonEscape / jsonGetString ──────────────────────────────────────────────
static void testJsonEscape() {
    TEST("jsonEscape — passthrough of plain ASCII");
    EXPECT_EQ(jsonEscape("hello"), std::string("hello"));

    TEST("jsonEscape — escapes double-quote");
    EXPECT_EQ(jsonEscape("a\"b"), std::string("a\\\"b"));

    TEST("jsonEscape — escapes backslash");
    EXPECT_EQ(jsonEscape("a\\b"), std::string("a\\\\b"));

    TEST("jsonEscape — escapes newline");
    EXPECT_EQ(jsonEscape("a\nb"), std::string("a\\nb"));

    TEST("jsonEscape — handles empty string");
    EXPECT_EQ(jsonEscape(""), std::string(""));

    TEST("jsonEscape — combined special chars");
    EXPECT_EQ(jsonEscape("\"\\\n"), std::string("\\\"\\\\\\n"));

    // C0 control bytes other than \n must become \uXXXX escapes — a raw \r or
    // \t in a device name would otherwise produce invalid JSON.
    TEST("jsonEscape — escapes carriage return as \\u000d");
    EXPECT_EQ(jsonEscape("a\rb"), std::string("a\\u000db"));

    TEST("jsonEscape — escapes tab as \\u0009");
    EXPECT_EQ(jsonEscape("a\tb"), std::string("a\\u0009b"));

    TEST("jsonEscape — escapes NUL as \\u0000");
    EXPECT_EQ(jsonEscape(std::string("a\0b", 3)), std::string("a\\u0000b"));

    TEST("jsonEscape — escapes a high C0 control byte (0x1f)");
    EXPECT_EQ(jsonEscape(std::string("\x1f")), std::string("\\u001f"));

    TEST("jsonEscape — 0x20 (space) is not escaped");
    EXPECT_EQ(jsonEscape(" "), std::string(" "));
}

static void testJsonGetString() {
    TEST("jsonGetString — extracts simple value");
    std::string j = R"({"name":"alice","age":30})";
    EXPECT_EQ(jsonGetString(j, "name"), std::string("alice"));

    TEST("jsonGetString — handles whitespace around colon");
    std::string j2 = R"({ "name" : "bob" })";
    EXPECT_EQ(jsonGetString(j2, "name"), std::string("bob"));

    TEST("jsonGetString — returns empty when key absent");
    EXPECT_EQ(jsonGetString(R"({"a":"x"})", "missing"), std::string(""));

    TEST("jsonGetString — empty input returns empty");
    EXPECT_EQ(jsonGetString("", "any"), std::string(""));

    TEST("jsonGetString — handles empty value");
    EXPECT_EQ(jsonGetString(R"({"name":""})", "name"), std::string(""));
}

// ── configPath / loadConfig / saveConfig ────────────────────────────────────
static void testConfigPath() {
    TempXdg tmp;
    TEST("configPath — lives under XDG_CONFIG_HOME/satellite");
    std::string p = configPath();
    EXPECT(p.find(tmp.path) == 0);
    EXPECT(p.find("/satellite/config.json") != std::string::npos);

    TEST("configPath — creates parent dirs");
    EXPECT(fileExists(tmp.path + "/satellite"));
}

static void testConfigRoundTrip() {
    TempXdg tmp;

    Config out;
    out.udpPort = 12345;
    out.webPort = 23456;
    out.pairPort = 34567;
    out.discPort = 45678;
    out.autoStart = true;
    out.credentials = "admin:hash$abc";
    PairedDevice d;
    d.id = "device-1";
    d.name = "Pixel 7";
    d.lastIP = "192.168.1.42";
    d.pairedAt = "2025-01-15";
    d.sharedKeyHex = "deadbeef00";
    d.touchpadMode = TOUCHPAD_MODE_MOUSE;
    out.pairedDevices.push_back(d);

    saveConfig(out);

    TEST("saveConfig — writes file");
    EXPECT(fileExists(configPath()));

    Config in = loadConfig();
    TEST("loadConfig — round-trips ports");
    EXPECT_EQ(in.udpPort, out.udpPort);
    EXPECT_EQ(in.webPort, out.webPort);
    EXPECT_EQ(in.pairPort, out.pairPort);
    EXPECT_EQ(in.discPort, out.discPort);

    TEST("loadConfig — round-trips autoStart");
    EXPECT_EQ(in.autoStart, true);

    TEST("loadConfig — round-trips credentials with special chars");
    EXPECT_EQ(in.credentials, std::string("admin:hash$abc"));

    TEST("loadConfig — round-trips paired devices");
    EXPECT_EQ(in.pairedDevices.size(), size_t{1});
    if (in.pairedDevices.size() == 1) {
        EXPECT_EQ(in.pairedDevices[0].id, std::string("device-1"));
        EXPECT_EQ(in.pairedDevices[0].name, std::string("Pixel 7"));
        EXPECT_EQ(in.pairedDevices[0].lastIP, std::string("192.168.1.42"));
        EXPECT_EQ(in.pairedDevices[0].pairedAt, std::string("2025-01-15"));
        EXPECT_EQ(in.pairedDevices[0].sharedKeyHex, std::string("deadbeef00"));
        // Task 1.3 — touchpadMode persists across the save/load round-trip.
        EXPECT_EQ(static_cast<int>(in.pairedDevices[0].touchpadMode),
                  static_cast<int>(TOUCHPAD_MODE_MOUSE));
    }
}

// discoveryBroadcastEnabled (Task 1.6) — round-trips both ways, and an absent
// key defaults to true so a pre-1.6 config doesn't silently disable the
// legacy beacon. Each branch uses a fresh TempXdg so the config files don't
// alias.
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
        // A config JSON with no discoveryBroadcastEnabled key (pre-1.6 shape).
        std::ofstream f(configPath());
        f << "{\n  \"udpPort\": 9876,\n  \"pairedDevices\": []\n}\n";
        f.close();
        Config in = loadConfig();
        TEST("loadConfig — absent discoveryBroadcastEnabled key defaults to true");
        EXPECT_EQ(in.discoveryBroadcastEnabled, true);
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
    // A missing file yields struct defaults — the legacy beacon stays on.
    EXPECT_EQ(c.discoveryBroadcastEnabled, true);
}

// ── XDG autostart .desktop file ─────────────────────────────────────────────
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

// ── Path / date helpers ─────────────────────────────────────────────────────
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

// ── Driver ──────────────────────────────────────────────────────────────────
int main() {
    std::cout << "Running Linux platform tests...\n\n";

    testJsonEscape();
    testJsonGetString();
    testConfigPath();
    testConfigRoundTrip();
    testDiscoveryBroadcastConfig();
    testLoadConfigMissingFile();
    testAutoStartEnable();
    testAutoStartDisable();
    testAutoStartIdempotent();
    testGetExeDir();
    testGetCurrentDate();

    std::cout << "\n=== Test Results ===\n";
    std::cout << "  Passed: " << g_pass << "\n";
    std::cout << "  Failed: " << g_fail << "\n";
    std::cout << "  STATUS: " << (g_fail == 0 ? "ALL PASSED" : "FAILED") << "\n";
    return g_fail == 0 ? 0 : 1;
}
