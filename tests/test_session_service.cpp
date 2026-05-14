// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * tests/test_session_service.cpp — Unit tests for SessionService.
 *
 * Self-contained: no external test framework required.
 * Build:  build-tests.bat
 * Run:    test_session_service.exe
 */
#include "../src/core/session_service.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <sstream>
#include <thread>
#include <chrono>

// ── Counters & state for assertions ─────────────────────────────────────────
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

// ── Mock IGamepadPort ───────────────────────────────────────────────────────
struct MockViGem : IGamepadPort {
    bool busOpen = false;
    bool ensureBusReturnVal = true;
    bool pluginReturnVal = true;
    bool submitReturnVal = true;
    bool driverInstalled = true;

    int ensureBusCalls = 0;
    int closeBusCalls = 0;
    int pluginCalls = 0;
    int pluginDS4Calls = 0;
    int unplugCalls = 0;
    int submitCalls = 0;
    int submitDS4Calls = 0;
    int setRumbleCallbackCalls = 0;

    std::vector<uint32_t> pluggedSerials;
    std::vector<uint32_t> unpluggedSerials;
    GamepadReport lastSubmittedReport{};
    // Captured rumble callback. Tests synthesize "the platform fired a
    // notification" by invoking this directly via `fireRumble(serial, r)`.
    RumbleCallback capturedRumbleCb;

    bool ensureBusOpen() override {
        ensureBusCalls++;
        if (ensureBusReturnVal) busOpen = true;
        return ensureBusReturnVal;
    }
    void closeBus() override {
        closeBusCalls++;
        busOpen = false;
    }
    bool isBusOpen() const override { return busOpen; }
    bool pluginDevice(uint32_t serial) override {
        pluginCalls++;
        pluggedSerials.push_back(serial);
        return pluginReturnVal;
    }
    bool pluginDeviceDS4(uint32_t serial) override {
        pluginDS4Calls++;
        pluggedSerials.push_back(serial);
        return pluginReturnVal;
    }
    void unplugDevice(uint32_t serial) override {
        unplugCalls++;
        unpluggedSerials.push_back(serial);
    }
    bool submitReport(uint32_t, const GamepadReport& r) override {
        submitCalls++;
        lastSubmittedReport = r;
        return submitReturnVal;
    }
    bool submitDS4Report(uint32_t, const GamepadReport& r) override {
        submitDS4Calls++;
        lastSubmittedReport = r;
        return submitReturnVal;
    }
    void setRumbleCallback(RumbleCallback cb) override {
        setRumbleCallbackCalls++;
        capturedRumbleCb = std::move(cb);
    }
    // Helper for tests: simulate the platform firing a rumble notification.
    void fireRumble(uint32_t serial, const RumbleReport& r) {
        if (capturedRumbleCb) capturedRumbleCb(serial, r);
    }

    // Custom reset that preserves no state — copy/move-assigning the mock
    // would clobber the std::function which is fine since tests reset
    // between cases. Note: we don't use `*this = MockViGem{}` because that
    // would also wipe `setRumbleCallbackCalls` which tests sometimes care
    // about across phases of a single case.
    void reset() { *this = MockViGem{}; }
};

// ── Mock IClientPort ────────────────────────────────────────────────────────
struct MockClient : IClientPort {
    int updateAddrCalls = 0;
    int removeAddrCalls = 0;
    int heartbeatAckCalls = 0;
    int controllerAckCalls = 0;
    int serverStatusCalls = 0;
    int broadcastCalls = 0;
    int rumbleCalls = 0;

    // Last controller ACK params
    uint16_t lastAckType = 0;
    uint8_t lastAckCtrl = 0;
    uint8_t lastAckResult = 0;

    // Last rumble dispatch params (from sendRumble).
    uint32_t lastRumbleConnToken = 0;
    uint8_t lastRumbleCtrlIdx = 0;
    RumbleReport lastRumble{};

    void updateClientAddr(uint32_t, const std::string&, uint16_t) override { updateAddrCalls++; }
    void removeClientAddr(uint32_t) override { removeAddrCalls++; }
    void sendHeartbeatAck(const Connection&) override { heartbeatAckCalls++; }
    void sendControllerAck(const Connection&, uint16_t t, uint8_t c, uint8_t r) override {
        controllerAckCalls++;
        lastAckType = t;
        lastAckCtrl = c;
        lastAckResult = r;
    }
    void sendServerStatus(const Connection&, bool, uint8_t) override { serverStatusCalls++; }
    void broadcastServerStatus(const std::vector<std::pair<uint32_t, const Connection*>>&, bool,
                               uint8_t) override {
        broadcastCalls++;
    }
    void sendRumble(const Connection& conn, uint8_t ctrlIdx, const RumbleReport& report) override {
        rumbleCalls++;
        lastRumbleConnToken = conn.token;
        lastRumbleCtrlIdx = ctrlIdx;
        lastRumble = report;
    }

    void reset() { *this = MockClient{}; }
};

// ── Mock ILogPort ───────────────────────────────────────────────────────────
struct MockLog : ILogPort {
    int logCalls = 0;
    std::vector<std::string> messages;

    void logMsg(LogLevel, const std::string&, const std::string& msg) override {
        logCalls++;
        messages.push_back(msg);
    }
    void reset() { *this = MockLog{}; }
};

// ── Helper ──────────────────────────────────────────────────────────────────
static const uint8_t TEST_KEY[CRYPTO_KEY_SIZE] = {1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11,
                                                  12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22,
                                                  23, 24, 25, 26, 27, 28, 29, 30, 31, 32};

static OpenSessionResult openTestSession(SessionService& svc, const std::string& devId = "dev1",
                                         const std::string& devName = "TestDevice") {
    return svc.openSession(devId, devName, "192.168.1.100", TEST_KEY);
}

// ── Tests ───────────────────────────────────────────────────────────────────

static void test_openSession_basic() {
    TEST("openSession — basic");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    EXPECT(r.ok);
    EXPECT(r.token != 0);
    EXPECT_EQ(r.availableSlots, 16);
    EXPECT(r.error.empty());
    EXPECT(log.logCalls > 0);
}

static void test_openSession_replacesStale() {
    TEST("openSession — replaces stale connection for same deviceId");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r1 = openTestSession(svc);
    auto r2 = openTestSession(svc); // same deviceId
    EXPECT(r2.ok);
    EXPECT(r2.token != r1.token);
    // Only one connection should remain
    auto snap = svc.getConnectionsSnapshot();
    EXPECT_EQ((int)snap.connections.size(), 1);
}

static void test_openSession_multipleDevices() {
    TEST("openSession — multiple different devices");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r1 = svc.openSession("dev1", "D1", "1.2.3.4", TEST_KEY);
    auto r2 = svc.openSession("dev2", "D2", "1.2.3.5", TEST_KEY);
    EXPECT(r1.ok);
    EXPECT(r2.ok);
    auto snap = svc.getConnectionsSnapshot();
    EXPECT_EQ((int)snap.connections.size(), 2);
}

static void test_closeSession_basic() {
    TEST("closeSession — removes connection");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    int removed = svc.closeSession(r.token);
    EXPECT_EQ(removed, 0); // no controllers were active
    EXPECT(!svc.isDeviceConnected("dev1"));
}

static void test_closeSession_withControllers() {
    TEST("closeSession — unplugs active controllers");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    svc.handleControllerAdd(r.token, 1);
    EXPECT_EQ(svc.totalActiveControllers(), 2);

    int removed = svc.closeSession(r.token);
    EXPECT_EQ(removed, 2);
    EXPECT_EQ(vigem.unplugCalls, 2);
    EXPECT_EQ(svc.totalActiveControllers(), 0);
}

static void test_closeSession_invalidToken() {
    TEST("closeSession — invalid token returns -1");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    EXPECT_EQ(svc.closeSession(99999), -1);
}

static void test_closeAllSessions() {
    TEST("closeAllSessions — clears everything");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    svc.openSession("d1", "D1", "1.1.1.1", TEST_KEY);
    svc.openSession("d2", "D2", "1.1.1.2", TEST_KEY);
    svc.closeAllSessions();
    auto snap = svc.getConnectionsSnapshot();
    EXPECT_EQ((int)snap.connections.size(), 0);
    EXPECT_EQ(svc.availableSlots(), 16);
}

static void test_handleControllerAdd_success() {
    TEST("handleControllerAdd — success path");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    EXPECT_EQ(vigem.ensureBusCalls, 1);
    EXPECT_EQ(vigem.pluginCalls, 1);
    EXPECT_EQ(client.lastAckResult, ACK_OK);
    EXPECT_EQ(svc.totalActiveControllers(), 1);
    EXPECT_EQ(svc.availableSlots(), 15);
}

static void test_handleControllerAdd_alreadyExists() {
    TEST("handleControllerAdd — already exists");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    client.reset();
    svc.handleControllerAdd(r.token, 0); // duplicate
    EXPECT_EQ(client.lastAckResult, ACK_ERR_ALREADY_EXISTS);
    EXPECT_EQ(svc.totalActiveControllers(), 1); // still 1
}

static void test_handleControllerAdd_backendUnavailable() {
    TEST("handleControllerAdd — backend bus unavailable");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    vigem.ensureBusReturnVal = false;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    EXPECT_EQ(client.lastAckResult, ACK_ERR_BACKEND_UNAVAIL);
    EXPECT_EQ(svc.totalActiveControllers(), 0);
}

static void test_handleControllerAdd_noSlots() {
    TEST("handleControllerAdd — no serial slots left");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    // Fill all 16 slots across multiple sessions
    for (int i = 0; i < 16; i++) {
        auto r = svc.openSession("dev" + std::to_string(i), "D" + std::to_string(i), "1.1.1.1",
                                 TEST_KEY);
        svc.handleControllerAdd(r.token, 0);
    }
    EXPECT_EQ(svc.availableSlots(), 0);

    // 17th should fail
    auto r2 = svc.openSession("devX", "DX", "1.1.1.1", TEST_KEY);
    svc.handleControllerAdd(r2.token, 0);
    EXPECT_EQ(client.lastAckResult, ACK_ERR_NO_SLOTS);
}

static void test_handleControllerAdd_pluginFail() {
    TEST("handleControllerAdd — plugin fails");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    vigem.pluginReturnVal = false;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    EXPECT_EQ(client.lastAckResult, ACK_ERR_PLUGIN_FAIL);
    EXPECT_EQ(svc.availableSlots(), 16); // serial released
}

static void test_handleControllerAdd_invalidToken() {
    TEST("handleControllerAdd — invalid token");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    svc.handleControllerAdd(99999, 0);
    EXPECT_EQ(client.controllerAckCalls, 0); // nothing sent
}

static void test_handleControllerAdd_outOfBounds() {
    TEST("handleControllerAdd — ctrlIdx out of bounds");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 20); // out of bounds
    EXPECT_EQ(client.controllerAckCalls, 0);
    EXPECT_EQ(vigem.pluginCalls, 0);
}

static void test_handleControllerRemove_success() {
    TEST("handleControllerRemove — success");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    EXPECT_EQ(svc.totalActiveControllers(), 1);

    svc.handleControllerRemove(r.token, 0);
    EXPECT_EQ(client.lastAckResult, ACK_OK);
    EXPECT_EQ(client.lastAckType, MSG_CONTROLLER_REMOVE);
    EXPECT_EQ(svc.totalActiveControllers(), 0);
    EXPECT_EQ(vigem.unplugCalls, 1);
}

static void test_handleControllerRemove_notActive() {
    TEST("handleControllerRemove — controller not active");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerRemove(r.token, 0);
    EXPECT_EQ(client.lastAckResult, ACK_ERR_NOT_FOUND);
}

static void test_handleControllerRemove_closesBusWhenIdle() {
    TEST("handleControllerRemove — closes bus when no controllers left");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    EXPECT(vigem.busOpen);

    svc.handleControllerRemove(r.token, 0);
    EXPECT(!vigem.busOpen); // bus should close
    EXPECT_EQ(vigem.closeBusCalls, 1);
}

static void test_handleGamepadData_success() {
    TEST("handleGamepadData — submits report");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);

    GamepadReport rpt{};
    rpt.wButtons = 0x1234;
    rpt.sThumbLX = 5000;
    EXPECT(svc.handleGamepadData(r.token, 0, rpt));
    EXPECT_EQ(vigem.lastSubmittedReport.wButtons, (uint16_t)0x1234);
    EXPECT_EQ(vigem.lastSubmittedReport.sThumbLX, (int16_t)5000);
}

static void test_handleGamepadData_invalidToken() {
    TEST("handleGamepadData — invalid token");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    GamepadReport rpt{};
    EXPECT(!svc.handleGamepadData(99999, 0, rpt));
}

static void test_handleGamepadData_inactiveController() {
    TEST("handleGamepadData — inactive controller");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    // No controller added
    GamepadReport rpt{};
    EXPECT(!svc.handleGamepadData(r.token, 0, rpt));
}

static void test_handleGamepadData_outOfBounds() {
    TEST("handleGamepadData — ctrlIdx out of bounds");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    GamepadReport rpt{};
    EXPECT(!svc.handleGamepadData(r.token, 20, rpt));
}

static void test_handleHeartbeat() {
    TEST("handleHeartbeat — sends ACK and status");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleHeartbeat(r.token);
    EXPECT_EQ(client.heartbeatAckCalls, 1);
    EXPECT_EQ(client.serverStatusCalls, 1);
}

static void test_handleHeartbeat_invalidToken() {
    TEST("handleHeartbeat — invalid token is no-op");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    svc.handleHeartbeat(99999);
    EXPECT_EQ(client.heartbeatAckCalls, 0);
}

static void test_getDecryptInfo() {
    TEST("getDecryptInfo — returns key and counter");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    uint8_t outKey[CRYPTO_KEY_SIZE] = {};
    uint32_t outCounter = 0;
    EXPECT(svc.getDecryptInfo(r.token, outKey, outCounter));
    EXPECT(std::memcmp(outKey, TEST_KEY, CRYPTO_KEY_SIZE) == 0);
    EXPECT_EQ(outCounter, (uint32_t)0);
}

static void test_getDecryptInfo_invalidToken() {
    TEST("getDecryptInfo — invalid token returns false");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    uint8_t outKey[CRYPTO_KEY_SIZE] = {};
    uint32_t outCounter = 0;
    EXPECT(!svc.getDecryptInfo(99999, outKey, outCounter));
}

static void test_updatePostDecrypt() {
    TEST("updatePostDecrypt — updates counter and address");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.updatePostDecrypt(r.token, 42, "10.0.0.1", 5555);
    EXPECT_EQ(client.updateAddrCalls, 1);

    uint8_t k[CRYPTO_KEY_SIZE];
    uint32_t c = 0;
    svc.getDecryptInfo(r.token, k, c);
    EXPECT_EQ(c, (uint32_t)42);
}

static void test_isDeviceConnected() {
    TEST("isDeviceConnected");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    EXPECT(!svc.isDeviceConnected("dev1"));
    auto r = openTestSession(svc);
    EXPECT(svc.isDeviceConnected("dev1"));
    svc.closeSession(r.token);
    EXPECT(!svc.isDeviceConnected("dev1"));
}

static void test_getConnectionsSnapshot() {
    TEST("getConnectionsSnapshot");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto snap0 = svc.getConnectionsSnapshot();
    EXPECT_EQ((int)snap0.connections.size(), 0);
    EXPECT_EQ(snap0.maxControllers, MAX_BACKEND_CONTROLLERS);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    auto snap1 = svc.getConnectionsSnapshot();
    EXPECT_EQ((int)snap1.connections.size(), 1);
    EXPECT_EQ(snap1.totalControllers, 1);
    EXPECT_EQ(snap1.connections[0].deviceName, std::string("TestDevice"));
    EXPECT_EQ(snap1.connections[0].activeControllerCount, 1);
    EXPECT_EQ((int)snap1.connections[0].controllers.size(), 1);
}

static void test_stats() {
    TEST("isBackendAvailable / totalActiveControllers / availableSlots");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    EXPECT(!svc.isBackendAvailable());
    EXPECT_EQ(svc.totalActiveControllers(), 0);
    EXPECT_EQ(svc.availableSlots(), 16);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    EXPECT(svc.isBackendAvailable());
    EXPECT_EQ(svc.totalActiveControllers(), 1);
    EXPECT_EQ(svc.availableSlots(), 15);
}

static void test_serialRecycling() {
    TEST("serial recycling after controller remove");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    EXPECT_EQ(svc.availableSlots(), 15);
    svc.handleControllerRemove(r.token, 0);
    EXPECT_EQ(svc.availableSlots(), 16);
    // Adding again should succeed (serial recycled)
    svc.handleControllerAdd(r.token, 0);
    EXPECT_EQ(client.lastAckResult, ACK_OK);
    EXPECT_EQ(svc.availableSlots(), 15);
}

static void test_broadcastOnControllerChange() {
    TEST("broadcast status on controller add/remove");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    EXPECT(client.broadcastCalls >= 1);
    int prevBroadcasts = client.broadcastCalls;
    svc.handleControllerRemove(r.token, 0);
    EXPECT(client.broadcastCalls > prevBroadcasts);
}

static void test_staleReplacementCleansUpControllers() {
    TEST("openSession stale replacement unplugs old controllers");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r1 = openTestSession(svc);
    svc.handleControllerAdd(r1.token, 0);
    svc.handleControllerAdd(r1.token, 1);
    EXPECT_EQ(vigem.pluginCalls, 2);

    // Replace with same deviceId
    auto r2 = openTestSession(svc);
    EXPECT_EQ(vigem.unplugCalls, 2); // old controllers unplugged
    EXPECT_EQ(svc.totalActiveControllers(), 0);
}

// ═══════════════════════════════════════════════════════════════════════════
// CONTROLLER TYPE TESTS
// ═══════════════════════════════════════════════════════════════════════════
//
// These tests document the controller type system introduced to support
// multiple virtual device types through ViGEm. Currently Xbox (X360)
// and PlayStation (DS4) are the two supported types.
//
// Key behaviors:
//   • Each controller defaults to CONTROLLER_TYPE_XBOX (value 0).
//   • Setting type to CONTROLLER_TYPE_PLAYSTATION (value 1) causes the
//     ViGEm virtual device to be replugged as a DualShock 4.
//   • Switching back replugs as Xbox 360.
//   • Same-family type changes do NOT replug (no-op at ViGEm level).
//   • Invalid type values are clamped to XBOX.
//   • Gamepad data is routed to submitDS4Report when type is PlayStation.
// ═══════════════════════════════════════════════════════════════════════════

// ── types.h helper functions ────────────────────────────────────────────────

static void test_controllerTypeName_xbox() {
    TEST("controllerTypeName — XBOX returns 'xbox'");
    EXPECT_EQ(std::string(controllerTypeName(CONTROLLER_TYPE_XBOX)), std::string("xbox"));
}

static void test_controllerTypeName_playstation() {
    TEST("controllerTypeName — PLAYSTATION returns 'playstation'");
    EXPECT_EQ(std::string(controllerTypeName(CONTROLLER_TYPE_PLAYSTATION)),
              std::string("playstation"));
}

static void test_controllerTypeName_outOfRange() {
    TEST("controllerTypeName — out-of-range values default to 'xbox'");
    EXPECT_EQ(std::string(controllerTypeName(255)), std::string("xbox"));
    EXPECT_EQ(std::string(controllerTypeName(CONTROLLER_TYPE_COUNT)), std::string("xbox"));
    EXPECT_EQ(std::string(controllerTypeName(99)), std::string("xbox"));
}

static void test_controllerTypeLabel_xbox() {
    TEST("controllerTypeLabel — XBOX returns 'Xbox'");
    EXPECT_EQ(std::string(controllerTypeLabel(CONTROLLER_TYPE_XBOX)), std::string("Xbox"));
}

static void test_controllerTypeLabel_playstation() {
    TEST("controllerTypeLabel — PLAYSTATION returns 'PlayStation'");
    EXPECT_EQ(std::string(controllerTypeLabel(CONTROLLER_TYPE_PLAYSTATION)),
              std::string("PlayStation"));
}

static void test_controllerTypeLabel_outOfRange() {
    TEST("controllerTypeLabel — out-of-range values default to 'Xbox'");
    EXPECT_EQ(std::string(controllerTypeLabel(255)), std::string("Xbox"));
    EXPECT_EQ(std::string(controllerTypeLabel(CONTROLLER_TYPE_COUNT)), std::string("Xbox"));
}

static void test_controllerTypeUsesDS4_xbox() {
    TEST("controllerTypeUsesDS4 — XBOX returns false");
    EXPECT(!controllerTypeUsesDS4(CONTROLLER_TYPE_XBOX));
}

static void test_controllerTypeUsesDS4_playstation() {
    TEST("controllerTypeUsesDS4 — PLAYSTATION returns true");
    EXPECT(controllerTypeUsesDS4(CONTROLLER_TYPE_PLAYSTATION));
}

static void test_controllerTypeUsesDS4_outOfRange() {
    TEST("controllerTypeUsesDS4 — out-of-range values return false");
    EXPECT(!controllerTypeUsesDS4(255));
    EXPECT(!controllerTypeUsesDS4(CONTROLLER_TYPE_COUNT));
}

static void test_controllerType_constants() {
    TEST("controller type constants — values and count are correct");
    EXPECT_EQ((int)CONTROLLER_TYPE_XBOX, 0);
    EXPECT_EQ((int)CONTROLLER_TYPE_PLAYSTATION, 1);
    EXPECT_EQ((int)CONTROLLER_TYPE_COUNT, 2);
}

// ── handleControllerType — basic behavior ───────────────────────────────────

static void test_handleControllerType_setsTypeAndBroadcasts() {
    TEST("handleControllerType — sets type in state and broadcasts to clients");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    int prevBroadcasts = client.broadcastCalls;

    svc.handleControllerType(r.token, 0, CONTROLLER_TYPE_PLAYSTATION);
    EXPECT(client.broadcastCalls > prevBroadcasts);

    auto snap = svc.getConnectionsSnapshot();
    EXPECT_EQ((int)snap.connections[0].controllers.size(), 1);
    EXPECT_EQ(snap.connections[0].controllers[0].controllerType, CONTROLLER_TYPE_PLAYSTATION);
}

static void test_handleControllerType_invalidToken() {
    TEST("handleControllerType — invalid token is silently ignored");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    int prevBroadcasts = client.broadcastCalls;
    svc.handleControllerType(99999, 0, CONTROLLER_TYPE_PLAYSTATION);
    EXPECT_EQ(client.broadcastCalls, prevBroadcasts); // no broadcast
    EXPECT_EQ(vigem.unplugCalls, 0);                  // no unplug
}

static void test_handleControllerType_outOfBoundsCtrlIdx() {
    TEST("handleControllerType — ctrlIdx >= MAX_CONTROLLERS_PER_CONN is ignored");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    int prevBroadcasts = client.broadcastCalls;

    svc.handleControllerType(r.token, MAX_CONTROLLERS_PER_CONN, CONTROLLER_TYPE_PLAYSTATION);
    svc.handleControllerType(r.token, 255, CONTROLLER_TYPE_PLAYSTATION);
    EXPECT_EQ(client.broadcastCalls, prevBroadcasts); // nothing happened
}

static void test_handleControllerType_inactiveControllerIgnored() {
    TEST("handleControllerType — inactive (not added) controller is ignored");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    // Don't add any controller
    int prevBroadcasts = client.broadcastCalls;
    svc.handleControllerType(r.token, 0, CONTROLLER_TYPE_PLAYSTATION);
    EXPECT_EQ(client.broadcastCalls, prevBroadcasts);
    EXPECT_EQ(vigem.unplugCalls, 0);
}

static void test_handleControllerType_invalidValueClampsToXbox() {
    TEST("handleControllerType — out-of-range type value clamps to XBOX");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);

    svc.handleControllerType(r.token, 0, 255);
    auto snap = svc.getConnectionsSnapshot();
    EXPECT_EQ(snap.connections[0].controllers[0].controllerType, CONTROLLER_TYPE_XBOX);
}

static void test_handleControllerType_boundaryValueClampsToXbox() {
    TEST("handleControllerType — CONTROLLER_TYPE_COUNT (2) clamps to XBOX");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);

    svc.handleControllerType(r.token, 0, CONTROLLER_TYPE_COUNT);
    auto snap = svc.getConnectionsSnapshot();
    EXPECT_EQ(snap.connections[0].controllers[0].controllerType, CONTROLLER_TYPE_XBOX);
}

// ── handleControllerType — replug behavior (Xbox ↔ DS4) ─────────────────────

static void test_handleControllerType_xboxToPlaystationReplugsAsDS4() {
    TEST("handleControllerType — Xbox→PlayStation unplugs Xbox, replugs as DS4");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    EXPECT_EQ(vigem.pluginCalls, 1); // initial Xbox plugin
    EXPECT_EQ(vigem.pluginDS4Calls, 0);

    svc.handleControllerType(r.token, 0, CONTROLLER_TYPE_PLAYSTATION);
    EXPECT_EQ(vigem.unplugCalls, 1);    // unplugged Xbox
    EXPECT_EQ(vigem.pluginDS4Calls, 1); // replugged as DS4
    EXPECT_EQ(vigem.pluginCalls, 1);    // Xbox plugin count unchanged
}

static void test_handleControllerType_playstationToXboxReplugsAsXbox() {
    TEST("handleControllerType — PlayStation→Xbox unplugs DS4, replugs as Xbox");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    svc.handleControllerType(r.token, 0, CONTROLLER_TYPE_PLAYSTATION);
    int prevUnplugs = vigem.unplugCalls;
    int prevPluginXbox = vigem.pluginCalls;

    svc.handleControllerType(r.token, 0, CONTROLLER_TYPE_XBOX);
    EXPECT_EQ(vigem.unplugCalls, prevUnplugs + 1);    // unplugged DS4
    EXPECT_EQ(vigem.pluginCalls, prevPluginXbox + 1); // replugged as Xbox
}

static void test_handleControllerType_xboxToXboxNoReplug() {
    TEST("handleControllerType — Xbox→Xbox does NOT replug");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    int prevUnplugs = vigem.unplugCalls;

    svc.handleControllerType(r.token, 0, CONTROLLER_TYPE_XBOX);
    EXPECT_EQ(vigem.unplugCalls, prevUnplugs); // no replug
}

static void test_handleControllerType_playstationToPlaystationNoReplug() {
    TEST("handleControllerType — PlayStation→PlayStation does NOT replug");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    svc.handleControllerType(r.token, 0, CONTROLLER_TYPE_PLAYSTATION);
    int prevUnplugs = vigem.unplugCalls;
    int prevDS4 = vigem.pluginDS4Calls;

    svc.handleControllerType(r.token, 0, CONTROLLER_TYPE_PLAYSTATION);
    EXPECT_EQ(vigem.unplugCalls, prevUnplugs); // no replug
    EXPECT_EQ(vigem.pluginDS4Calls, prevDS4);  // no new DS4 plugin
}

static void test_handleControllerType_replugPreservesSerial() {
    TEST("handleControllerType — replug reuses the same ViGEm serial number");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    // Get the serial assigned
    auto snap1 = svc.getConnectionsSnapshot();
    uint32_t originalSerial = snap1.connections[0].controllers[0].serial;

    svc.handleControllerType(r.token, 0, CONTROLLER_TYPE_PLAYSTATION);
    // The unplug should be the same serial
    EXPECT_EQ(vigem.unpluggedSerials.back(), originalSerial);
    // The new plugin should also reuse the same serial
    EXPECT_EQ(vigem.pluggedSerials.back(), originalSerial);

    auto snap2 = svc.getConnectionsSnapshot();
    EXPECT_EQ(snap2.connections[0].controllers[0].serial, originalSerial);
}

static void test_handleControllerType_replugFailureLogsError() {
    TEST("handleControllerType — replug failure is logged as error");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);

    // Make the DS4 plugin fail
    vigem.pluginReturnVal = false;
    svc.handleControllerType(r.token, 0, CONTROLLER_TYPE_PLAYSTATION);

    // Should have logged an error about the failed replug
    bool foundError = false;
    for (auto& msg : log.messages) {
        if (msg.find("Failed to replug") != std::string::npos) {
            foundError = true;
            break;
        }
    }
    EXPECT(foundError);
}

static void test_handleControllerType_multipleRapidSwitches() {
    TEST("handleControllerType — rapid Xbox↔PS↔Xbox↔PS is stable");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);

    svc.handleControllerType(r.token, 0, CONTROLLER_TYPE_PLAYSTATION); // replug
    svc.handleControllerType(r.token, 0, CONTROLLER_TYPE_XBOX);        // replug
    svc.handleControllerType(r.token, 0, CONTROLLER_TYPE_PLAYSTATION); // replug
    svc.handleControllerType(r.token, 0, CONTROLLER_TYPE_XBOX);        // replug

    EXPECT_EQ(vigem.unplugCalls, 4);    // 4 replugs
    EXPECT_EQ(vigem.pluginDS4Calls, 2); // 2 DS4 plugins
    EXPECT_EQ(vigem.pluginCalls, 3);    // 1 initial + 2 Xbox replugs

    auto snap = svc.getConnectionsSnapshot();
    EXPECT_EQ(snap.connections[0].controllers[0].controllerType, CONTROLLER_TYPE_XBOX);
}

// ── handleControllerAdd — DS4-aware plugin ──────────────────────────────────

static void test_handleControllerAdd_defaultTypeIsXbox() {
    TEST("handleControllerAdd — default type is XBOX, uses pluginDevice (not DS4)");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);

    EXPECT_EQ(vigem.pluginCalls, 1);
    EXPECT_EQ(vigem.pluginDS4Calls, 0);

    auto snap = svc.getConnectionsSnapshot();
    EXPECT_EQ(snap.connections[0].controllers[0].controllerType, CONTROLLER_TYPE_XBOX);
}

static void test_handleControllerAdd_presetPlaystationType() {
    TEST("handleControllerAdd — if type was pre-set to PlayStation, uses DS4 plugin");
    // NOTE: In the current flow, the controller type is set AFTER add via a
    // separate MSG_CONTROLLER_TYPE message. But if the Controller struct's type
    // were somehow pre-set, add should respect it. This tests that code path.
    // Currently controllers always start as XBOX, so add always uses Xbox plugin.
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    // Controller type defaults to XBOX, so add should use Xbox plugin
    svc.handleControllerAdd(r.token, 0);
    EXPECT_EQ(vigem.pluginCalls, 1);
    EXPECT_EQ(vigem.pluginDS4Calls, 0);
}

static void test_handleControllerAdd_thenSetPlaystation_fullFlow() {
    TEST("Full flow: add controller → set PlayStation type → DS4 replug");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    // 1. Open session
    auto r = openTestSession(svc);
    EXPECT(r.ok);

    // 2. Add controller (starts as Xbox)
    svc.handleControllerAdd(r.token, 0);
    EXPECT_EQ(client.lastAckResult, ACK_OK);
    EXPECT_EQ(vigem.pluginCalls, 1);
    EXPECT_EQ(vigem.pluginDS4Calls, 0);

    // 3. Change type to PlayStation
    svc.handleControllerType(r.token, 0, CONTROLLER_TYPE_PLAYSTATION);
    EXPECT_EQ(vigem.unplugCalls, 1);    // unplug Xbox
    EXPECT_EQ(vigem.pluginDS4Calls, 1); // plugin DS4

    // 4. Submit gamepad data — should route to DS4
    GamepadReport rpt{};
    rpt.wButtons = 0x1000;
    EXPECT(svc.handleGamepadData(r.token, 0, rpt));
    EXPECT_EQ(vigem.submitDS4Calls, 1);
    EXPECT_EQ(vigem.submitCalls, 0);

    // 5. Snapshot should reflect PlayStation type
    auto snap = svc.getConnectionsSnapshot();
    EXPECT_EQ(snap.connections[0].controllers[0].controllerType, CONTROLLER_TYPE_PLAYSTATION);
}

// ── Gamepad data routing — Xbox vs DS4 ──────────────────────────────────────

static void test_gamepadData_xboxTypeUsesSubmitReport() {
    TEST("handleGamepadData — Xbox type uses submitReport (not DS4)");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);

    GamepadReport rpt{};
    rpt.wButtons = 0x1234;
    rpt.sThumbLX = 5000;
    svc.handleGamepadData(r.token, 0, rpt);

    EXPECT_EQ(vigem.submitCalls, 1);
    EXPECT_EQ(vigem.submitDS4Calls, 0);
    EXPECT_EQ(vigem.lastSubmittedReport.wButtons, (uint16_t)0x1234);
}

static void test_gamepadData_playstationTypeUsesSubmitDS4Report() {
    TEST("handleGamepadData — PlayStation type uses submitDS4Report");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    svc.handleControllerType(r.token, 0, CONTROLLER_TYPE_PLAYSTATION);

    GamepadReport rpt{};
    rpt.wButtons = 0x1000;
    rpt.bLeftTrigger = 200;
    svc.handleGamepadData(r.token, 0, rpt);

    EXPECT_EQ(vigem.submitDS4Calls, 1);
    EXPECT_EQ(vigem.submitCalls, 0);
    // Verify the raw report is passed through to the adapter for conversion
    EXPECT_EQ(vigem.lastSubmittedReport.bLeftTrigger, (uint8_t)200);
}

static void test_gamepadData_routingSwitchesWithType() {
    TEST("handleGamepadData — routing changes when controller type changes");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);

    GamepadReport rpt{};

    // Initially Xbox → submitReport
    svc.handleGamepadData(r.token, 0, rpt);
    EXPECT_EQ(vigem.submitCalls, 1);
    EXPECT_EQ(vigem.submitDS4Calls, 0);

    // Switch to PlayStation → submitDS4Report
    svc.handleControllerType(r.token, 0, CONTROLLER_TYPE_PLAYSTATION);
    svc.handleGamepadData(r.token, 0, rpt);
    EXPECT_EQ(vigem.submitDS4Calls, 1);
    EXPECT_EQ(vigem.submitCalls, 1); // unchanged

    // Switch back to Xbox → submitReport
    svc.handleControllerType(r.token, 0, CONTROLLER_TYPE_XBOX);
    svc.handleGamepadData(r.token, 0, rpt);
    EXPECT_EQ(vigem.submitCalls, 2);
    EXPECT_EQ(vigem.submitDS4Calls, 1); // unchanged
}

// ── Snapshot — controller type field ────────────────────────────────────────

static void test_snapshot_defaultControllerType() {
    TEST("getConnectionsSnapshot — new controller has XBOX type in snapshot");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);

    auto snap = svc.getConnectionsSnapshot();
    EXPECT_EQ((int)snap.connections[0].controllers.size(), 1);
    EXPECT_EQ(snap.connections[0].controllers[0].controllerType, CONTROLLER_TYPE_XBOX);
}

static void test_snapshot_reflectsTypeChange() {
    TEST("getConnectionsSnapshot — reflects type change to PlayStation");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    svc.handleControllerType(r.token, 0, CONTROLLER_TYPE_PLAYSTATION);

    auto snap = svc.getConnectionsSnapshot();
    EXPECT_EQ(snap.connections[0].controllers[0].controllerType, CONTROLLER_TYPE_PLAYSTATION);
}

static void test_snapshot_multipleControllersWithDifferentTypes() {
    TEST("getConnectionsSnapshot — multiple controllers can have different types");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    svc.handleControllerAdd(r.token, 1);
    svc.handleControllerType(r.token, 1, CONTROLLER_TYPE_PLAYSTATION);

    auto snap = svc.getConnectionsSnapshot();
    EXPECT_EQ((int)snap.connections[0].controllers.size(), 2);
    EXPECT_EQ(snap.connections[0].controllers[0].controllerType, CONTROLLER_TYPE_XBOX);
    EXPECT_EQ(snap.connections[0].controllers[1].controllerType, CONTROLLER_TYPE_PLAYSTATION);
}

// ── Session lifecycle with DS4 ──────────────────────────────────────────────

static void test_closeSession_unplugsDS4Controllers() {
    TEST("closeSession — correctly unplugs DS4 (PlayStation) controllers");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    svc.handleControllerType(r.token, 0, CONTROLLER_TYPE_PLAYSTATION);
    int prevUnplugs = vigem.unplugCalls; // 1 from replug

    svc.closeSession(r.token);
    EXPECT_EQ(vigem.unplugCalls, prevUnplugs + 1); // teardown unplug
    EXPECT_EQ(svc.totalActiveControllers(), 0);
    EXPECT_EQ(svc.availableSlots(), 16);
}

static void test_closeAllSessions_withMixedTypes() {
    TEST("closeAllSessions — unplugs both Xbox and DS4 controllers");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r1 = svc.openSession("dev1", "D1", "1.1.1.1", TEST_KEY);
    svc.handleControllerAdd(r1.token, 0); // Xbox

    auto r2 = svc.openSession("dev2", "D2", "1.1.1.2", TEST_KEY);
    svc.handleControllerAdd(r2.token, 0);
    svc.handleControllerType(r2.token, 0, CONTROLLER_TYPE_PLAYSTATION); // DS4

    svc.closeAllSessions();
    EXPECT_EQ(svc.totalActiveControllers(), 0);
    EXPECT_EQ(svc.availableSlots(), 16);
}

static void test_staleReplacement_unplugsDS4Controller() {
    TEST("openSession stale replacement — unplugs DS4 controller from old session");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r1 = openTestSession(svc);
    svc.handleControllerAdd(r1.token, 0);
    svc.handleControllerType(r1.token, 0, CONTROLLER_TYPE_PLAYSTATION);
    int prevUnplugs = vigem.unplugCalls; // 1 from replug

    // Reconnect same device — stale session torn down
    auto r2 = openTestSession(svc);
    EXPECT(r2.ok);
    EXPECT(r2.token != r1.token);
    EXPECT_EQ(vigem.unplugCalls, prevUnplugs + 1); // old DS4 unplugged
    EXPECT_EQ(svc.totalActiveControllers(), 0);
}

static void test_controllerRemove_thenReaddRetainsTypeFromSameSlot() {
    TEST("Controller remove + re-add — retains type from previous slot state");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    svc.handleControllerType(r.token, 0, CONTROLLER_TYPE_PLAYSTATION);

    svc.handleControllerRemove(r.token, 0);
    svc.handleControllerAdd(r.token, 0); // re-add same slot

    // Controller struct persists within the connection, so type is retained
    auto snap = svc.getConnectionsSnapshot();
    EXPECT_EQ(snap.connections[0].controllers[0].controllerType, CONTROLLER_TYPE_PLAYSTATION);
    // Re-add should use DS4 plugin since type was retained
    EXPECT_EQ(vigem.pluginDS4Calls, 2); // 1 from replug + 1 from re-add
}

// ── MSG_CONTROLLER_TYPE protocol constant ───────────────────────────────────

static void test_msgControllerType_constant() {
    TEST("MSG_CONTROLLER_TYPE — has expected wire value 0x0008");
    EXPECT_EQ(MSG_CONTROLLER_TYPE, (uint16_t)0x0008);
}

// ═══════════════════════════════════════════════════════════════════════════
// RUMBLE TESTS
// ═══════════════════════════════════════════════════════════════════════════
//
// Rumble is the reverse-direction message (satellite → dish): the platform
// gamepad backend (ViGEm/uinput) fires a notification when a game sets
// vibration on the virtual device, the SessionService maps the backend
// `serial` back to a (Connection, ctrlIdx), coalesces against the prior
// value, and dispatches via IClientPort::sendRumble.
//
// Tests below cover:
//   • Protocol constant value
//   • Callback installation by the constructor
//   • Routing serial → connection/controller
//   • Coalescing (don't re-emit identical reports)
//   • Wire duration stamping
//   • Stray/orphan notifications drop silently
//   • Full add → rumble → remove lifecycle
//   • Lightbar-bearing (DS4) reports preserve flag + RGB through the service
// ═══════════════════════════════════════════════════════════════════════════

static void test_msgRumble_constant() {
    TEST("MSG_RUMBLE — has expected wire value 0x0009");
    EXPECT_EQ(MSG_RUMBLE, (uint16_t)0x0009);
}

static void test_constructor_installsRumbleCallback() {
    TEST("SessionService constructor — installs IGamepadPort rumble callback");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    EXPECT_EQ(vigem.setRumbleCallbackCalls, 1);
    EXPECT(static_cast<bool>(vigem.capturedRumbleCb));
}

static void test_handleRumbleFromBackend_routesToOwningConnection() {
    TEST("handleRumbleFromBackend — dispatches to the connection owning the serial");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 3); // ctrlIdx 3
    auto snap = svc.getConnectionsSnapshot();
    uint32_t serial = snap.connections[0].controllers[0].serial;

    RumbleReport rr{};
    rr.strongMagnitude = 32768;
    rr.weakMagnitude = 16384;
    vigem.fireRumble(serial, rr);

    EXPECT_EQ(client.rumbleCalls, 1);
    EXPECT_EQ(client.lastRumbleConnToken, r.token);
    EXPECT_EQ(client.lastRumbleCtrlIdx, (uint8_t)3);
    EXPECT_EQ(client.lastRumble.strongMagnitude, (uint16_t)32768);
    EXPECT_EQ(client.lastRumble.weakMagnitude, (uint16_t)16384);
    EXPECT_EQ(client.lastRumble.durationMs, (uint16_t)500); // default wire duration
}

static void test_handleRumbleFromBackend_unknownSerialDropped() {
    TEST("handleRumbleFromBackend — unknown serial drops silently");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);

    RumbleReport rr{};
    rr.strongMagnitude = 1234;
    // Use a serial that doesn't match any active controller.
    vigem.fireRumble(999, rr);

    EXPECT_EQ(client.rumbleCalls, 0);
}

static void test_handleRumbleFromBackend_inactiveControllerDropped() {
    TEST("handleRumbleFromBackend — inactive controller drops silently");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    auto snap = svc.getConnectionsSnapshot();
    uint32_t serial = snap.connections[0].controllers[0].serial;
    svc.handleControllerRemove(r.token, 0); // controller no longer owns the serial

    RumbleReport rr{};
    rr.strongMagnitude = 1234;
    vigem.fireRumble(serial, rr);
    EXPECT_EQ(client.rumbleCalls, 0);
}

static void test_handleRumbleFromBackend_coalescesIdenticalReports() {
    TEST("handleRumbleFromBackend — coalesces back-to-back identical magnitudes");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    auto snap = svc.getConnectionsSnapshot();
    uint32_t serial = snap.connections[0].controllers[0].serial;

    RumbleReport rr{};
    rr.strongMagnitude = 1000;
    rr.weakMagnitude = 500;

    vigem.fireRumble(serial, rr); // 1st: emits
    vigem.fireRumble(serial, rr); // 2nd: same magnitudes, suppress
    vigem.fireRumble(serial, rr); // 3rd: still same, suppress
    EXPECT_EQ(client.rumbleCalls, 1);

    rr.strongMagnitude = 1001; // change → emit
    vigem.fireRumble(serial, rr);
    EXPECT_EQ(client.rumbleCalls, 2);
}

static void test_handleRumbleFromBackend_stopReportEmitted() {
    TEST("handleRumbleFromBackend — non-zero followed by all-zero emits both");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    auto snap = svc.getConnectionsSnapshot();
    uint32_t serial = snap.connections[0].controllers[0].serial;

    RumbleReport on{};
    on.strongMagnitude = 50000;
    vigem.fireRumble(serial, on);
    EXPECT_EQ(client.rumbleCalls, 1);

    RumbleReport off{}; // zero magnitudes — game asked for stop
    vigem.fireRumble(serial, off);
    EXPECT_EQ(client.rumbleCalls, 2);
    EXPECT_EQ(client.lastRumble.strongMagnitude, (uint16_t)0);
    EXPECT_EQ(client.lastRumble.weakMagnitude, (uint16_t)0);
}

static void test_handleRumbleFromBackend_lightbarPreserved() {
    TEST("handleRumbleFromBackend — DS4 lightbar bytes pass through to the client");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    svc.handleControllerType(r.token, 0, CONTROLLER_TYPE_PLAYSTATION);
    auto snap = svc.getConnectionsSnapshot();
    uint32_t serial = snap.connections[0].controllers[0].serial;

    RumbleReport rr{};
    rr.strongMagnitude = 200;
    rr.weakMagnitude = 100;
    rr.hasLightbar = true;
    rr.lightbarR = 0x10;
    rr.lightbarG = 0x80;
    rr.lightbarB = 0xFF;
    vigem.fireRumble(serial, rr);
    EXPECT_EQ(client.rumbleCalls, 1);
    EXPECT(client.lastRumble.hasLightbar);
    EXPECT_EQ((int)client.lastRumble.lightbarR, 0x10);
    EXPECT_EQ((int)client.lastRumble.lightbarG, 0x80);
    EXPECT_EQ((int)client.lastRumble.lightbarB, 0xFF);
}

static void test_handleRumbleFromBackend_lightbarChangeDefeatsCoalesce() {
    TEST("handleRumbleFromBackend — lightbar colour change re-emits even at same magnitude");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    auto snap = svc.getConnectionsSnapshot();
    uint32_t serial = snap.connections[0].controllers[0].serial;

    RumbleReport rr{};
    rr.strongMagnitude = 100;
    rr.weakMagnitude = 50;
    rr.hasLightbar = true;
    rr.lightbarR = 0x10;
    vigem.fireRumble(serial, rr);
    EXPECT_EQ(client.rumbleCalls, 1);

    rr.lightbarR = 0x20; // colour change, magnitudes identical
    vigem.fireRumble(serial, rr);
    EXPECT_EQ(client.rumbleCalls, 2);
}

static void test_handleRumbleFromBackend_routesAcrossMultipleConnections() {
    TEST("handleRumbleFromBackend — correctly routes when multiple connections own different "
         "serials");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r1 = svc.openSession("d1", "D1", "1.1.1.1", TEST_KEY);
    svc.handleControllerAdd(r1.token, 0);
    auto r2 = svc.openSession("d2", "D2", "1.1.1.2", TEST_KEY);
    svc.handleControllerAdd(r2.token, 0);

    auto snap = svc.getConnectionsSnapshot();
    // Snapshot ordering across an unordered_map is unspecified, so build a
    // {token → serial} map from the snapshot rather than indexing positionally.
    uint32_t serialFor1 = 0;
    uint32_t serialFor2 = 0;
    for (auto& c : snap.connections) {
        if (c.token == r1.token) serialFor1 = c.controllers[0].serial;
        if (c.token == r2.token) serialFor2 = c.controllers[0].serial;
    }
    EXPECT(serialFor1 != 0);
    EXPECT(serialFor2 != 0);
    EXPECT(serialFor1 != serialFor2);

    RumbleReport rr{};
    rr.strongMagnitude = 0xAAAA;
    vigem.fireRumble(serialFor2, rr);
    EXPECT_EQ(client.rumbleCalls, 1);
    EXPECT_EQ(client.lastRumbleConnToken, r2.token);

    vigem.fireRumble(serialFor1, rr);
    EXPECT_EQ(client.rumbleCalls, 2);
    EXPECT_EQ(client.lastRumbleConnToken, r1.token);
}

static void test_handleRumbleFromBackend_serialReuseClearsState() {
    TEST("handleRumbleFromBackend — remove + re-add resets coalesce state for the slot");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    auto snap = svc.getConnectionsSnapshot();
    uint32_t serial1 = snap.connections[0].controllers[0].serial;

    RumbleReport rr{};
    rr.strongMagnitude = 1000;
    vigem.fireRumble(serial1, rr);
    EXPECT_EQ(client.rumbleCalls, 1);

    // Remove + re-add: the same serial gets recycled (allocateSerial picks
    // the first free slot, which is the one we just released). The coalesce
    // cache lives on the Controller struct, which is reset by handleControllerAdd.
    svc.handleControllerRemove(r.token, 0);
    svc.handleControllerAdd(r.token, 0);
    snap = svc.getConnectionsSnapshot();
    uint32_t serial2 = snap.connections[0].controllers[0].serial;
    EXPECT_EQ(serial1, serial2); // serial recycled

    // Same magnitudes as before — but the cache should have been cleared by
    // re-add, so the report should re-emit instead of being coalesced.
    vigem.fireRumble(serial2, rr);
    EXPECT_EQ(client.rumbleCalls, 2);
}

static void test_handleRumbleFromBackend_customDuration() {
    TEST("handleRumbleFromBackend — explicit wireDurationMs is stamped on the outgoing report");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    auto snap = svc.getConnectionsSnapshot();
    uint32_t serial = snap.connections[0].controllers[0].serial;

    RumbleReport rr{};
    rr.strongMagnitude = 100;
    svc.handleRumbleFromBackend(serial, rr, /*wireDurationMs=*/1234);
    EXPECT_EQ(client.rumbleCalls, 1);
    EXPECT_EQ(client.lastRumble.durationMs, (uint16_t)1234);
}

static void test_handleRumbleFromBackend_durationChangeAloneDoesNotEmit() {
    TEST("handleRumbleFromBackend — duration-only changes are NOT a re-emit trigger");
    // Rationale: durationMs is a wire-side refresh deadline knob, not part of
    // the actuator-state comparison. Bumping it without changing magnitudes
    // should not flood the wire.
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    auto snap = svc.getConnectionsSnapshot();
    uint32_t serial = snap.connections[0].controllers[0].serial;

    RumbleReport rr{};
    rr.strongMagnitude = 100;
    svc.handleRumbleFromBackend(serial, rr, 500);
    EXPECT_EQ(client.rumbleCalls, 1);
    svc.handleRumbleFromBackend(serial, rr, 700);
    EXPECT_EQ(client.rumbleCalls, 1); // suppressed
}

static void test_handleRumbleFromBackend_separateControllersIndependentlyCoalesced() {
    TEST("handleRumbleFromBackend — coalesce state is per-controller, not global");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    svc.handleControllerAdd(r.token, 1);
    auto snap = svc.getConnectionsSnapshot();
    uint32_t s0 = 0;
    uint32_t s1 = 0;
    for (auto& c : snap.connections[0].controllers) {
        if (c.index == 0) s0 = c.serial;
        if (c.index == 1) s1 = c.serial;
    }
    EXPECT(s0 != 0);
    EXPECT(s1 != 0);

    RumbleReport rr{};
    rr.strongMagnitude = 5000;

    vigem.fireRumble(s0, rr);
    vigem.fireRumble(s0, rr); // coalesced
    vigem.fireRumble(s1, rr); // distinct controller — emits
    vigem.fireRumble(s1, rr); // coalesced
    EXPECT_EQ(client.rumbleCalls, 2);
}

static void test_handleRumbleFromBackend_zeroIsCoalescedWhenInitial() {
    TEST("handleRumbleFromBackend — initial all-zero report still emits the first time");
    // Rationale: the cache marker `lastRumbleValid=false` means we always
    // emit the first packet, even if it's all zeros, so a stop request that
    // arrives before any non-zero update is still forwarded to the dish.
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    auto snap = svc.getConnectionsSnapshot();
    uint32_t serial = snap.connections[0].controllers[0].serial;

    RumbleReport rr{}; // all zeros
    vigem.fireRumble(serial, rr);
    EXPECT_EQ(client.rumbleCalls, 1);
    // Same all-zero again is now coalesced.
    vigem.fireRumble(serial, rr);
    EXPECT_EQ(client.rumbleCalls, 1);
}

int main() {
    std::cout << "Running SessionService tests...\n\n";

    test_openSession_basic();
    test_openSession_replacesStale();
    test_openSession_multipleDevices();
    test_closeSession_basic();
    test_closeSession_withControllers();
    test_closeSession_invalidToken();
    test_closeAllSessions();
    test_handleControllerAdd_success();
    test_handleControllerAdd_alreadyExists();
    test_handleControllerAdd_backendUnavailable();
    test_handleControllerAdd_noSlots();
    test_handleControllerAdd_pluginFail();
    test_handleControllerAdd_invalidToken();
    test_handleControllerAdd_outOfBounds();
    test_handleControllerRemove_success();
    test_handleControllerRemove_notActive();
    test_handleControllerRemove_closesBusWhenIdle();
    test_handleGamepadData_success();
    test_handleGamepadData_invalidToken();
    test_handleGamepadData_inactiveController();
    test_handleGamepadData_outOfBounds();
    test_handleHeartbeat();
    test_handleHeartbeat_invalidToken();
    test_getDecryptInfo();
    test_getDecryptInfo_invalidToken();
    test_updatePostDecrypt();
    test_isDeviceConnected();
    test_getConnectionsSnapshot();
    test_stats();
    test_serialRecycling();
    test_broadcastOnControllerChange();
    test_staleReplacementCleansUpControllers();
    // ── Controller type helper functions (types.h) ──
    test_controllerTypeName_xbox();
    test_controllerTypeName_playstation();
    test_controllerTypeName_outOfRange();
    test_controllerTypeLabel_xbox();
    test_controllerTypeLabel_playstation();
    test_controllerTypeLabel_outOfRange();
    test_controllerTypeUsesDS4_xbox();
    test_controllerTypeUsesDS4_playstation();
    test_controllerTypeUsesDS4_outOfRange();
    test_controllerType_constants();

    // ── handleControllerType — basic behavior ──
    test_handleControllerType_setsTypeAndBroadcasts();
    test_handleControllerType_invalidToken();
    test_handleControllerType_outOfBoundsCtrlIdx();
    test_handleControllerType_inactiveControllerIgnored();
    test_handleControllerType_invalidValueClampsToXbox();
    test_handleControllerType_boundaryValueClampsToXbox();

    // ── handleControllerType — replug behavior ──
    test_handleControllerType_xboxToPlaystationReplugsAsDS4();
    test_handleControllerType_playstationToXboxReplugsAsXbox();
    test_handleControllerType_xboxToXboxNoReplug();
    test_handleControllerType_playstationToPlaystationNoReplug();
    test_handleControllerType_replugPreservesSerial();
    test_handleControllerType_replugFailureLogsError();
    test_handleControllerType_multipleRapidSwitches();

    // ── handleControllerAdd — DS4-aware ──
    test_handleControllerAdd_defaultTypeIsXbox();
    test_handleControllerAdd_presetPlaystationType();
    test_handleControllerAdd_thenSetPlaystation_fullFlow();

    // ── Gamepad data routing ──
    test_gamepadData_xboxTypeUsesSubmitReport();
    test_gamepadData_playstationTypeUsesSubmitDS4Report();
    test_gamepadData_routingSwitchesWithType();

    // ── Snapshot ──
    test_snapshot_defaultControllerType();
    test_snapshot_reflectsTypeChange();
    test_snapshot_multipleControllersWithDifferentTypes();

    // ── Session lifecycle with DS4 ──
    test_closeSession_unplugsDS4Controllers();
    test_closeAllSessions_withMixedTypes();
    test_staleReplacement_unplugsDS4Controller();
    test_controllerRemove_thenReaddRetainsTypeFromSameSlot();

    // ── Protocol constants ──
    test_msgControllerType_constant();

    // ── Rumble (return-path) ──
    test_msgRumble_constant();
    test_constructor_installsRumbleCallback();
    test_handleRumbleFromBackend_routesToOwningConnection();
    test_handleRumbleFromBackend_unknownSerialDropped();
    test_handleRumbleFromBackend_inactiveControllerDropped();
    test_handleRumbleFromBackend_coalescesIdenticalReports();
    test_handleRumbleFromBackend_stopReportEmitted();
    test_handleRumbleFromBackend_lightbarPreserved();
    test_handleRumbleFromBackend_lightbarChangeDefeatsCoalesce();
    test_handleRumbleFromBackend_routesAcrossMultipleConnections();
    test_handleRumbleFromBackend_serialReuseClearsState();
    test_handleRumbleFromBackend_customDuration();
    test_handleRumbleFromBackend_durationChangeAloneDoesNotEmit();
    test_handleRumbleFromBackend_separateControllersIndependentlyCoalesced();
    test_handleRumbleFromBackend_zeroIsCoalescedWhenInitial();

    std::cout << "\n=== Test Results ===\n";
    std::cout << "  Passed: " << g_pass << "\n";
    std::cout << "  Failed: " << g_fail << "\n";
    if (g_fail > 0) {
        std::cout << "  STATUS: FAIL\n";
        return 1;
    }
    std::cout << "  STATUS: ALL PASSED\n";
    return 0;
}
