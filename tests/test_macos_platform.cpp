// SPDX-License-Identifier: LGPL-3.0-or-later
// macOS platform suite: config persistence under ~/Library/Application
// Support, LaunchAgent autostart, the keyfile-backed dpapi* keystore, and PIN
// rotation semantics. Mirrors test_linux_platform.cpp / the non-crypto parts
// of test_windows_platform.cpp.
//
// Hermeticity: homeDir() reads $HOME first, so every test runs with HOME
// redirected into a per-test tmpdir (config, LaunchAgents plist, and keyfile
// all land there). launchctl registration is compiled out under
// SATELLITE_BUILD_TESTS (see config.cpp) — `launchctl load -w` would mutate
// the real user's launchd override database, which no file snapshot restores.
// PIN rotation uses the backdatePinClockForTest seam, never sleeps.
#include "../src/platform/macos/config.h"
#include "../src/platform/macos/crypto.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "test_util.h"

// Per-test tmp dir as HOME, restored on teardown: isolates the tests from the
// real user profile and from each other (macOS analogue of the linux TempXdg).
struct TempHome {
    std::string path;
    std::string savedHome;
    bool hadHome = false;
    TempHome() {
        const char* h = getenv("HOME");
        hadHome = (h != nullptr);
        if (hadHome) savedHome = h;
        char tmpl[] = "/tmp/satellite-test-XXXXXX";
        char* d = mkdtemp(tmpl);
        path = (d != nullptr) ? d : "";
        setenv("HOME", path.c_str(), 1);
    }
    ~TempHome() {
        if (!path.empty()) {
            std::string cmd = "rm -rf " + path;
            int rc = system(cmd.c_str());
            (void)rc;
        }
        if (hadHome) {
            setenv("HOME", savedHome.c_str(), 1);
        } else {
            unsetenv("HOME");
        }
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
    TempHome tmp;

    Config out;
    out.networkInterface = "en0 \"quoted\" \\ backslash";
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
    TempHome tmp;
    TEST("configPath: lives under HOME/Library/Application Support/satellite");
    std::string p = configPath();
    EXPECT(p.find(tmp.path) == 0);
    EXPECT(p.find("/Library/Application Support/satellite/config.json") != std::string::npos);

    TEST("configPath: creates parent dirs");
    EXPECT(fileExists(tmp.path + "/Library/Application Support/satellite"));
}

static void testAtomicWriteFile() {
    TempHome tmp;
    std::string path = tmp.path + "/atomic_test.json";
    std::string tmpPath = path + ".tmp";

    TEST("atomicWriteFile: writes the exact bytes");
    EXPECT(atomicWriteFile(path, "hello world"));
    EXPECT(fileExists(path));
    EXPECT_EQ(slurp(path), std::string("hello world"));

    TEST("atomicWriteFile: leaves no .tmp behind");
    EXPECT(!fileExists(tmpPath));

    TEST("atomicWriteFile: replaces existing content");
    EXPECT(atomicWriteFile(path, "second payload, a different length"));
    EXPECT_EQ(slurp(path), std::string("second payload, a different length"));
    EXPECT(!fileExists(tmpPath));

    TEST("atomicWriteFile: preserves embedded NULs and newlines");
    std::string payload("a\0b\nc", 5);
    EXPECT(atomicWriteFile(path, payload));
    EXPECT_EQ(slurp(path), payload);

    TEST("atomicWriteFile: handles empty content");
    EXPECT(atomicWriteFile(path, ""));
    EXPECT(fileExists(path));
    EXPECT_EQ(slurp(path), std::string(""));
}

static void testConfigRoundTrip() {
    TempHome tmp;

    Config out;
    out.udpPort = 12345;
    out.webPort = 23456;
    out.discPort = 45678;
    out.autoStart = true;
    out.networkInterface = "en1";
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
    EXPECT_EQ(in.networkInterface, std::string("en1"));
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
    {
        TempHome tmp;
        Config out;
        out.discoveryBroadcastEnabled = true;
        saveConfig(out);
        Config in = loadConfig();
        TEST("loadConfig: discoveryBroadcastEnabled round-trips true");
        EXPECT_EQ(in.discoveryBroadcastEnabled, true);
    }
    {
        TempHome tmp;
        Config out;
        out.discoveryBroadcastEnabled = false;
        saveConfig(out);
        Config in = loadConfig();
        TEST("loadConfig: discoveryBroadcastEnabled round-trips false");
        EXPECT_EQ(in.discoveryBroadcastEnabled, false);
    }
    {
        TempHome tmp;
        // Pre-1.6 config shape: no discoveryBroadcastEnabled key.
        std::ofstream f(configPath());
        f << "{\n  \"udpPort\": 9876,\n  \"pairedDevices\": []\n}\n";
        f.close();
        Config in = loadConfig();
        TEST("loadConfig: absent discoveryBroadcastEnabled key defaults to true");
        EXPECT_EQ(in.discoveryBroadcastEnabled, true);

        TEST("loadConfig: absent networkInterface/allowPublicNetwork keep defaults");
        EXPECT_EQ(in.networkInterface, std::string(""));
        EXPECT_EQ(in.allowPublicNetwork, false);
    }
}

static void testLoadConfigMissingFile() {
    TempHome tmp;
    TEST("loadConfig: returns defaults when file missing");
    Config c = loadConfig();
    EXPECT(c.udpPort > 0);
    EXPECT(c.webPort > 0);
    EXPECT_EQ(c.autoStart, false);
    EXPECT_EQ(c.pairedDevices.size(), size_t{0});
    // Missing file yields struct defaults; the legacy beacon stays on.
    EXPECT_EQ(c.discoveryBroadcastEnabled, true);
}

static void testAutoStartEnable() {
    TempHome tmp;
    std::string plist = tmp.path + "/Library/LaunchAgents/com.tinkernorth.satellite.plist";

    TEST("getAutoStart: false on a clean profile");
    EXPECT_EQ(getAutoStart(), false);

    setAutoStart(true);

    TEST("setAutoStart(true): creates the LaunchAgent plist");
    EXPECT(fileExists(plist));

    TEST("getAutoStart: true after enable");
    EXPECT_EQ(getAutoStart(), true);

    std::string body = slurp(plist);
    TEST("setAutoStart(true): file is a v1.0 property list");
    EXPECT(body.find("<?xml version=\"1.0\"") != std::string::npos);
    EXPECT(body.find("<plist version=\"1.0\">") != std::string::npos);

    TEST("setAutoStart(true): Label is the bundle identifier");
    EXPECT(body.find("<key>Label</key><string>com.tinkernorth.satellite</string>") !=
           std::string::npos);

    TEST("setAutoStart(true): ProgramArguments points at the satellite binary");
    EXPECT(body.find("<key>ProgramArguments</key>") != std::string::npos);
    EXPECT(body.find("/satellite</string>") != std::string::npos);

    TEST("setAutoStart(true): runs at load, no keep-alive");
    EXPECT(body.find("<key>RunAtLoad</key><true/>") != std::string::npos);
    EXPECT(body.find("<key>KeepAlive</key><false/>") != std::string::npos);
}

static void testAutoStartDisable() {
    TempHome tmp;
    std::string plist = tmp.path + "/Library/LaunchAgents/com.tinkernorth.satellite.plist";

    setAutoStart(true);
    EXPECT(fileExists(plist));

    setAutoStart(false);

    TEST("setAutoStart(false): removes the plist");
    EXPECT(!fileExists(plist));

    TEST("getAutoStart: false after disable");
    EXPECT_EQ(getAutoStart(), false);
}

static void testAutoStartIdempotent() {
    TempHome tmp;
    TEST("setAutoStart(false): no-op when not previously enabled");
    setAutoStart(false);
    EXPECT_EQ(getAutoStart(), false);

    TEST("setAutoStart(true) twice: still enabled");
    setAutoStart(true);
    setAutoStart(true);
    EXPECT_EQ(getAutoStart(), true);
}

static void testGetExeDir() {
    TEST("getExeDir: returns an absolute path");
    std::string d = getExeDir();
    EXPECT(!d.empty());
    EXPECT(d[0] == '/' || d == ".");

    TEST("getExeDir: directory exists");
    if (d != ".") {
        struct stat st;
        EXPECT(stat(d.c_str(), &st) == 0);
        EXPECT(S_ISDIR(st.st_mode));
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

// Keystore parity: on macOS the dpapi* names are backed by a 0600 keyfile next
// to the config plus libsodium secretbox (see platform/posix/crypto_posix.cpp).
static void testDpapiKeystore() {
    TempHome tmp;
    const std::string secret = "0123456789abcdef0123456789abcdef";

    TEST("dpapiEncrypt: produces a non-empty blob that isn't the plaintext");
    std::string blob = dpapiEncrypt(secret);
    EXPECT(!blob.empty());
    EXPECT(blob != secret);
    EXPECT(blob.find(secret) == std::string::npos);

    TEST("dpapiEncrypt: creates the keyfile with 0600 permissions");
    std::string keyfile = tmp.path + "/Library/Application Support/satellite/keyfile";
    EXPECT(fileExists(keyfile));
    struct stat st{};
    if (stat(keyfile.c_str(), &st) == 0) EXPECT_EQ((int)(st.st_mode & 0777), 0600);

    TEST("dpapiDecrypt: round-trips the plaintext");
    EXPECT_EQ(dpapiDecrypt(blob), secret);

    TEST("dpapiEncrypt: fresh nonce per call (same plaintext, distinct blobs)");
    std::string blob2 = dpapiEncrypt(secret);
    EXPECT(blob2 != blob);
    EXPECT_EQ(dpapiDecrypt(blob2), secret);

    TEST("dpapiDecrypt: tampered blob fails closed (empty result)");
    std::string tampered = blob;
    size_t mid = tampered.size() / 2;
    tampered[mid] = (tampered[mid] == 'A') ? 'B' : 'A';
    EXPECT_EQ(dpapiDecrypt(tampered), std::string(""));

    TEST("dpapiDecrypt: garbage and empty inputs fail closed");
    EXPECT_EQ(dpapiDecrypt(""), std::string(""));
    EXPECT_EQ(dpapiDecrypt("not base64 at all"), std::string(""));

    TEST("dpapiDecrypt: fails closed when the keyfile is missing");
    unlink(keyfile.c_str());
    EXPECT_EQ(dpapiDecrypt(blob), std::string(""));
}

static void testRandomHelpers() {
    TEST("randomHex: length and charset");
    std::string h = randomHex(8);
    EXPECT_EQ(h.size(), size_t{16});
    EXPECT(h.find_first_not_of("0123456789abcdef") == std::string::npos);

    TEST("randomHex: draws fresh bytes per call");
    EXPECT(randomHex(8) != h);

    TEST("randomDigits: length and charset");
    std::string d = randomDigits(4);
    EXPECT_EQ(d.size(), size_t{4});
    EXPECT(d.find_first_not_of("0123456789") == std::string::npos);
}

// PIN rotation semantics (net/pin_rotation.cpp): a 4-digit PIN rotates every
// 5 minutes; the outgoing PIN stays valid as "previous" for one more period;
// a pair or a burst of failures burns both. All time travel goes through the
// backdatePinClockForTest seam.
//
// Collision note: a re-mint draws 4 random digits, so a fresh PIN can
// legitimately equal the one it replaces (p = 1e-4). Assertions of the form
// "the old PIN stopped working" are therefore gated on the mint actually
// having produced different digits; the guaranteed state transitions
// (previous kept/cleared, countdown restart) carry the semantics.

// Returns a 4-digit PIN guaranteed != both snapshot PINs (flips the first
// digit of current; if that collides with previous, flips one more step).
static std::string wrongPinFor(const PinSnapshot& s) {
    std::string w = s.currentPin;
    for (int step = 1; step <= 2; step++) {
        w[0] = static_cast<char>('0' + ((s.currentPin[0] - '0') + step) % 10);
        if (w != s.previousPin) return w;
    }
    return w;
}

static void testPinSnapshotInitial() {
    // Consume any prior state: a successful verify resets to a fresh pair.
    PinSnapshot pre = pinSnapshot();
    verifyPin(pre.currentPin);

    PinSnapshot s = pinSnapshot();
    TEST("pinSnapshot: current is a 4-digit PIN");
    EXPECT_EQ(s.currentPin.size(), size_t{4});
    EXPECT(s.currentPin.find_first_not_of("0123456789") == std::string::npos);

    TEST("pinSnapshot: previous is empty before the first rotation");
    EXPECT_EQ(s.previousPin, std::string(""));

    TEST("pinSnapshot: rotation countdown is within (0, 300]");
    EXPECT(s.secondsRemaining > 0);
    EXPECT(s.secondsRemaining <= 300);

    TEST("pinSnapshot: does not rotate before the period lapses");
    PinSnapshot s2 = pinSnapshot();
    EXPECT_EQ(s2.currentPin, s.currentPin);
}

static void testPinRotationKeepsPrevious() {
    verifyPin(pinSnapshot().currentPin); // fresh pair, previous empty
    PinSnapshot before = pinSnapshot();

    // One period + 10 s: due, and within the previous-PIN validity window.
    backdatePinClockForTest(310);
    PinSnapshot after = pinSnapshot();

    TEST("rotation due: outgoing PIN is kept as previous for one period");
    EXPECT_EQ(after.previousPin, before.currentPin);
    EXPECT_EQ(after.currentPin.size(), size_t{4});

    TEST("rotation due: countdown restarts");
    EXPECT(after.secondsRemaining > 290);
    EXPECT(after.secondsRemaining <= 300);

    TEST("previous PIN still verifies inside its window");
    EXPECT(verifyPin(after.previousPin));

    TEST("a consumed PIN resets the pair (previous cleared)");
    PinSnapshot reset = pinSnapshot();
    EXPECT_EQ(reset.previousPin, std::string(""));
}

static void testPinStaleRotationDropsPrevious() {
    verifyPin(pinSnapshot().currentPin); // fresh pair
    PinSnapshot before = pinSnapshot();

    // Three periods past mint: the PIN went stale a full period ago, so the
    // old current must NOT survive as previous.
    backdatePinClockForTest(900);
    PinSnapshot after = pinSnapshot();

    TEST("stale rotation: does not keep a long-expired PIN as previous");
    EXPECT_EQ(after.previousPin, std::string(""));

    TEST("stale rotation: the expired PIN no longer verifies");
    // Gated on the re-mint actually differing (see the collision note above).
    if (after.currentPin != before.currentPin) EXPECT(!verifyPin(before.currentPin));
}

static void testVerifyPinConsumesAndFlashes() {
    PinSnapshot s = pinSnapshot();

    TEST("verifyPin: accepts the current PIN");
    EXPECT(verifyPin(s.currentPin));

    TEST("verifyPin: a consumed PIN cannot be replayed");
    // Gated on the re-mint actually differing (see the collision note above).
    if (pinSnapshot().currentPin != s.currentPin) EXPECT(!verifyPin(s.currentPin));

    TEST("pinSnapshot: flashes PinPaired right after a pair");
    PinSnapshot paired = pinSnapshot();
    EXPECT(paired.state == PinState::PinPaired);

    TEST("pinSnapshot: PinPaired flash decays back to PinActive");
    backdatePinClockForTest(6); // PIN_PAIRED_HOLD_SEC is 5
    PinSnapshot decayed = pinSnapshot();
    EXPECT(decayed.state == PinState::PinActive);
}

static void testPinRejectsWrongGuesses() {
    verifyPin(pinSnapshot().currentPin); // fresh pair
    PinSnapshot s = pinSnapshot();

    TEST("verifyPin: rejects a wrong 4-digit guess");
    EXPECT(!verifyPin(wrongPinFor(s)));

    TEST("verifyPin: rejects wrong-length input");
    EXPECT(!verifyPin(""));
    EXPECT(!verifyPin("123"));
    EXPECT(!verifyPin("99999"));

    TEST("verifyPin: the real PIN still works after scattered failures");
    EXPECT(verifyPin(pinSnapshot().currentPin));
}

static void testPinBurnsAfterMaxFails() {
    verifyPin(pinSnapshot().currentPin); // fresh pair, fail count 0
    PinSnapshot s = pinSnapshot();

    // PIN_MAX_FAILS is 5: the 5th wrong guess burns both PINs.
    for (int i = 0; i < 5; i++) (void)verifyPin("99999");

    TEST("burn: five wrong guesses reset the pair");
    PinSnapshot burned = pinSnapshot();
    EXPECT_EQ(burned.previousPin, std::string(""));
    EXPECT_EQ(burned.currentPin.size(), size_t{4});

    TEST("burn: the pre-burn PIN no longer verifies");
    // Gated on the re-mint actually differing (see the collision note above).
    if (burned.currentPin != s.currentPin) EXPECT(!verifyPin(s.currentPin));
}

int main() {
    std::cout << "Running macOS platform tests...\n\n";

    if (!sodiumInit()) {
        std::cerr << "sodium init failed\n";
        return 1;
    }

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
    testHexCodec();
    testDpapiKeystore();
    testRandomHelpers();
    testPinSnapshotInitial();
    testPinRotationKeepsPrevious();
    testPinStaleRotationDropsPrevious();
    testVerifyPinConsumesAndFlashes();
    testPinRejectsWrongGuesses();
    testPinBurnsAfterMaxFails();

    std::cout << "\n=== Test Results ===\n";
    std::cout << "  Passed: " << g_pass << "\n";
    std::cout << "  Failed: " << g_fail << "\n";
    std::cout << "  STATUS: " << (g_fail == 0 ? "ALL PASSED" : "FAILED") << "\n";
    return g_fail == 0 ? 0 : 1;
}
