// SPDX-License-Identifier: LGPL-3.0-or-later
// configPath() resolves under %APPDATA%\satellite and can't be redirected via
// env var on Windows (SHGetFolderPath reads the shell-folders registry); the
// autostart tests touch HKCU\...\Run\satellite. Both side effects are made
// hermetic via snapshot/restore so the tests are safe to run locally and in CI.
#include "../src/platform/windows/config.h"

#include <fstream>
#include <iostream>
#include <sstream>
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

// Captures any existing HKCU Run-key value for APP_NAME so the registry tests
// can flip it freely and put it back afterwards.
struct AutoStartSnapshot {
    bool existed = false;
    std::string value;
    AutoStartSnapshot() {
        HKEY key = nullptr;
        const char* run = "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run";
        if (RegOpenKeyExA(HKEY_CURRENT_USER, run, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
            return;
        }
        DWORD type = 0, size = 0;
        if (RegQueryValueExA(key, APP_NAME, nullptr, &type, nullptr, &size) == ERROR_SUCCESS &&
            type == REG_SZ && size > 0) {
            std::string buf(size, '\0');
            if (RegQueryValueExA(key, APP_NAME, nullptr, &type, reinterpret_cast<BYTE*>(&buf[0]),
                                 &size) == ERROR_SUCCESS) {
                if (!buf.empty() && buf.back() == '\0') buf.pop_back();
                existed = true;
                value = buf;
            }
        }
        RegCloseKey(key);
    }
    ~AutoStartSnapshot() {
        HKEY key = nullptr;
        const char* run = "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run";
        if (RegOpenKeyExA(HKEY_CURRENT_USER, run, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
            return;
        }
        if (existed) {
            RegSetValueExA(key, APP_NAME, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                           static_cast<DWORD>(value.size()) + 1);
        } else {
            RegDeleteValueA(key, APP_NAME);
        }
        RegCloseKey(key);
    }
};

// configPath() can't be redirected on Windows, so save/restore whatever's
// already on disk to keep the round-trip test hermetic.
struct ConfigFileSnapshot {
    std::string path;
    bool existed = false;
    std::string content;
    ConfigFileSnapshot() : path(configPath()) {
        std::ifstream f(path, std::ios::binary);
        if (f.is_open()) {
            std::stringstream ss;
            ss << f.rdbuf();
            content = ss.str();
            existed = true;
        }
    }
    ~ConfigFileSnapshot() {
        if (existed) {
            std::ofstream f(path, std::ios::binary | std::ios::trunc);
            f.write(content.data(), static_cast<std::streamsize>(content.size()));
        } else {
            DeleteFileA(path.c_str());
        }
    }
};

static bool fileExists(const std::string& p) {
    DWORD attrs = GetFileAttributesA(p.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

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

static void testConfigPath() {
    TEST("configPath — lives under satellite\\config.json");
    std::string p = configPath();
    EXPECT(p.find("\\satellite\\config.json") != std::string::npos);
}

static void testConfigRoundTrip() {
    ConfigFileSnapshot snap;

    Config out;
    out.udpPort = 12345;
    out.webPort = 23456;
    out.pairPort = 34567;
    out.discPort = 45678;
    out.autoStart = true;
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

    TEST("loadConfig — round-trips paired devices");
    EXPECT_EQ(in.pairedDevices.size(), size_t{1});
    if (in.pairedDevices.size() == 1) {
        EXPECT_EQ(in.pairedDevices[0].id, std::string("device-1"));
        EXPECT_EQ(in.pairedDevices[0].name, std::string("Pixel 7"));
        EXPECT_EQ(in.pairedDevices[0].lastIP, std::string("192.168.1.42"));
        EXPECT_EQ(in.pairedDevices[0].pairedAt, std::string("2025-01-15"));
        EXPECT_EQ(in.pairedDevices[0].sharedKeyHex, std::string("deadbeef00"));
        EXPECT_EQ(static_cast<int>(in.pairedDevices[0].touchpadMode),
                  static_cast<int>(TOUCHPAD_MODE_MOUSE));
    }
}

// An absent discoveryBroadcastEnabled key must default to true so a pre-1.6
// config doesn't silently disable the legacy beacon.
static void testDiscoveryBroadcastConfig() {
    ConfigFileSnapshot snap;

    {
        Config out;
        out.discoveryBroadcastEnabled = true;
        saveConfig(out);
        Config in = loadConfig();
        TEST("loadConfig — discoveryBroadcastEnabled round-trips true");
        EXPECT_EQ(in.discoveryBroadcastEnabled, true);
    }
    {
        Config out;
        out.discoveryBroadcastEnabled = false;
        saveConfig(out);
        Config in = loadConfig();
        TEST("loadConfig — discoveryBroadcastEnabled round-trips false");
        EXPECT_EQ(in.discoveryBroadcastEnabled, false);
    }
    {
        // A config JSON with no discoveryBroadcastEnabled key (pre-1.6 shape).
        std::ofstream f(configPath(), std::ios::binary | std::ios::trunc);
        f << "{\r\n  \"udpPort\": 9876,\r\n  \"pairedDevices\": []\r\n}\r\n";
        f.close();
        Config in = loadConfig();
        TEST("loadConfig — absent discoveryBroadcastEnabled key defaults to true");
        EXPECT_EQ(in.discoveryBroadcastEnabled, true);
    }
}

static void testAutoStartEnable() {
    AutoStartSnapshot snap;
    setAutoStart(false);

    TEST("getAutoStart — false on a clean profile");
    EXPECT_EQ(getAutoStart(), false);

    setAutoStart(true);

    TEST("getAutoStart — true after enable");
    EXPECT_EQ(getAutoStart(), true);
}

static void testAutoStartDisable() {
    AutoStartSnapshot snap;
    setAutoStart(true);
    EXPECT_EQ(getAutoStart(), true);

    setAutoStart(false);

    TEST("getAutoStart — false after disable");
    EXPECT_EQ(getAutoStart(), false);
}

static void testAutoStartIdempotent() {
    AutoStartSnapshot snap;
    setAutoStart(false);

    TEST("setAutoStart(false) — no-op when not previously enabled");
    setAutoStart(false);
    EXPECT_EQ(getAutoStart(), false);

    TEST("setAutoStart(true) twice — still enabled");
    setAutoStart(true);
    setAutoStart(true);
    EXPECT_EQ(getAutoStart(), true);
}

static void testGetExeDir() {
    TEST("getExeDir — returns a non-empty path");
    std::string d = getExeDir();
    EXPECT(!d.empty());

    TEST("getExeDir — directory exists");
    if (!d.empty() && d != ".") {
        DWORD attrs = GetFileAttributesA(d.c_str());
        EXPECT(attrs != INVALID_FILE_ATTRIBUTES);
        EXPECT((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0);
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

int main() {
    std::cout << "Running Windows platform tests...\n\n";

    testJsonEscape();
    testJsonGetString();
    testConfigPath();
    testConfigRoundTrip();
    testDiscoveryBroadcastConfig();
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
