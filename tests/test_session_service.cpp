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

#define TEST(name) \
    do { g_currentTest = (name); } while(0)

#define EXPECT(cond) \
    do { \
        if (cond) { g_pass++; } \
        else { g_fail++; std::cerr << "  FAIL [" << g_currentTest << "] " \
            << __FILE__ << ":" << __LINE__ << "  " << #cond << "\n"; } \
    } while(0)

#define EXPECT_EQ(a, b) \
    do { \
        auto _a = (a); auto _b = (b); \
        if (_a == _b) { g_pass++; } \
        else { g_fail++; std::cerr << "  FAIL [" << g_currentTest << "] " \
            << __FILE__ << ":" << __LINE__ << "  " << #a << " == " << #b \
            << "  (got " << _a << " vs " << _b << ")\n"; } \
    } while(0)

// ── Mock IViGemPort ─────────────────────────────────────────────────────────
struct MockViGem : IViGemPort {
    bool busOpen = false;
    bool ensureBusReturnVal = true;
    bool pluginReturnVal = true;
    bool submitReturnVal = true;
    bool driverInstalled = true;

    int ensureBusCalls = 0;
    int closeBusCalls = 0;
    int pluginCalls = 0;
    int unplugCalls = 0;
    int submitCalls = 0;

    std::vector<uint32_t> pluggedSerials;
    std::vector<uint32_t> unpluggedSerials;
    GamepadReport lastSubmittedReport{};

    bool ensureBusOpen() override { ensureBusCalls++; if (ensureBusReturnVal) busOpen = true; return ensureBusReturnVal; }
    void closeBus() override { closeBusCalls++; busOpen = false; }
    bool isBusOpen() const override { return busOpen; }
    bool pluginDevice(uint32_t serial) override { pluginCalls++; pluggedSerials.push_back(serial); return pluginReturnVal; }
    void unplugDevice(uint32_t serial) override { unplugCalls++; unpluggedSerials.push_back(serial); }
    bool submitReport(uint32_t, const GamepadReport& r) override { submitCalls++; lastSubmittedReport = r; return submitReturnVal; }
    bool isDriverInstalled() override { return driverInstalled; }

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

    // Last controller ACK params
    uint16_t lastAckType = 0;
    uint8_t lastAckCtrl = 0;
    uint8_t lastAckResult = 0;

    void updateClientAddr(uint32_t, const std::string&, uint16_t) override { updateAddrCalls++; }
    void removeClientAddr(uint32_t) override { removeAddrCalls++; }
    void sendHeartbeatAck(const Connection&) override { heartbeatAckCalls++; }
    void sendControllerAck(const Connection&, uint16_t t, uint8_t c, uint8_t r) override {
        controllerAckCalls++;
        lastAckType = t; lastAckCtrl = c; lastAckResult = r;
    }
    void sendServerStatus(const Connection&, bool, uint8_t) override { serverStatusCalls++; }
    void broadcastServerStatus(const std::vector<std::pair<uint32_t, const Connection*>>&,
                               bool, uint8_t) override { broadcastCalls++; }

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
static const uint8_t TEST_KEY[CRYPTO_KEY_SIZE] = {
    1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,
    17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32
};

static OpenSessionResult openTestSession(SessionService& svc,
    const std::string& devId = "dev1",
    const std::string& devName = "TestDevice") {
    return svc.openSession(devId, devName, "192.168.1.100", TEST_KEY);
}

// ── Tests ───────────────────────────────────────────────────────────────────

static void test_openSession_basic() {
    TEST("openSession — basic");
    MockViGem vigem; MockClient client; MockLog log;
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
    MockViGem vigem; MockClient client; MockLog log;
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
    MockViGem vigem; MockClient client; MockLog log;
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
    MockViGem vigem; MockClient client; MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    int removed = svc.closeSession(r.token);
    EXPECT_EQ(removed, 0); // no controllers were active
    EXPECT(!svc.isDeviceConnected("dev1"));
}

static void test_closeSession_withControllers() {
    TEST("closeSession — unplugs active controllers");
    MockViGem vigem; MockClient client; MockLog log;
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
    MockViGem vigem; MockClient client; MockLog log;
    SessionService svc(vigem, client, log);

    EXPECT_EQ(svc.closeSession(99999), -1);
}

static void test_closeAllSessions() {
    TEST("closeAllSessions — clears everything");
    MockViGem vigem; MockClient client; MockLog log;
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
    MockViGem vigem; MockClient client; MockLog log;
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
    MockViGem vigem; MockClient client; MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    client.reset();
    svc.handleControllerAdd(r.token, 0); // duplicate
    EXPECT_EQ(client.lastAckResult, ACK_ERR_ALREADY_EXISTS);
    EXPECT_EQ(svc.totalActiveControllers(), 1); // still 1
}

static void test_handleControllerAdd_vigemUnavailable() {
    TEST("handleControllerAdd — ViGEm bus unavailable");
    MockViGem vigem; MockClient client; MockLog log;
    vigem.ensureBusReturnVal = false;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    EXPECT_EQ(client.lastAckResult, ACK_ERR_VIGEM_UNAVAIL);
    EXPECT_EQ(svc.totalActiveControllers(), 0);
}

static void test_handleControllerAdd_noSlots() {
    TEST("handleControllerAdd — no serial slots left");
    MockViGem vigem; MockClient client; MockLog log;
    SessionService svc(vigem, client, log);

    // Fill all 16 slots across multiple sessions
    for (int i = 0; i < 16; i++) {
        auto r = svc.openSession("dev" + std::to_string(i), "D" + std::to_string(i), "1.1.1.1", TEST_KEY);
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
    MockViGem vigem; MockClient client; MockLog log;
    vigem.pluginReturnVal = false;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    EXPECT_EQ(client.lastAckResult, ACK_ERR_PLUGIN_FAIL);
    EXPECT_EQ(svc.availableSlots(), 16); // serial released
}

static void test_handleControllerAdd_invalidToken() {
    TEST("handleControllerAdd — invalid token");
    MockViGem vigem; MockClient client; MockLog log;
    SessionService svc(vigem, client, log);

    svc.handleControllerAdd(99999, 0);
    EXPECT_EQ(client.controllerAckCalls, 0); // nothing sent
}

static void test_handleControllerAdd_outOfBounds() {
    TEST("handleControllerAdd — ctrlIdx out of bounds");
    MockViGem vigem; MockClient client; MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 20); // out of bounds
    EXPECT_EQ(client.controllerAckCalls, 0);
    EXPECT_EQ(vigem.pluginCalls, 0);
}

static void test_handleControllerRemove_success() {
    TEST("handleControllerRemove — success");
    MockViGem vigem; MockClient client; MockLog log;
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
    MockViGem vigem; MockClient client; MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerRemove(r.token, 0);
    EXPECT_EQ(client.lastAckResult, ACK_ERR_NOT_FOUND);
}

static void test_handleControllerRemove_closesBusWhenIdle() {
    TEST("handleControllerRemove — closes bus when no controllers left");
    MockViGem vigem; MockClient client; MockLog log;
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
    MockViGem vigem; MockClient client; MockLog log;
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
    MockViGem vigem; MockClient client; MockLog log;
    SessionService svc(vigem, client, log);

    GamepadReport rpt{};
    EXPECT(!svc.handleGamepadData(99999, 0, rpt));
}

static void test_handleGamepadData_inactiveController() {
    TEST("handleGamepadData — inactive controller");
    MockViGem vigem; MockClient client; MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    // No controller added
    GamepadReport rpt{};
    EXPECT(!svc.handleGamepadData(r.token, 0, rpt));
}

static void test_handleGamepadData_outOfBounds() {
    TEST("handleGamepadData — ctrlIdx out of bounds");
    MockViGem vigem; MockClient client; MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    GamepadReport rpt{};
    EXPECT(!svc.handleGamepadData(r.token, 20, rpt));
}

static void test_handleHeartbeat() {
    TEST("handleHeartbeat — sends ACK and status");
    MockViGem vigem; MockClient client; MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleHeartbeat(r.token);
    EXPECT_EQ(client.heartbeatAckCalls, 1);
    EXPECT_EQ(client.serverStatusCalls, 1);
}

static void test_handleHeartbeat_invalidToken() {
    TEST("handleHeartbeat — invalid token is no-op");
    MockViGem vigem; MockClient client; MockLog log;
    SessionService svc(vigem, client, log);

    svc.handleHeartbeat(99999);
    EXPECT_EQ(client.heartbeatAckCalls, 0);
}

static void test_getDecryptInfo() {
    TEST("getDecryptInfo — returns key and counter");
    MockViGem vigem; MockClient client; MockLog log;
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
    MockViGem vigem; MockClient client; MockLog log;
    SessionService svc(vigem, client, log);

    uint8_t outKey[CRYPTO_KEY_SIZE] = {};
    uint32_t outCounter = 0;
    EXPECT(!svc.getDecryptInfo(99999, outKey, outCounter));
}

static void test_updatePostDecrypt() {
    TEST("updatePostDecrypt — updates counter and address");
    MockViGem vigem; MockClient client; MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.updatePostDecrypt(r.token, 42, "10.0.0.1", 5555);
    EXPECT_EQ(client.updateAddrCalls, 1);

    uint8_t k[CRYPTO_KEY_SIZE]; uint32_t c = 0;
    svc.getDecryptInfo(r.token, k, c);
    EXPECT_EQ(c, (uint32_t)42);
}

static void test_isDeviceConnected() {
    TEST("isDeviceConnected");
    MockViGem vigem; MockClient client; MockLog log;
    SessionService svc(vigem, client, log);

    EXPECT(!svc.isDeviceConnected("dev1"));
    auto r = openTestSession(svc);
    EXPECT(svc.isDeviceConnected("dev1"));
    svc.closeSession(r.token);
    EXPECT(!svc.isDeviceConnected("dev1"));
}

static void test_getConnectionsSnapshot() {
    TEST("getConnectionsSnapshot");
    MockViGem vigem; MockClient client; MockLog log;
    SessionService svc(vigem, client, log);

    auto snap0 = svc.getConnectionsSnapshot();
    EXPECT_EQ((int)snap0.connections.size(), 0);
    EXPECT_EQ(snap0.maxControllers, MAX_VIGEM_CONTROLLERS);

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
    TEST("isViGEmAvailable / totalActiveControllers / availableSlots");
    MockViGem vigem; MockClient client; MockLog log;
    SessionService svc(vigem, client, log);

    EXPECT(!svc.isViGEmAvailable());
    EXPECT_EQ(svc.totalActiveControllers(), 0);
    EXPECT_EQ(svc.availableSlots(), 16);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    EXPECT(svc.isViGEmAvailable());
    EXPECT_EQ(svc.totalActiveControllers(), 1);
    EXPECT_EQ(svc.availableSlots(), 15);
}

static void test_serialRecycling() {
    TEST("serial recycling after controller remove");
    MockViGem vigem; MockClient client; MockLog log;
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
    MockViGem vigem; MockClient client; MockLog log;
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
    MockViGem vigem; MockClient client; MockLog log;
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
    test_handleControllerAdd_vigemUnavailable();
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

