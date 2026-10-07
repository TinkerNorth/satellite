// SPDX-License-Identifier: LGPL-3.0-or-later
// configPath() resolves under %APPDATA%\satellite and can't be redirected via
// env var on Windows (SHGetFolderPath reads the shell-folders registry); the
// autostart tests touch HKCU\...\Run\satellite. Both side effects are made
// hermetic via snapshot/restore so the tests are safe to run locally and in CI.
#include "../src/platform/windows/config.h"
#include "../src/platform/windows/crypto.h"
#include "../src/net/pairing_keys.h"
#include "../src/platform/windows/autostart_rule.h"
#include "../src/platform/windows/tray_menu.h"
#include "../src/platform/windows/installer_launch_error.h"
#include "../src/platform/windows/update_toast.h"
#include "../src/core/update_service.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "test_util.h"

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

// Config (de)serialization moved to nlohmann/json (core/config_json.h); the
// JSON library is unit-tested by test_json. What this suite must still pin is
// that special characters in persisted strings survive a save/load round-trip
// through the public config API.
static void testConfigEscapingRoundTrip() {
    ConfigFileSnapshot snap;

    Config out;
    out.networkInterface = "Eth \"quoted\" \\ backslash";
    PairedDevice d;
    d.id = "dev-special";
    d.name = std::string("name\twith\ncontrol\x01 and \"quotes\"", 30);
    d.sharedKeyHex = "00ff";
    out.pairedDevices.push_back(d);
    saveConfig(out);

    Config in = loadConfig();
    TEST("config round-trip: quotes/backslash in networkInterface survive");
    EXPECT_EQ(in.networkInterface, out.networkInterface);
    TEST("config round-trip: quotes/backslash/control in device name survive");
    EXPECT_EQ(in.pairedDevices.size(), size_t{1});
    if (in.pairedDevices.size() == 1) EXPECT_EQ(in.pairedDevices[0].name, d.name);
}

static void testConfigPath() {
    TEST("configPath: lives under satellite\\config.json");
    std::string p = configPath();
    EXPECT(p.find("\\satellite\\config.json") != std::string::npos);
}

static void testConfigRoundTrip() {
    ConfigFileSnapshot snap;

    Config out;
    out.udpPort = 12345;
    out.webPort = 23456;
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

    TEST("saveConfig: writes file");
    EXPECT(fileExists(configPath()));

    TEST("saveConfig: leaves no .tmp sibling behind");
    EXPECT(!fileExists(configPath() + ".tmp"));

    Config in = loadConfig();
    TEST("loadConfig: round-trips ports");
    EXPECT_EQ(in.udpPort, out.udpPort);
    EXPECT_EQ(in.webPort, out.webPort);
    EXPECT_EQ(in.discPort, out.discPort);

    TEST("loadConfig: round-trips autoStart");
    EXPECT_EQ(in.autoStart, true);

    TEST("loadConfig: round-trips networkInterface and allowPublicNetwork");
    EXPECT_EQ(in.networkInterface, std::string("Ethernet 2"));
    EXPECT_EQ(in.allowPublicNetwork, true);

    TEST("loadConfig: round-trips paired devices");
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
    ConfigFileSnapshot snap;

    {
        Config out;
        out.discoveryBroadcastEnabled = true;
        saveConfig(out);
        Config in = loadConfig();
        TEST("loadConfig: discoveryBroadcastEnabled round-trips true");
        EXPECT_EQ(in.discoveryBroadcastEnabled, true);
    }
    {
        Config out;
        out.discoveryBroadcastEnabled = false;
        saveConfig(out);
        Config in = loadConfig();
        TEST("loadConfig: discoveryBroadcastEnabled round-trips false");
        EXPECT_EQ(in.discoveryBroadcastEnabled, false);
    }
    {
        // A config JSON with no discoveryBroadcastEnabled key (pre-1.6 shape).
        std::ofstream f(configPath(), std::ios::binary | std::ios::trunc);
        f << "{\r\n  \"udpPort\": 9876,\r\n  \"pairedDevices\": []\r\n}\r\n";
        f.close();
        Config in = loadConfig();
        TEST("loadConfig: absent discoveryBroadcastEnabled key defaults to true");
        EXPECT_EQ(in.discoveryBroadcastEnabled, true);

        TEST("loadConfig: absent networkInterface/allowPublicNetwork keep defaults");
        EXPECT_EQ(in.networkInterface, std::string(""));
        EXPECT_EQ(in.allowPublicNetwork, false);
    }
}

static void testAutoStartEnable() {
    AutoStartSnapshot snap;
    setAutoStart(false);

    TEST("getAutoStart: false on a clean profile");
    EXPECT_EQ(getAutoStart(), false);

    setAutoStart(true);

    TEST("getAutoStart: true after enable");
    EXPECT_EQ(getAutoStart(), true);
}

static void testAutoStartDisable() {
    AutoStartSnapshot snap;
    setAutoStart(true);
    EXPECT_EQ(getAutoStart(), true);

    setAutoStart(false);

    TEST("getAutoStart: false after disable");
    EXPECT_EQ(getAutoStart(), false);
}

static void testAutoStartIdempotent() {
    AutoStartSnapshot snap;
    setAutoStart(false);

    TEST("setAutoStart(false): no-op when not previously enabled");
    setAutoStart(false);
    EXPECT_EQ(getAutoStart(), false);

    TEST("setAutoStart(true) twice: still enabled");
    setAutoStart(true);
    setAutoStart(true);
    EXPECT_EQ(getAutoStart(), true);
}

static void testGetExeDir() {
    TEST("getExeDir: returns a non-empty path");
    std::string d = getExeDir();
    EXPECT(!d.empty());

    TEST("getExeDir: directory exists");
    if (!d.empty() && d != ".") {
        DWORD attrs = GetFileAttributesA(d.c_str());
        EXPECT(attrs != INVALID_FILE_ATTRIBUTES);
        EXPECT((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0);
    }
}

static void testGetCurrentDate() {
    TEST("getCurrentDate: returns YYYY-MM-DD");
    std::string s = getCurrentDate();
    EXPECT_EQ(s.size(), size_t{10});
    if (s.size() == 10) {
        EXPECT(s[4] == '-');
        EXPECT(s[7] == '-');
    }
}

static void testHexCodec() {
    TEST("hexEncode: known vector");
    const uint8_t in[] = {0x00, 0x0f, 0xa5, 0xff};
    EXPECT_EQ(hexEncode(in, sizeof(in)), std::string("000fa5ff"));

    TEST("hexDecode: roundtrip of hexEncode");
    uint8_t out[4] = {0};
    EXPECT(hexDecode("000fa5ff", out, sizeof(out)));
    EXPECT_EQ((int)out[0], 0x00);
    EXPECT_EQ((int)out[1], 0x0f);
    EXPECT_EQ((int)out[2], 0xa5);
    EXPECT_EQ((int)out[3], 0xff);

    TEST("hexDecode: uppercase accepted");
    uint8_t up[2] = {0};
    EXPECT(hexDecode("A5FF", up, sizeof(up)));
    EXPECT_EQ((int)up[0], 0xa5);
    EXPECT_EQ((int)up[1], 0xff);

    TEST("hexDecode: wrong length rejected");
    uint8_t two[2] = {0};
    EXPECT(!hexDecode("abc", two, sizeof(two)));

    TEST("hexDecode: non-hex rejected");
    uint8_t two2[2] = {0};
    EXPECT(!hexDecode("zz00", two2, sizeof(two2)));

    TEST("sha256hex: empty-string vector");
    EXPECT_EQ(sha256hex(""),
              std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
}

// The session-crypto interop vectors (HKDF, HMAC proof, AEAD bindings) live in
// tests/test_session_crypto.cpp, which runs on every CI lane. This suite keeps
// only the Windows-specific surfaces.

static std::string readFileBytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static void testAtomicWriteFile() {
    char tmpDir[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, tmpDir);
    std::string base = (n > 0 && n < MAX_PATH) ? std::string(tmpDir, n) : std::string();
    std::string path = base + "satellite_atomic_test.json";
    std::string tmpPath = path + ".tmp";
    DeleteFileA(path.c_str());
    DeleteFileA(tmpPath.c_str());

    TEST("atomicWriteFile: writes the exact bytes");
    EXPECT(atomicWriteFile(path, "hello world"));
    EXPECT(fileExists(path));
    EXPECT_EQ(readFileBytes(path), std::string("hello world"));

    TEST("atomicWriteFile: leaves no .tmp behind");
    EXPECT(!fileExists(tmpPath));

    TEST("atomicWriteFile: replaces existing content");
    EXPECT(atomicWriteFile(path, "second write, shorter-then-longer payload"));
    EXPECT_EQ(readFileBytes(path), std::string("second write, shorter-then-longer payload"));
    EXPECT(!fileExists(tmpPath));

    TEST("atomicWriteFile: preserves embedded NULs and newlines");
    std::string payload("a\0b\r\nc", 6);
    EXPECT(atomicWriteFile(path, payload));
    EXPECT_EQ(readFileBytes(path), payload);

    TEST("atomicWriteFile: handles empty content");
    EXPECT(atomicWriteFile(path, ""));
    EXPECT(fileExists(path));
    EXPECT_EQ(readFileBytes(path), std::string(""));

    DeleteFileA(path.c_str());
    DeleteFileA(tmpPath.c_str());
}

static void testResolvePairingSharedKey() {
    uint8_t serverPk[32], serverSk[32];
    generateKeyPair(serverPk, serverSk);

    uint8_t clientPk[32], clientSk[32];
    generateKeyPair(clientPk, clientSk);
    std::string clientPkHex = hexEncode(clientPk, 32);

    TEST("resolvePairingSharedKey: valid client key derives an ECDH key");
    std::string k1;
    PairingKeyOutcome o1 = resolvePairingSharedKey(clientPkHex, serverPk, serverSk, k1);
    EXPECT(o1 == PairingKeyOutcome::Derived);
    EXPECT_EQ(k1.size(), size_t{64});

    TEST("resolvePairingSharedKey: derivation is deterministic for fixed inputs");
    std::string k1b;
    PairingKeyOutcome o1b = resolvePairingSharedKey(clientPkHex, serverPk, serverSk, k1b);
    EXPECT(o1b == PairingKeyOutcome::Derived);
    EXPECT_EQ(k1, k1b);

    TEST("resolvePairingSharedKey: empty client key mints a random key");
    std::string k2;
    PairingKeyOutcome o2 = resolvePairingSharedKey("", serverPk, serverSk, k2);
    EXPECT(o2 == PairingKeyOutcome::Random);
    EXPECT_EQ(k2.size(), size_t{64});

    TEST("resolvePairingSharedKey: random keys differ across calls");
    std::string k2b;
    resolvePairingSharedKey("", serverPk, serverSk, k2b);
    EXPECT(k2 != k2b);

    TEST("resolvePairingSharedKey: malformed hex is rejected, not silently randomized");
    std::string k3 = "sentinel";
    PairingKeyOutcome o3 = resolvePairingSharedKey("zzzz", serverPk, serverSk, k3);
    EXPECT(o3 == PairingKeyOutcome::InvalidClientKey);
    EXPECT(k3.empty());

    TEST("resolvePairingSharedKey: wrong-length client key is rejected");
    std::string k4;
    PairingKeyOutcome o4 = resolvePairingSharedKey("abcd", serverPk, serverSk, k4);
    EXPECT(o4 == PairingKeyOutcome::InvalidClientKey);
    EXPECT(k4.empty());

    TEST("resolvePairingSharedKey: low-order (all-zero) client key is rejected");
    std::string allZero(64, '0');
    std::string k5 = "sentinel";
    PairingKeyOutcome o5 = resolvePairingSharedKey(allZero, serverPk, serverSk, k5);
    EXPECT(o5 == PairingKeyOutcome::InvalidClientKey);
    EXPECT(k5.empty());
}

static void testRunEntryRule() {
    TEST("runEntryNeedsWrite: an absent entry is written");
    EXPECT(lifecycle::runEntryNeedsWrite("", "C:\\a\\satellite.exe", false));

    TEST("runEntryNeedsWrite: an entry already pointing at this exe is rewritten, whatever "
         "its quoting or case");
    EXPECT(lifecycle::runEntryNeedsWrite("\"C:\\A\\SATELLITE.EXE\"", "C:\\a\\satellite.exe", true));
    EXPECT(lifecycle::runEntryNeedsWrite("C:\\a\\satellite.exe", "C:\\a\\satellite.exe", true));

    TEST("runEntryNeedsWrite: another exe that still exists is left alone");
    EXPECT(!lifecycle::runEntryNeedsWrite("\"C:\\other\\satellite.exe\"", "C:\\a\\satellite.exe",
                                          true));

    TEST("runEntryNeedsWrite: another exe that is gone is replaced");
    EXPECT(lifecycle::runEntryNeedsWrite("\"C:\\gone\\satellite.exe\"", "C:\\a\\satellite.exe",
                                         false));

    TEST("stripQuotes: only a matched pair comes off");
    EXPECT(lifecycle::stripQuotes("\"x\"") == "x");
    EXPECT(lifecycle::stripQuotes("\"x") == "\"x");
    EXPECT(lifecycle::stripQuotes("x") == "x");
    EXPECT(lifecycle::stripQuotes("\"") == "\"");
}

static void testUpdateMenuItem() {
    using satellite::tray::updateMenuItemFor;

    TEST("update menu: with no updater wired, the check is greyed");
    const auto none = updateMenuItemFor(false, UpdateState::Idle, false, L"");
    EXPECT(none.id == IDM_CHECK_UPDATES && (none.flags & MF_GRAYED) != 0);

    TEST("update menu: a downloaded release installs, named");
    const auto downloaded = updateMenuItemFor(true, UpdateState::Downloaded, true, L"2.2.0");
    EXPECT(downloaded.id == IDM_INSTALL_UPDATE);
    EXPECT(downloaded.label == L"Install Update 2.2.0");
    EXPECT((downloaded.flags & MF_GRAYED) == 0);

    TEST("update menu: an available release downloads, named");
    const auto available = updateMenuItemFor(true, UpdateState::UpdateAvailable, true, L"2.2.0");
    EXPECT(available.id == IDM_INSTALL_UPDATE);
    EXPECT(available.label == L"Download Update 2.2.0...");

    TEST("update menu: UpdateAvailable without an asset to offer falls back to a check");
    EXPECT(updateMenuItemFor(true, UpdateState::UpdateAvailable, false, L"").id ==
           IDM_CHECK_UPDATES);

    TEST("update menu: a download, a verify or a check in flight is greyed");
    for (const UpdateState st :
         {UpdateState::Downloading, UpdateState::Verifying, UpdateState::Checking}) {
        const auto busy = updateMenuItemFor(true, st, false, L"");
        EXPECT(busy.id == IDM_CHECK_UPDATES && (busy.flags & MF_GRAYED) != 0);
    }

    TEST("update menu: idle offers a live check");
    const auto idle = updateMenuItemFor(true, UpdateState::Idle, false, L"");
    EXPECT(idle.id == IDM_CHECK_UPDATES);
    EXPECT((idle.flags & MF_GRAYED) == 0);
    EXPECT(idle.label == L"Check for Updates...");
}

static void testInstallerLaunchError() {
    using satellite::updater::installerLaunchError;

    TEST("installer launch: a declined UAC prompt (GLE 1223) says the permission was declined");
    EXPECT_EQ(installerLaunchError(1223),
              std::string("The installer needs administrator permission, and the request was "
                          "declined."));

    TEST("installer launch: any other ShellExecuteEx failure keeps its error code");
    EXPECT_EQ(installerLaunchError(2), std::string("ShellExecuteEx failed (GLE=2)"));
}

static UpdateStatusSnapshot updateAt(UpdateState state) {
    UpdateStatusSnapshot snap;
    snap.state = state;
    snap.info.version = "2.2.0";
    snap.info.available = state != UpdateState::Idle && state != UpdateState::UpToDate;
    return snap;
}

static UpdateStatusSnapshot dismissedAt(UpdateState state) {
    UpdateStatusSnapshot snap = updateAt(state);
    snap.dismissed = true;
    return snap;
}

static std::string toastPattern(const std::vector<UpdateStatusSnapshot>& broadcasts) {
    satellite::tray::UpdateToastState toastState;
    std::string pattern;
    for (const UpdateStatusSnapshot& snap : broadcasts) {
        pattern += satellite::tray::updateToastDue(toastState, snap) ? '!' : '.';
    }
    return pattern;
}

struct ToastCase {
    const char* label;
    std::vector<UpdateStatusSnapshot> broadcasts;
    const char* pattern;
};

static void testUpdateToast() {
    const UpdateStatusSnapshot offered = updateAt(UpdateState::UpdateAvailable);
    const UpdateStatusSnapshot offeredDismissed = dismissedAt(UpdateState::UpdateAvailable);
    const UpdateStatusSnapshot checking = updateAt(UpdateState::Checking);
    const UpdateStatusSnapshot downloading = updateAt(UpdateState::Downloading);
    const ToastCase cases[] = {
        {"update toast: an offer toasts once, however often it is broadcast again",
         {offered, offered, checking, offered},
         "!..."},
        {"update toast: remind me later, then Download and Cancel, stays quiet",
         {offered, offeredDismissed, downloading, downloading, offered},
         "!...."},
        {"update toast: remind me later, then the next check that finds the update toasts once",
         {offered, offeredDismissed, checking, offered, offered},
         "!..!."},
        {"update toast: a failed check after remind me later stays quiet through a cancelled "
         "download, and the check after it reminds",
         {offered, offeredDismissed, checking, offeredDismissed, downloading, offered, checking,
          offered},
         "!......!"},
        {"update toast: Idle and UpToDate re-arm, so the next offer toasts",
         {offered, updateAt(UpdateState::Idle), offered, updateAt(UpdateState::UpToDate), offered},
         "!.!.!"},
        {"update toast: a staged, a failed or a dismissed update never toasts",
         {updateAt(UpdateState::Downloaded), updateAt(UpdateState::Error), offeredDismissed,
          dismissedAt(UpdateState::Downloaded)},
         "...."},
    };
    for (const ToastCase& toastCase : cases) {
        TEST(toastCase.label);
        EXPECT_EQ(toastPattern(toastCase.broadcasts), std::string(toastCase.pattern));
    }
}

struct ToastProbeUpdater : IUpdaterPort {
    std::atomic<bool> downloading{false};
    bool fetchLatestRelease(const std::string& channel, const std::string&, UpdateInfo& out,
                            std::string&) override {
        out.version = "99.0.0";
        out.channel = channel;
        out.assetName = "SatelliteSetup-99.0.0.exe";
        out.assetSize = 1024;
        return true;
    }
    bool downloadArtifact(const UpdateInfo&, const std::function<void(uint64_t, uint64_t)>&,
                          const std::atomic<bool>* cancel, std::string&,
                          std::string& outError) override {
        downloading = true;
        for (int i = 0; i < 3000 && cancel != nullptr && !cancel->load(); i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        outError = "cancelled";
        return false;
    }
    bool verifyArtifact(const std::string&, const UpdateInfo&, std::string&) override {
        return true;
    }
    bool applyUpdate(const std::string&, const UpdateInfo&, std::string&) override { return true; }
    std::string platformId() const override { return "windows"; }
};

struct ToastProbeLog : ILogPort {
    void logMsg(LogLevel, const std::string&, const std::string&) override {}
};

struct ToastProbe {
    std::mutex mtx;
    satellite::tray::UpdateToastState toastState;
    int toasts = 0;
};

static void probeToast(ToastProbe& probe, const UpdateStatusSnapshot& snap) {
    std::lock_guard<std::mutex> lk(probe.mtx);
    if (satellite::tray::updateToastDue(probe.toastState, snap)) probe.toasts++;
}

static int toastCount(ToastProbe& probe) {
    std::lock_guard<std::mutex> lk(probe.mtx);
    return probe.toasts;
}

static bool waitForToasts(ToastProbe& probe, int want) {
    for (int i = 0; i < 3000 && toastCount(probe) < want; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return toastCount(probe) >= want;
}

static bool waitForDownloading(const ToastProbeUpdater& updater) {
    for (int i = 0; i < 3000 && !updater.downloading.load(); i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return updater.downloading.load();
}

static void cancelThenStop(UpdateService& svc) {
    svc.cancelInFlight();
    svc.stop();
}

static void stopOnly(UpdateService& svc) { svc.stop(); }

struct ToastEnding {
    const char* label;
    void (*end)(UpdateService&);
};

static void testUpdateToastWithTheRealService() {
    const ToastEnding endings[] = {
        {"update toast: with the real service, remind me later then Download and Cancel toasts "
         "once",
         cancelThenStop},
        {"update toast: with the real service, remind me later then a download cancelled by "
         "shutdown toasts once",
         stopOnly},
    };
    for (const ToastEnding& ending : endings) {
        TEST(ending.label);
        ToastProbe probe;
        ToastProbeUpdater updater;
        ToastProbeLog log;
        Config cfg;
        std::mutex cfgMtx;
        UpdateService svc(updater, log, cfg, cfgMtx);
        svc.setStatusCallback(
            [&probe](const UpdateStatusSnapshot& snap) { probeToast(probe, snap); });
        svc.start();
        svc.requestCheck(true);
        EXPECT(waitForToasts(probe, 1));
        svc.dismiss();
        svc.requestDownload();
        EXPECT(waitForDownloading(updater));
        ending.end(svc);
        EXPECT_EQ(toastCount(probe), 1);
    }
}

int main() {
    std::cout << "Running Windows platform tests...\n\n";

    if (!sodiumInit()) {
        std::cerr << "sodium init failed\n";
        return 1;
    }

    testConfigEscapingRoundTrip();
    testConfigPath();
    testConfigRoundTrip();
    testAtomicWriteFile();
    testDiscoveryBroadcastConfig();
    testAutoStartEnable();
    testAutoStartDisable();
    testAutoStartIdempotent();
    testGetExeDir();
    testGetCurrentDate();
    testHexCodec();
    testResolvePairingSharedKey();
    testRunEntryRule();
    testUpdateMenuItem();
    testInstallerLaunchError();
    testUpdateToast();
    testUpdateToastWithTheRealService();

    std::cout << "\n=== Test Results ===\n";
    std::cout << "  Passed: " << g_pass << "\n";
    std::cout << "  Failed: " << g_fail << "\n";
    std::cout << "  STATUS: " << (g_fail == 0 ? "ALL PASSED" : "FAILED") << "\n";
    return g_fail == 0 ? 0 : 1;
}
