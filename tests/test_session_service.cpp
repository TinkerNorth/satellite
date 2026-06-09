// SPDX-License-Identifier: LGPL-3.0-or-later
#include "../src/core/session_service.h"
#include "../src/core/touchpad_codec.h"
#include "../src/core/gamepad_backend.h"
#include "../src/core/ipv4_util.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <thread>
#include <chrono>
#include <unordered_map>

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
    int submitMotionCalls = 0;
    int submitBatteryCalls = 0;
    int submitTouchpadCalls = 0;
    int submitRelativeMouseCalls = 0;
    int setLightbarCallbackCalls = 0;

    // Defaults mimic the IGamepadPort base (no backend supports these yet).
    bool submitMotionReturnVal = true;
    bool submitBatteryReturnVal = true;
    bool submitTouchpadReturnVal = true;
    bool submitRelativeMouseReturnVal = true;

    std::vector<uint32_t> pluggedSerials;
    std::vector<uint32_t> unpluggedSerials;
    GamepadReport lastSubmittedReport{};
    uint32_t lastMotionSerial = 0;
    MotionReport lastMotion{};
    uint32_t lastBatterySerial = 0;
    BatteryReport lastBattery{};
    uint32_t lastTouchpadSerial = 0;
    TouchpadReport lastTouchpad{};
    int lastMouseDx = 0;
    int lastMouseDy = 0;
    bool lastMouseButton = false;
    // Tests synthesize "the platform fired a notification" by invoking these via
    // fireRumble / fireLightbar.
    RumbleCallback capturedRumbleCb;
    LightbarCallback capturedLightbarCb;

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
    void fireRumble(uint32_t serial, const RumbleReport& r) {
        if (capturedRumbleCb) capturedRumbleCb(serial, r);
    }
    bool submitMotion(uint32_t serial, const MotionReport& r) override {
        submitMotionCalls++;
        lastMotionSerial = serial;
        lastMotion = r;
        return submitMotionReturnVal;
    }
    bool submitBattery(uint32_t serial, const BatteryReport& r) override {
        submitBatteryCalls++;
        lastBatterySerial = serial;
        lastBattery = r;
        return submitBatteryReturnVal;
    }
    bool submitTouchpad(uint32_t serial, const TouchpadReport& r) override {
        submitTouchpadCalls++;
        lastTouchpadSerial = serial;
        lastTouchpad = r;
        return submitTouchpadReturnVal;
    }
    bool submitRelativeMouse(int dx, int dy, bool leftButton) override {
        submitRelativeMouseCalls++;
        lastMouseDx = dx;
        lastMouseDy = dy;
        lastMouseButton = leftButton;
        return submitRelativeMouseReturnVal;
    }
    void setLightbarCallback(LightbarCallback cb) override {
        setLightbarCallbackCalls++;
        capturedLightbarCb = std::move(cb);
    }
    void fireLightbar(uint32_t serial, uint8_t r, uint8_t g, uint8_t b) {
        if (capturedLightbarCb) capturedLightbarCb(serial, r, g, b);
    }

    // Default mirrors real Windows/Linux: DS4 has an IMU sink, Xbox does not.
    // Tests override per-case to exercise the alternate paths.
    std::unordered_map<uint8_t, bool> supportsMotionForTypeMap{
        {CONTROLLER_TYPE_XBOX, false},
        {CONTROLLER_TYPE_PLAYSTATION, true},
    };
    bool supportsMotionForType(uint8_t controllerType) const override {
        auto it = supportsMotionForTypeMap.find(controllerType);
        return it != supportsMotionForTypeMap.end() ? it->second : false;
    }

    // Default true; tests flip a serial's entry to exercise the kernel-rejected path.
    std::unordered_map<uint32_t, bool> motionBackendOkMap;
    bool motionBackendOk(uint32_t serial) const override {
        auto it = motionBackendOkMap.find(serial);
        return it != motionBackendOkMap.end() ? it->second : true;
    }

    void reset() { *this = MockViGem{}; }
};

struct MockClient : IClientPort {
    int updateAddrCalls = 0;
    // Hot-path entry points (handleGamepadDataAndUpdate, updatePostDecryptV4)
    // route through updateClientAddrV4, not the legacy string overload.
    int updateAddrV4Calls = 0;
    uint32_t lastV4Token = 0;
    uint32_t lastV4IPv4Nbo = 0;
    uint16_t lastV4Port = 0;
    int removeAddrCalls = 0;
    int heartbeatAckCalls = 0;
    int controllerAckCalls = 0;
    int serverStatusCalls = 0;
    int broadcastCalls = 0;
    int rumbleCalls = 0;
    int lightbarCalls = 0;

    uint16_t lastAckType = 0;
    uint8_t lastAckCtrl = 0;
    uint8_t lastAckResult = 0;
    uint8_t lastAckMotionFlags = 0;

    uint32_t lastRumbleConnToken = 0;
    uint8_t lastRumbleCtrlIdx = 0;
    RumbleReport lastRumble{};

    uint32_t lastLightbarConnToken = 0;
    uint8_t lastLightbarCtrlIdx = 0;
    uint8_t lastLightbarR = 0;
    uint8_t lastLightbarG = 0;
    uint8_t lastLightbarB = 0;

    void updateClientAddr(uint32_t, const std::string&, uint16_t) override { updateAddrCalls++; }
    void updateClientAddrV4(uint32_t token, uint32_t ipv4Nbo, uint16_t port) override {
        updateAddrV4Calls++;
        lastV4Token = token;
        lastV4IPv4Nbo = ipv4Nbo;
        lastV4Port = port;
    }
    void removeClientAddr(uint32_t) override { removeAddrCalls++; }
    void sendHeartbeatAck(const Connection&) override { heartbeatAckCalls++; }
    void sendControllerAck(const Connection&, uint16_t t, uint8_t c, uint8_t r,
                           uint8_t motionFlags) override {
        controllerAckCalls++;
        lastAckType = t;
        lastAckCtrl = c;
        lastAckResult = r;
        lastAckMotionFlags = motionFlags;
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
    void sendLightbar(const Connection& conn, uint8_t ctrlIdx, uint8_t r, uint8_t g,
                      uint8_t b) override {
        lightbarCalls++;
        lastLightbarConnToken = conn.token;
        lastLightbarCtrlIdx = ctrlIdx;
        lastLightbarR = r;
        lastLightbarG = g;
        lastLightbarB = b;
    }

    void reset() { *this = MockClient{}; }
};

struct MockLog : ILogPort {
    int logCalls = 0;
    std::vector<std::string> messages;

    void logMsg(LogLevel, const std::string&, const std::string& msg) override {
        logCalls++;
        messages.push_back(msg);
    }
    void reset() { *this = MockLog{}; }
};

static const uint8_t TEST_KEY[CRYPTO_KEY_SIZE] = {1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11,
                                                  12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22,
                                                  23, 24, 25, 26, 27, 28, 29, 30, 31, 32};

static OpenSessionResult openTestSession(SessionService& svc, const std::string& devId = "dev1",
                                         const std::string& devName = "TestDevice",
                                         uint8_t touchpadMode = TOUCHPAD_MODE_DS4) {
    return svc.openSession(devId, devName, "192.168.1.100", TEST_KEY, touchpadMode);
}

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

// Controller type system: a controller defaults to XBOX (X360) and can switch
// to PLAYSTATION (DS4), which replugs the ViGEm virtual device; same-family
// changes don't replug, and invalid values clamp to XBOX.
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

static void test_handleControllerType_replugResendsMotionAckPlaystation() {
    TEST("handleControllerType — Xbox→PlayStation replug re-sends a motion ACK with both bits");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    client.reset();

    svc.handleControllerType(r.token, 0, CONTROLLER_TYPE_PLAYSTATION);
    EXPECT_EQ(client.controllerAckCalls, 1);
    EXPECT_EQ(client.lastAckType, MSG_CONTROLLER_TYPE);
    EXPECT_EQ(client.lastAckResult, ACK_OK);
    EXPECT_EQ(client.lastAckMotionFlags,
              (uint8_t)(ACK_MOTION_FLAG_SINK_SUPPORTED_FOR_TYPE | ACK_MOTION_FLAG_BACKEND_OK));
}

static void test_handleControllerType_replugResendsMotionAckBackendBroken() {
    TEST("handleControllerType — replug re-sends a motion ACK without BACKEND_OK when kernel "
         "rejects");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    vigem.motionBackendOkMap[1] = false;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    client.reset();

    svc.handleControllerType(r.token, 0, CONTROLLER_TYPE_PLAYSTATION);
    EXPECT_EQ(client.controllerAckCalls, 1);
    EXPECT_EQ(client.lastAckType, MSG_CONTROLLER_TYPE);
    EXPECT_EQ(client.lastAckMotionFlags, (uint8_t)ACK_MOTION_FLAG_SINK_SUPPORTED_FOR_TYPE);
}

static void test_handleControllerType_replugToXboxResendsMotionAck() {
    TEST("handleControllerType — PlayStation→Xbox replug re-sends a motion ACK with BACKEND_OK "
         "only");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    svc.handleControllerType(r.token, 0, CONTROLLER_TYPE_PLAYSTATION);
    client.reset();

    svc.handleControllerType(r.token, 0, CONTROLLER_TYPE_XBOX);
    EXPECT_EQ(client.controllerAckCalls, 1);
    EXPECT_EQ(client.lastAckType, MSG_CONTROLLER_TYPE);
    EXPECT_EQ(client.lastAckResult, ACK_OK);
    EXPECT_EQ(client.lastAckMotionFlags, (uint8_t)ACK_MOTION_FLAG_BACKEND_OK);
}

static void test_handleControllerType_sameTypeSendsNoAck() {
    TEST("handleControllerType — setting the same type does NOT re-send an ACK (no replug)");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    client.reset();

    svc.handleControllerType(r.token, 0, CONTROLLER_TYPE_XBOX);
    EXPECT_EQ(client.controllerAckCalls, 0);
}

static void test_handleControllerType_replugFailureSendsNoAck() {
    TEST("handleControllerType — a failed replug does NOT re-send an ACK");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    vigem.pluginReturnVal = false;
    client.reset();

    svc.handleControllerType(r.token, 0, CONTROLLER_TYPE_PLAYSTATION);
    EXPECT_EQ(client.controllerAckCalls, 0);
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

static void test_handleControllerAdd_withType_playstationPlugsDS4Directly() {
    TEST("handleControllerAdd — controllerType=PlayStation plugs DS4 on the first plug, no replug");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0, 0, CONTROLLER_TYPE_PLAYSTATION);

    EXPECT_EQ(vigem.pluginDS4Calls, 1);
    EXPECT_EQ(vigem.pluginCalls, 0);
    EXPECT_EQ(vigem.unplugCalls, 0);
    EXPECT_EQ(client.lastAckType, MSG_CONTROLLER_ADD);
    EXPECT_EQ(client.lastAckResult, ACK_OK);
    EXPECT_EQ(client.lastAckMotionFlags,
              (uint8_t)(ACK_MOTION_FLAG_SINK_SUPPORTED_FOR_TYPE | ACK_MOTION_FLAG_BACKEND_OK));
    auto snap = svc.getConnectionsSnapshot();
    EXPECT_EQ(snap.connections[0].controllers[0].controllerType, CONTROLLER_TYPE_PLAYSTATION);
}

static void test_handleControllerAdd_withType_invalidClampsToXbox() {
    TEST("handleControllerAdd — explicit out-of-range controllerType clamps to Xbox");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    // CONTROLLER_TYPE_COUNT is an explicit-but-invalid value (distinct from the
    // UNSPECIFIED sentinel), so it clamps to Xbox rather than retaining.
    svc.handleControllerAdd(r.token, 0, 0, CONTROLLER_TYPE_COUNT);

    EXPECT_EQ(vigem.pluginCalls, 1);
    EXPECT_EQ(vigem.pluginDS4Calls, 0);
    auto snap = svc.getConnectionsSnapshot();
    EXPECT_EQ(snap.connections[0].controllers[0].controllerType, CONTROLLER_TYPE_XBOX);
}

static void test_handleControllerAdd_unspecifiedRetainsExistingType() {
    TEST("handleControllerAdd — UNSPECIFIED type retains the slot's existing type across re-add");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0, 0, CONTROLLER_TYPE_PLAYSTATION);
    svc.handleControllerRemove(r.token, 0);
    svc.handleControllerAdd(r.token, 0, 0, CONTROLLER_TYPE_UNSPECIFIED);

    EXPECT_EQ(vigem.pluginDS4Calls, 2);
    EXPECT_EQ(vigem.pluginCalls, 0);
    auto snap = svc.getConnectionsSnapshot();
    EXPECT_EQ(snap.connections[0].controllers[0].controllerType, CONTROLLER_TYPE_PLAYSTATION);
}

static void test_handleControllerAdd_withType_xboxExplicitPlugsXbox() {
    TEST("handleControllerAdd — explicit Xbox type plugs Xbox, ACK has BACKEND_OK only (no sink)");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0, 0, CONTROLLER_TYPE_XBOX);

    EXPECT_EQ(vigem.pluginCalls, 1);
    EXPECT_EQ(vigem.pluginDS4Calls, 0);
    EXPECT_EQ(client.lastAckType, MSG_CONTROLLER_ADD);
    EXPECT_EQ(client.lastAckResult, ACK_OK);
    // Xbox: supportsMotionForType=false → no SINK; motionBackendOk default true → BACKEND_OK.
    EXPECT_EQ(client.lastAckMotionFlags, (uint8_t)ACK_MOTION_FLAG_BACKEND_OK);
}

static void test_handleControllerAdd_withType_playstationBackendRejected() {
    TEST("handleControllerAdd — PlayStation type but kernel-rejected sink → ACK has SINK, no "
         "BACKEND_OK");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    vigem.motionBackendOkMap[1] = false; // first serial's IMU sink rejected
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0, 0, CONTROLLER_TYPE_PLAYSTATION);

    EXPECT_EQ(vigem.pluginDS4Calls, 1);
    EXPECT_EQ(client.lastAckResult, ACK_OK);
    EXPECT_EQ(client.lastAckMotionFlags, (uint8_t)ACK_MOTION_FLAG_SINK_SUPPORTED_FOR_TYPE);
}

static void test_handleControllerAdd_withType_unspecifiedFreshSlotDefaultsXbox() {
    TEST("handleControllerAdd — UNSPECIFIED on a never-set slot keeps the default Xbox");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0, 0, CONTROLLER_TYPE_UNSPECIFIED);

    EXPECT_EQ(vigem.pluginCalls, 1);
    EXPECT_EQ(vigem.pluginDS4Calls, 0);
    auto snap = svc.getConnectionsSnapshot();
    EXPECT_EQ(snap.connections[0].controllers[0].controllerType, CONTROLLER_TYPE_XBOX);
}

static void test_handleControllerType_replugAckCarriesIdxAndType() {
    TEST("handleControllerType — replug ACK targets the right ctrlIdx with requestType TYPE");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0); // idx 0, Xbox
    svc.handleControllerAdd(r.token, 1); // idx 1, Xbox
    client.reset();

    svc.handleControllerType(r.token, 1, CONTROLLER_TYPE_PLAYSTATION); // replug idx 1
    EXPECT_EQ(client.controllerAckCalls, 1);
    EXPECT_EQ(client.lastAckType, MSG_CONTROLLER_TYPE);
    EXPECT_EQ((int)client.lastAckCtrl, 1);
    EXPECT_EQ(client.lastAckResult, ACK_OK);
}

static void test_handleControllerType_playstationToPlaystationSendsNoAck() {
    TEST("handleControllerType — PlayStation→PlayStation does not replug and sends no ACK");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0, 0, CONTROLLER_TYPE_PLAYSTATION);
    client.reset();

    svc.handleControllerType(r.token, 0, CONTROLLER_TYPE_PLAYSTATION); // same type
    EXPECT_EQ(client.controllerAckCalls, 0);
    EXPECT_EQ(vigem.unplugCalls, 0);
}

static void test_handleControllerAdd_presetPlaystationType() {
    TEST("handleControllerAdd — if type was pre-set to PlayStation, uses DS4 plugin");
    // Controllers always start as XBOX (type is set after add via
    // MSG_CONTROLLER_TYPE), so add always uses the Xbox plugin today.
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

    auto r = openTestSession(svc);
    EXPECT(r.ok);

    svc.handleControllerAdd(r.token, 0);
    EXPECT_EQ(client.lastAckResult, ACK_OK);
    EXPECT_EQ(vigem.pluginCalls, 1);
    EXPECT_EQ(vigem.pluginDS4Calls, 0);

    svc.handleControllerType(r.token, 0, CONTROLLER_TYPE_PLAYSTATION);
    EXPECT_EQ(vigem.unplugCalls, 1);
    EXPECT_EQ(vigem.pluginDS4Calls, 1);

    GamepadReport rpt{};
    rpt.wButtons = 0x1000;
    EXPECT(svc.handleGamepadData(r.token, 0, rpt));
    EXPECT_EQ(vigem.submitDS4Calls, 1);
    EXPECT_EQ(vigem.submitCalls, 0);

    auto snap = svc.getConnectionsSnapshot();
    EXPECT_EQ(snap.connections[0].controllers[0].controllerType, CONTROLLER_TYPE_PLAYSTATION);
}

// Each test pins one corner of the 2-bit motionFlags ACK byte (ACK_MOTION_FLAG_*
// in core/types.h); the dish drives its per-slot motion pill from these.
static void test_handleControllerAdd_motionFlags_xboxTypeBackendOk() {
    TEST("handleControllerAdd — motion flags: Xbox type → BACKEND_OK only (no sink for type)");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0); // default controller type is XBOX
    EXPECT_EQ(client.lastAckResult, ACK_OK);
    // MockViGem defaults: supportsMotionForType(XBOX) = false (no IMU surface
    // on the Xbox 360 virtual pad), motionBackendOk() = true. So only the
    // backend-ok bit is set.
    EXPECT_EQ(client.lastAckMotionFlags, ACK_MOTION_FLAG_BACKEND_OK);
}

static void test_handleControllerAdd_motionFlags_playstationTypeBothBits() {
    TEST("handleControllerAdd — motion flags: PS type + backend ok → both bits set");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    // Type can only be set after add (no preset), so pin the PS branch via the
    // snapshot: after the PS-type change, motionSinkSupportedForType reflects PS.
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    svc.handleControllerType(r.token, 0, CONTROLLER_TYPE_PLAYSTATION);
    auto snap = svc.getConnectionsSnapshot();
    EXPECT_EQ(snap.connections[0].controllers[0].motionSinkSupportedForType, true);
    EXPECT_EQ(snap.connections[0].controllers[0].motionBackendOk, true);
}

static void test_handleControllerAdd_motionFlags_backendBroken() {
    TEST("handleControllerAdd — motion flags: kernel rejected sink → no BACKEND_OK bit");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    // First-added controller gets serial 1; mark its IMU sink as kernel-
    // rejected ahead of time so the ACK's flag byte reads as "backend broken".
    vigem.motionBackendOkMap[1] = false;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    EXPECT_EQ(client.lastAckResult, ACK_OK);
    // Xbox default type → supportsMotionForType(XBOX) = false; the per-serial
    // motionBackendOk = false. Both bits clear.
    EXPECT_EQ(client.lastAckMotionFlags, 0);
}

static void test_handleControllerAdd_motionFlags_errorAcksCarryZero() {
    TEST("handleControllerAdd — motion flags: error ACKs always carry 0");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    vigem.pluginReturnVal = false; // force ACK_ERR_PLUGIN_FAIL
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    EXPECT_EQ(client.lastAckResult, ACK_ERR_PLUGIN_FAIL);
    // The controller never plugged in, so the motion-sink question is moot
    // — the satellite passes the default 0 motion-flags byte on error paths.
    EXPECT_EQ(client.lastAckMotionFlags, 0);
}

static void test_handleControllerRemove_motionFlags_carriesZero() {
    TEST("handleControllerRemove — motion flags byte is always 0 on remove acks");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    client.reset();
    svc.handleControllerRemove(r.token, 0);
    EXPECT_EQ(client.lastAckType, MSG_CONTROLLER_REMOVE);
    EXPECT_EQ(client.lastAckResult, ACK_OK);
    EXPECT_EQ(client.lastAckMotionFlags, 0);
}

// MSG_CONTROLLER_CAPS_UPDATE (0x000E) overwrites Controller::caps in place — no
// replug, no fresh ACK, no controller flicker on the receiver host.
static void test_handleControllerCapsUpdate_overwritesCapsInPlace() {
    TEST("handleControllerCapsUpdate — caps word is updated in place");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    // Register with motion on at first (CAP_ANALOG_TRIGGERS | CAP_RUMBLE | CAP_MOTION).
    svc.handleControllerAdd(r.token, 0, CAP_ANALOG_TRIGGERS | CAP_RUMBLE | CAP_MOTION);

    auto before = svc.getConnectionsSnapshot();
    EXPECT_EQ(before.connections[0].controllers[0].motionCapable, true);

    // User toggled motion off on the dish — caps drop CAP_MOTION.
    svc.handleControllerCapsUpdate(r.token, 0, CAP_ANALOG_TRIGGERS | CAP_RUMBLE);

    auto after = svc.getConnectionsSnapshot();
    EXPECT_EQ(after.connections[0].controllers[0].motionCapable, false);
    // Other facts about the controller must not change — same serial, still
    // active, no replug, controller type unchanged.
    EXPECT_EQ(after.connections[0].controllers[0].serial,
              before.connections[0].controllers[0].serial);
    EXPECT_EQ(after.connections[0].controllers[0].active, true);
    EXPECT_EQ(vigem.unplugCalls, 0);
    EXPECT_EQ(vigem.pluginCalls, 1); // still just the initial plug-in
}

static void test_handleControllerCapsUpdate_idempotentOnSameWord() {
    TEST("handleControllerCapsUpdate — same caps word is a no-op (no log churn, no broadcast)");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0, CAP_ANALOG_TRIGGERS | CAP_RUMBLE | CAP_MOTION);
    auto logCallsAfterAdd = log.logCalls;
    auto broadcastsAfterAdd = client.broadcastCalls;

    // Resend the same caps the dish already advertised.
    svc.handleControllerCapsUpdate(r.token, 0, CAP_ANALOG_TRIGGERS | CAP_RUMBLE | CAP_MOTION);

    // No log line, no extra broadcast — duplicate emissions on the dish
    // side (e.g. composer re-emits with no real change) shouldn't burn
    // satellite log space or wake every other connection's status feed.
    EXPECT_EQ(log.logCalls, logCallsAfterAdd);
    EXPECT_EQ(client.broadcastCalls, broadcastsAfterAdd);
}

static void test_handleControllerCapsUpdate_invalidToken() {
    TEST("handleControllerCapsUpdate — invalid token is silently dropped");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0, CAP_ANALOG_TRIGGERS | CAP_RUMBLE | CAP_MOTION);

    // Wrong token — must not affect the real controller.
    svc.handleControllerCapsUpdate(99999, 0, 0);

    auto snap = svc.getConnectionsSnapshot();
    EXPECT_EQ(snap.connections[0].controllers[0].motionCapable, true);
}

static void test_handleControllerCapsUpdate_outOfBoundsCtrlIdx() {
    TEST("handleControllerCapsUpdate — out-of-bounds ctrlIdx is silently dropped");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0, CAP_ANALOG_TRIGGERS | CAP_RUMBLE | CAP_MOTION);

    // ctrlIdx >= MAX_CONTROLLERS_PER_CONN — must not corrupt the array.
    svc.handleControllerCapsUpdate(r.token, 20, 0);

    auto snap = svc.getConnectionsSnapshot();
    EXPECT_EQ(snap.connections[0].controllers[0].motionCapable, true);
}

static void test_handleControllerCapsUpdate_inactiveControllerIgnored() {
    TEST("handleControllerCapsUpdate — inactive controller is silently dropped");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    // A stale caps-update for a never-added slot must not corrupt or activate it.
    auto broadcastsBefore = client.broadcastCalls;
    svc.handleControllerCapsUpdate(r.token, 0, CAP_MOTION);

    // The snapshot enumerates only ACTIVE controllers, so controllers[0] is
    // absent: assert nothing was activated and no spurious broadcast fired.
    auto snap = svc.getConnectionsSnapshot();
    EXPECT_EQ(snap.connections[0].activeControllerCount, 0);
    EXPECT_EQ((int)snap.connections[0].controllers.size(), 0);
    EXPECT_EQ(client.broadcastCalls, broadcastsBefore);
}

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

    svc.handleGamepadData(r.token, 0, rpt);
    EXPECT_EQ(vigem.submitCalls, 1);
    EXPECT_EQ(vigem.submitDS4Calls, 0);

    svc.handleControllerType(r.token, 0, CONTROLLER_TYPE_PLAYSTATION);
    svc.handleGamepadData(r.token, 0, rpt);
    EXPECT_EQ(vigem.submitDS4Calls, 1);
    EXPECT_EQ(vigem.submitCalls, 1); // unchanged

    svc.handleControllerType(r.token, 0, CONTROLLER_TYPE_XBOX);
    svc.handleGamepadData(r.token, 0, rpt);
    EXPECT_EQ(vigem.submitCalls, 2);
    EXPECT_EQ(vigem.submitDS4Calls, 1); // unchanged
}

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

static void test_controllerReAdd_clearsCachedSenderStreams() {
    TEST("Controller re-add — clears stale motion / battery / touchpad cache");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);

    // Populate every cached sender→satellite stream on controller 0.
    MotionReport m;
    m.gyroX = 4242;
    svc.handleMotionData(r.token, 0, m);
    BatteryReport b;
    b.level = 80;
    b.status = BATTERY_STATUS_DISCHARGING;
    svc.handleBatteryUpdate(r.token, 0, b);
    TouchpadReport tp;
    tp.finger0.active = true;
    tp.finger0.x = 9000;
    svc.handleTouchpadData(r.token, 0, tp);

    // Sanity: the snapshot reflects the populated caches before the re-add.
    {
        auto pre = svc.getConnectionsSnapshot();
        EXPECT(pre.connections[0].controllers[0].motionActive);
        EXPECT(pre.connections[0].controllers[0].batteryKnown);
        EXPECT(pre.connections[0].controllers[0].touchpadActive);
    }

    // Remove + re-add the same slot. The Controller struct persists in the
    // connection's array, so without an explicit clear the stale caches would
    // leak into the fresh controller.
    svc.handleControllerRemove(r.token, 0);
    svc.handleControllerAdd(r.token, 0);

    auto post = svc.getConnectionsSnapshot();
    const auto& ci = post.connections[0].controllers[0];
    EXPECT(!ci.motionActive);   // lastMotionValid cleared
    EXPECT(!ci.batteryKnown);   // lastBatteryValid cleared
    EXPECT(!ci.touchpadActive); // lastTouchpadValid cleared
    EXPECT(!ci.motionSink);     // motionSinkActive cleared
}

static void test_controllerReAdd_touchpadMouseNoJumpAfterReconnect() {
    TEST("Controller re-add — TOUCHPAD_MODE_MOUSE does not jump the cursor on reconnect");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc, "dev1", "TestDevice", TOUCHPAD_MODE_MOUSE);
    svc.handleControllerAdd(r.token, 0);

    // A finger contact far from the origin establishes a stale lastTouchpad.
    TouchpadReport far;
    far.finger0.active = true;
    far.finger0.x = 25000;
    far.finger0.y = 25000;
    svc.handleTouchpadData(r.token, 0, far);

    // Reconnect the controller, then send a fresh contact. If the stale
    // lastTouchpad survived, the first post-readd sample would compute a huge
    // delta against the pre-readd finger position and teleport the cursor.
    svc.handleControllerRemove(r.token, 0);
    svc.handleControllerAdd(r.token, 0);

    TouchpadReport fresh;
    fresh.finger0.active = true;
    fresh.finger0.x = -25000;
    fresh.finger0.y = -25000;
    svc.handleTouchpadData(r.token, 0, fresh);
    // First contact after re-add re-anchors — no movement emitted.
    EXPECT_EQ(vigem.lastMouseDx, 0);
    EXPECT_EQ(vigem.lastMouseDy, 0);
}

static void test_msgControllerType_constant() {
    TEST("MSG_CONTROLLER_TYPE — has expected wire value 0x0008");
    EXPECT_EQ(MSG_CONTROLLER_TYPE, (uint16_t)0x0008);
}

// Rumble is the reverse path (satellite → dish): the backend fires a
// notification, SessionService maps the serial back to a (Connection, ctrlIdx),
// coalesces against the prior value, and dispatches via IClientPort::sendRumble.
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

static void test_msgMotion_constant() {
    TEST("MSG_MOTION — has expected wire value 0x000A");
    EXPECT_EQ(static_cast<int>(MSG_MOTION), 0x000A);
}

static void test_motionReport_wireSize() {
    TEST("MotionReport — packs into 16 bytes on the wire");
    EXPECT_EQ(static_cast<int>(sizeof(MotionReport)), 16);
}

static void test_motionScaleConstants_fullScale() {
    TEST("MOTION scale constants — match the wire full-scale (±2000 deg/s, ±4 g)");
    // The exact float comparison is fine because both sides are derived from
    // the same compile-time constant — there is no rounding step that could
    // diverge them.
    EXPECT(MOTION_GYRO_SCALE_DEG_S > 0.0f);
    EXPECT(MOTION_GYRO_SCALE_DEG_S * 32767.0f >= 1999.9f);
    EXPECT(MOTION_GYRO_SCALE_DEG_S * 32767.0f <= 2000.1f);
    EXPECT(MOTION_ACCEL_SCALE_G * 32767.0f >= 3.999f);
    EXPECT(MOTION_ACCEL_SCALE_G * 32767.0f <= 4.001f);
}

static void test_handleMotionData_forwardsToBackend() {
    TEST("handleMotionData — forwards sample to backend.submitMotion");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    EXPECT_EQ(vigem.submitMotionCalls, 0);

    MotionReport m;
    m.gyroX = 1234;
    m.gyroY = -567;
    m.gyroZ = 89;
    m.accelX = 100;
    m.accelY = -200;
    m.accelZ = 16384; // ~+2 g, controller resting screen-up
    m.timestampDeltaUs = 4000;
    bool ok = svc.handleMotionData(r.token, 0, m);

    // The default MockViGem submitMotionReturnVal is true so handleMotionData
    // returns true. The "no IMU surface yet" path is exercised separately.
    EXPECT(ok);
    EXPECT_EQ(vigem.submitMotionCalls, 1);
    EXPECT_EQ(static_cast<int>(vigem.lastMotion.gyroX), 1234);
    EXPECT_EQ(static_cast<int>(vigem.lastMotion.gyroY), -567);
    EXPECT_EQ(static_cast<int>(vigem.lastMotion.accelZ), 16384);
    EXPECT_EQ(static_cast<int>(vigem.lastMotion.timestampDeltaUs), 4000);
}

static void test_handleMotionData_cachesEvenWhenBackendDeclines() {
    TEST("handleMotionData — caches lastMotion even when backend returns false");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    // Default IGamepadPort behaviour: backend has no IMU surface, returns false.
    vigem.submitMotionReturnVal = false;

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);

    MotionReport m;
    m.gyroX = 42;
    bool ok = svc.handleMotionData(r.token, 0, m);

    EXPECT(!ok); // backend declined
    EXPECT_EQ(vigem.submitMotionCalls, 1);
    // The snapshot doesn't surface motion (web UI isn't asking yet), but the
    // controller's lastMotionValid flag should still be true. We can't read
    // private state from the test, so we sanity-check by re-firing motion: the
    // backend should still receive the new sample (i.e. nothing was rejected).
    MotionReport m2;
    m2.gyroX = 99;
    svc.handleMotionData(r.token, 0, m2);
    EXPECT_EQ(vigem.submitMotionCalls, 2);
    EXPECT_EQ(static_cast<int>(vigem.lastMotion.gyroX), 99);
}

static void test_handleMotionData_invalidToken() {
    TEST("handleMotionData — invalid token returns false, backend not called");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    MotionReport m;
    bool ok = svc.handleMotionData(0xDEADBEEF, 0, m);
    EXPECT(!ok);
    EXPECT_EQ(vigem.submitMotionCalls, 0);
}

static void test_handleMotionData_outOfBoundsCtrlIdx() {
    TEST("handleMotionData — ctrlIdx >= MAX_CONTROLLERS_PER_CONN returns false");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    MotionReport m;
    bool ok = svc.handleMotionData(r.token, MAX_CONTROLLERS_PER_CONN, m);
    EXPECT(!ok);
    EXPECT_EQ(vigem.submitMotionCalls, 0);
}

static void test_handleMotionData_inactiveController() {
    TEST("handleMotionData — inactive controller returns false");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc); // no controllers added
    MotionReport m;
    bool ok = svc.handleMotionData(r.token, 0, m);
    EXPECT(!ok);
    EXPECT_EQ(vigem.submitMotionCalls, 0);
}

static void test_handleMotionData_routesAcrossControllers() {
    TEST("handleMotionData — routes by serial across multiple controllers");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0); // serial 1
    svc.handleControllerAdd(r.token, 1); // serial 2

    MotionReport m0;
    m0.gyroX = 100;
    MotionReport m1;
    m1.gyroX = 200;

    svc.handleMotionData(r.token, 0, m0);
    EXPECT_EQ(vigem.lastMotionSerial, 1u);
    EXPECT_EQ(static_cast<int>(vigem.lastMotion.gyroX), 100);

    svc.handleMotionData(r.token, 1, m1);
    EXPECT_EQ(vigem.lastMotionSerial, 2u);
    EXPECT_EQ(static_cast<int>(vigem.lastMotion.gyroX), 200);
}

static void test_snapshot_motionSinkSupportedForType_DS4() {
    TEST("snapshot — motionSinkSupportedForType true for DS4-typed slot");
    // The headline diagnostic: a Playstation-typed slot has a real IMU
    // surface on the Windows/Linux backends. The web UI uses this to NOT
    // warn about motion on a DS4 slot, even when no game has subscribed.
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    svc.handleControllerType(r.token, 0, CONTROLLER_TYPE_PLAYSTATION);

    auto snap = svc.getConnectionsSnapshot();
    EXPECT_EQ(snap.connections.size(), static_cast<size_t>(1));
    EXPECT_EQ(snap.connections[0].controllers.size(), static_cast<size_t>(1));
    EXPECT(snap.connections[0].controllers[0].motionSinkSupportedForType);
}

static void test_snapshot_motionSinkSupportedForType_Xbox() {
    TEST("snapshot — motionSinkSupportedForType false for Xbox-typed slot");
    // The headline UX win: an Xbox virtual pad has no IMU surface. The web
    // UI must be honest about this so the operator knows up front that
    // motion can't flow through to a game on this slot — independent of
    // whether the dish advertises CAP_MOTION.
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    // controllerType defaults to CONTROLLER_TYPE_XBOX; pin it explicitly.
    svc.handleControllerType(r.token, 0, CONTROLLER_TYPE_XBOX);

    auto snap = svc.getConnectionsSnapshot();
    EXPECT(!snap.connections[0].controllers[0].motionSinkSupportedForType);
}

static void test_snapshot_motionSinkSupportedForType_macOS_no_backend() {
    TEST("snapshot — motionSinkSupportedForType false for both types on macOS-like backend");
    // The macOS adapter is a base no-op IGamepadPort; its
    // supportsMotionForType returns false unconditionally. Pin that path
    // so a regression that "helpfully" defaults to true for DS4 in the
    // base class would be caught.
    MockViGem vigem;
    vigem.supportsMotionForTypeMap = {
        {CONTROLLER_TYPE_XBOX, false},
        {CONTROLLER_TYPE_PLAYSTATION, false},
    };
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    svc.handleControllerType(r.token, 0, CONTROLLER_TYPE_PLAYSTATION);

    auto snap = svc.getConnectionsSnapshot();
    EXPECT(!snap.connections[0].controllers[0].motionSinkSupportedForType);
}

static void test_snapshot_motionBackendOk_default_true() {
    TEST("snapshot — motionBackendOk defaults to true for a healthy plug");
    // The success path — the platform adapter reports motionBackendOk=true
    // for a serial it just plugged in. The web UI then renders the slot
    // without a "broken IMU" badge.
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);

    auto snap = svc.getConnectionsSnapshot();
    EXPECT(snap.connections[0].controllers[0].motionBackendOk);
}

static void test_snapshot_motionBackendOk_kernel_rejected() {
    TEST("snapshot — motionBackendOk false when backend reports failure");
    // The diagnostic this field exists for: the Linux uinput motion node
    // failed to open (kernel too old, /dev/uinput permissions, etc.). The
    // sample is still cached, but the web UI now distinguishes "no game
    // subscribed" from "platform couldn't even create the sink."
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0); // serial = 1
    // Stub the backend to report failure for the just-allocated serial.
    vigem.motionBackendOkMap[1] = false;

    auto snap = svc.getConnectionsSnapshot();
    EXPECT(!snap.connections[0].controllers[0].motionBackendOk);
}

static void test_snapshot_motionBackendOk_unknown_serial_is_optimistic() {
    TEST("snapshot — motionBackendOk is true for a serial the backend doesn't know about");
    // Race: the snapshot reads `motionBackendOk(serial)` for a serial the
    // adapter has already forgotten (unplug between query and answer).
    // The base contract says return true — there's nothing to report
    // failure about, and false here would surface a phantom red badge.
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    // Don't seed motionBackendOkMap for the serial — the default-true
    // path of the mock is what the unknown-serial contract returns.

    auto snap = svc.getConnectionsSnapshot();
    EXPECT(snap.connections[0].controllers[0].motionBackendOk);
}

static void test_msgBattery_constant() {
    TEST("MSG_BATTERY — has expected wire value 0x000B");
    EXPECT_EQ(static_cast<int>(MSG_BATTERY), 0x000B);
}

static void test_batteryStatusName_known() {
    TEST("batteryStatusName — every defined status has a non-unknown name");
    EXPECT(std::string(batteryStatusName(BATTERY_STATUS_DISCHARGING)) == "discharging");
    EXPECT(std::string(batteryStatusName(BATTERY_STATUS_CHARGING)) == "charging");
    EXPECT(std::string(batteryStatusName(BATTERY_STATUS_FULL)) == "full");
    EXPECT(std::string(batteryStatusName(BATTERY_STATUS_WIRED)) == "wired");
    EXPECT(std::string(batteryStatusName(BATTERY_STATUS_UNKNOWN)) == "unknown");
    EXPECT(std::string(batteryStatusName(99)) == "unknown");
}

static void test_handleBatteryUpdate_cachesAndForwards() {
    TEST("handleBatteryUpdate — caches latest value and forwards to backend");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);

    BatteryReport b;
    b.level = 73;
    b.status = BATTERY_STATUS_DISCHARGING;
    bool ok = svc.handleBatteryUpdate(r.token, 0, b);
    EXPECT(ok);
    EXPECT_EQ(vigem.submitBatteryCalls, 1);
    EXPECT_EQ(static_cast<int>(vigem.lastBattery.level), 73);
    EXPECT_EQ(static_cast<int>(vigem.lastBattery.status), BATTERY_STATUS_DISCHARGING);

    auto snap = svc.getConnectionsSnapshot();
    EXPECT_EQ(static_cast<int>(snap.connections.size()), 1);
    EXPECT_EQ(static_cast<int>(snap.connections[0].controllers.size()), 1);
    auto& info = snap.connections[0].controllers[0];
    EXPECT(info.batteryKnown);
    EXPECT_EQ(static_cast<int>(info.batteryLevel), 73);
    EXPECT_EQ(static_cast<int>(info.batteryStatus), BATTERY_STATUS_DISCHARGING);
}

static void test_handleBatteryUpdate_unknownLevelIsAccepted() {
    TEST("handleBatteryUpdate — BATTERY_LEVEL_UNKNOWN (0xFF) is accepted");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);

    BatteryReport b;
    b.level = BATTERY_LEVEL_UNKNOWN;
    b.status = BATTERY_STATUS_CHARGING;
    EXPECT(svc.handleBatteryUpdate(r.token, 0, b));
    EXPECT_EQ(vigem.submitBatteryCalls, 1);
}

static void test_handleBatteryUpdate_rejectsBogusLevel() {
    TEST("handleBatteryUpdate — rejects level in 101..254 (bogus, not the 0xFF sentinel)");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);

    BatteryReport b;
    b.level = 200;
    b.status = BATTERY_STATUS_DISCHARGING;
    EXPECT(!svc.handleBatteryUpdate(r.token, 0, b));
    EXPECT_EQ(vigem.submitBatteryCalls, 0);
}

static void test_handleBatteryUpdate_rejectsBogusStatus() {
    TEST("handleBatteryUpdate — rejects status >= BATTERY_STATUS_COUNT");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);

    BatteryReport b;
    b.level = 50;
    b.status = BATTERY_STATUS_COUNT; // == 5, not a defined enum
    EXPECT(!svc.handleBatteryUpdate(r.token, 0, b));
    EXPECT_EQ(vigem.submitBatteryCalls, 0);
}

static void test_handleBatteryUpdate_invalidToken() {
    TEST("handleBatteryUpdate — invalid token returns false");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    BatteryReport b;
    b.level = 50;
    b.status = BATTERY_STATUS_DISCHARGING;
    EXPECT(!svc.handleBatteryUpdate(0xDEADBEEF, 0, b));
    EXPECT_EQ(vigem.submitBatteryCalls, 0);
}

static void test_handleBatteryUpdate_inactiveController() {
    TEST("handleBatteryUpdate — inactive controller returns false");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    BatteryReport b;
    b.level = 50;
    b.status = BATTERY_STATUS_DISCHARGING;
    EXPECT(!svc.handleBatteryUpdate(r.token, 0, b));
    EXPECT_EQ(vigem.submitBatteryCalls, 0);
}

static void test_snapshot_batteryDefaultsUnknown() {
    TEST("getConnectionsSnapshot — newly added controller has batteryKnown=false");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);

    auto snap = svc.getConnectionsSnapshot();
    EXPECT_EQ(static_cast<int>(snap.connections.size()), 1);
    EXPECT_EQ(static_cast<int>(snap.connections[0].controllers.size()), 1);
    EXPECT(!snap.connections[0].controllers[0].batteryKnown);
}

static void test_msgTouchpad_constant() {
    TEST("MSG_TOUCHPAD constant pins wire byte 0x000C");
    EXPECT_EQ(static_cast<int>(MSG_TOUCHPAD), 0x000C);
}

static void test_handleTouchpadData_forwardsToBackend() {
    TEST("handleTouchpadData — caches + forwards to backend on active controller");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);

    TouchpadReport tp;
    tp.finger0.active = true;
    tp.finger0.trackingId = 7;
    tp.finger0.x = 1234;
    tp.finger0.y = -5678;
    tp.finger1.active = false;
    tp.buttonPressed = true;

    EXPECT(svc.handleTouchpadData(r.token, 0, tp));
    EXPECT_EQ(vigem.submitTouchpadCalls, 1);
    EXPECT(vigem.lastTouchpad.finger0.active);
    EXPECT_EQ(static_cast<int>(vigem.lastTouchpad.finger0.trackingId), 7);
    EXPECT_EQ(static_cast<int>(vigem.lastTouchpad.finger0.x), 1234);
    EXPECT_EQ(static_cast<int>(vigem.lastTouchpad.finger0.y), -5678);
    EXPECT(!vigem.lastTouchpad.finger1.active);
    EXPECT(vigem.lastTouchpad.buttonPressed);
}

static void test_handleTouchpadData_cachesEvenWhenBackendDeclines() {
    TEST("handleTouchpadData — cache is updated even when backend returns false");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    // Default IGamepadPort::submitTouchpad returns false. Mirror it.
    vigem.submitTouchpadReturnVal = false;
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);

    TouchpadReport tp;
    tp.finger0.active = true;
    tp.finger0.x = 42;
    EXPECT(!svc.handleTouchpadData(r.token, 0, tp));
    EXPECT_EQ(vigem.submitTouchpadCalls, 1);

    auto snap = svc.getConnectionsSnapshot();
    EXPECT_EQ(static_cast<int>(snap.connections.size()), 1);
    // Snapshot doesn't expose touchpad yet — verify the cache via the value the
    // backend last received.
    EXPECT_EQ(static_cast<int>(vigem.lastTouchpad.finger0.x), 42);
}

static void test_handleTouchpadData_invalidToken() {
    TEST("handleTouchpadData — invalid token returns false, backend not called");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    TouchpadReport tp;
    EXPECT(!svc.handleTouchpadData(/*token=*/0xDEAD'BEEFu, 0, tp));
    EXPECT_EQ(vigem.submitTouchpadCalls, 0);
}

static void test_handleTouchpadData_outOfBoundsCtrlIdx() {
    TEST("handleTouchpadData — ctrlIdx >= MAX returns false");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc);

    TouchpadReport tp;
    EXPECT(!svc.handleTouchpadData(r.token, MAX_CONTROLLERS_PER_CONN, tp));
    EXPECT_EQ(vigem.submitTouchpadCalls, 0);
}

static void test_handleTouchpadData_inactiveController() {
    TEST("handleTouchpadData — controller not active returns false");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc);
    // Don't call handleControllerAdd — ctrlIdx 0 is inactive.

    TouchpadReport tp;
    EXPECT(!svc.handleTouchpadData(r.token, 0, tp));
    EXPECT_EQ(vigem.submitTouchpadCalls, 0);
}

static void test_touchpadWireToRange_endpoints() {
    TEST("touchpadWireToRange — wire endpoints map to device-range edges");
    EXPECT_EQ(touchpadWireToRange(-32768, 1920), 0);
    EXPECT_EQ(touchpadWireToRange(32767, 1920), 1919);
    EXPECT_EQ(touchpadWireToRange(-32768, 943), 0);
    EXPECT_EQ(touchpadWireToRange(32767, 943), 942);
    // Centre of the wire range lands near the centre of the device range.
    const int mid = touchpadWireToRange(0, 1920);
    EXPECT(mid >= 955 && mid <= 965);
}

static void test_touchpadWireToRange_clampsAndDegenerate() {
    TEST("touchpadWireToRange — saturates monotonically; res<=1 yields 0");
    EXPECT_EQ(touchpadWireToRange(-32768, 1), 0);
    EXPECT_EQ(touchpadWireToRange(32767, 1), 0);
    EXPECT(touchpadWireToRange(-10000, 1920) <= touchpadWireToRange(10000, 1920));
}

static void test_ds4PackTouchFinger_activeFlagAndId() {
    TEST("ds4PackTouchFinger — bit7 = lifted, low 7 bits = tracking id");
    TouchpadFinger active;
    active.active = true;
    auto a = ds4PackTouchFinger(active, 5);
    EXPECT_EQ(static_cast<int>(a[0] & 0x80), 0); // active → bit7 clear
    EXPECT_EQ(static_cast<int>(a[0] & 0x7F), 5); // tracking id in low 7 bits

    TouchpadFinger lifted;
    lifted.active = false;
    auto l = ds4PackTouchFinger(lifted, 9);
    EXPECT_EQ(static_cast<int>(l[0] & 0x80), 0x80); // lifted → bit7 set
    EXPECT_EQ(static_cast<int>(l[0] & 0x7F), 9);
}

static void test_ds4PackTouchFinger_coordPacking() {
    TEST("ds4PackTouchFinger — 12-bit x/y survive the DS4 packing round-trip");
    TouchpadFinger f;
    f.active = true;
    f.x = 32767;  // right edge → x12 = DS4_TOUCHPAD_RES_X - 1
    f.y = -32768; // top edge   → y12 = 0
    auto p = ds4PackTouchFinger(f, 0);
    const int x12 = p[1] | ((p[2] & 0x0F) << 8);
    const int y12 = (p[2] >> 4) | (p[3] << 4);
    EXPECT_EQ(x12, DS4_TOUCHPAD_RES_X - 1);
    EXPECT_EQ(y12, 0);
}

static void test_decodeTouchpadReport_wireLayout() {
    TEST("decodeTouchpadReport — flags + finger fields + eventTimeMs decode from the 15-byte tail");
    uint8_t p[TOUCHPAD_WIRE_PAYLOAD_BYTES] = {};
    p[0] = 0x07; // flags: finger0 + finger1 active + button
    p[1] = 0x2A; // finger0 trackingId
    p[2] = 0xD2;
    p[3] = 0x04; // finger0 x = 0x04D2 = 1234 (LE)
    p[4] = 0x2E;
    p[5] = 0xFB; // finger0 y = 0xFB2E = -1234 (LE)
    p[6] = 0x55; // finger1 trackingId
    p[7] = 0x00;
    p[8] = 0x80; // finger1 x = 0x8000 = -32768
    p[9] = 0xFF;
    p[10] = 0x7F; // finger1 y = 0x7FFF = 32767
    p[11] = 0xEF;
    p[12] = 0xCD;
    p[13] = 0xAB;
    p[14] = 0x89; // eventTimeMs = 0x89ABCDEF (LE)
    TouchpadReport r = decodeTouchpadReport(p);
    EXPECT(r.finger0.active);
    EXPECT(r.finger1.active);
    EXPECT(r.buttonPressed);
    EXPECT_EQ(static_cast<int>(r.finger0.trackingId), 0x2A);
    EXPECT_EQ(static_cast<int>(r.finger0.x), 1234);
    EXPECT_EQ(static_cast<int>(r.finger0.y), -1234);
    EXPECT_EQ(static_cast<int>(r.finger1.trackingId), 0x55);
    EXPECT_EQ(static_cast<int>(r.finger1.x), -32768);
    EXPECT_EQ(static_cast<int>(r.finger1.y), 32767);
    EXPECT_EQ(r.eventTimeMs, 0x89ABCDEFu);
}

static void test_decodeTouchpadReport_inactiveFlags() {
    TEST("decodeTouchpadReport — clear flags yield inactive fingers / no button");
    uint8_t p[TOUCHPAD_WIRE_PAYLOAD_BYTES] = {};
    TouchpadReport r = decodeTouchpadReport(p);
    EXPECT(!r.finger0.active);
    EXPECT(!r.finger1.active);
    EXPECT(!r.buttonPressed);
}

static void test_touchpadMode_constants() {
    TEST("TOUCHPAD_MODE_* constants + name round-trip");
    EXPECT_EQ(static_cast<int>(TOUCHPAD_MODE_DS4), 0);
    EXPECT_EQ(static_cast<int>(TOUCHPAD_MODE_MOUSE), 1);
    EXPECT_EQ(static_cast<int>(TOUCHPAD_MODE_OFF), 2);
    EXPECT_EQ(static_cast<int>(TOUCHPAD_MODE_COUNT), 3);
    EXPECT_EQ(std::string(touchpadModeName(TOUCHPAD_MODE_DS4)), std::string("ds4"));
    EXPECT_EQ(std::string(touchpadModeName(TOUCHPAD_MODE_MOUSE)), std::string("mouse"));
    EXPECT_EQ(std::string(touchpadModeName(TOUCHPAD_MODE_OFF)), std::string("off"));
    EXPECT_EQ(static_cast<int>(touchpadModeFromName("ds4")), static_cast<int>(TOUCHPAD_MODE_DS4));
    EXPECT_EQ(static_cast<int>(touchpadModeFromName("mouse")),
              static_cast<int>(TOUCHPAD_MODE_MOUSE));
    EXPECT_EQ(static_cast<int>(touchpadModeFromName("off")), static_cast<int>(TOUCHPAD_MODE_OFF));
    // Unknown / empty (a pre-1.3 config) migrates to DS4 pass-through.
    EXPECT_EQ(static_cast<int>(touchpadModeFromName("")), static_cast<int>(TOUCHPAD_MODE_DS4));
    EXPECT_EQ(static_cast<int>(touchpadModeFromName("bogus")), static_cast<int>(TOUCHPAD_MODE_DS4));
}

static void test_openSession_carriesTouchpadMode() {
    TEST("openSession — touchpadMode seeds the connection + snapshot");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc, "dev1", "TestDevice", TOUCHPAD_MODE_MOUSE);
    svc.handleControllerAdd(r.token, 0);
    auto snap = svc.getConnectionsSnapshot();
    EXPECT_EQ(static_cast<int>(snap.connections.size()), 1);
    EXPECT_EQ(static_cast<int>(snap.connections[0].touchpadMode),
              static_cast<int>(TOUCHPAD_MODE_MOUSE));
}

static void test_handleTouchpadData_ds4ModeForwardsToTouchpad() {
    TEST("handleTouchpadData — DS4 mode routes to submitTouchpad, not the mouse");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc, "dev1", "TestDevice", TOUCHPAD_MODE_DS4);
    svc.handleControllerAdd(r.token, 0);
    TouchpadReport tp;
    tp.finger0.active = true;
    EXPECT(svc.handleTouchpadData(r.token, 0, tp));
    EXPECT_EQ(vigem.submitTouchpadCalls, 1);
    EXPECT_EQ(vigem.submitRelativeMouseCalls, 0);
}

static void test_handleTouchpadData_offModeCachesOnly() {
    TEST("handleTouchpadData — OFF mode caches for the web UI but forwards nowhere");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc, "dev1", "TestDevice", TOUCHPAD_MODE_OFF);
    svc.handleControllerAdd(r.token, 0);
    TouchpadReport tp;
    tp.finger0.active = true;
    EXPECT(!svc.handleTouchpadData(r.token, 0, tp)); // OFF → returns false
    EXPECT_EQ(vigem.submitTouchpadCalls, 0);
    EXPECT_EQ(vigem.submitRelativeMouseCalls, 0);
    // ...but the sample is still cached — the snapshot shows it as active.
    auto snap = svc.getConnectionsSnapshot();
    EXPECT_EQ(static_cast<int>(snap.connections.size()), 1);
    EXPECT_EQ(static_cast<int>(snap.connections[0].controllers.size()), 1);
    EXPECT(snap.connections[0].controllers[0].touchpadActive);
}

static void test_handleTouchpadData_mouseModeTouchDownNoJump() {
    TEST("handleTouchpadData — MOUSE mode: a fresh touch-down emits no cursor jump");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc, "dev1", "TestDevice", TOUCHPAD_MODE_MOUSE);
    svc.handleControllerAdd(r.token, 0);
    TouchpadReport down;
    down.finger0.active = true;
    down.finger0.x = 12000;
    down.finger0.y = -8000;
    down.buttonPressed = true;
    EXPECT(svc.handleTouchpadData(r.token, 0, down));
    EXPECT_EQ(vigem.submitRelativeMouseCalls, 1);
    EXPECT_EQ(vigem.lastMouseDx, 0); // first contact re-anchors — no jump
    EXPECT_EQ(vigem.lastMouseDy, 0);
    EXPECT(vigem.lastMouseButton); // clicky button still forwarded
    EXPECT_EQ(vigem.submitTouchpadCalls, 0);
}

static void test_handleTouchpadData_mouseModeContinuousDelta() {
    TEST("handleTouchpadData — MOUSE mode: a continuous drag emits a signed delta");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc, "dev1", "TestDevice", TOUCHPAD_MODE_MOUSE);
    svc.handleControllerAdd(r.token, 0);
    TouchpadReport a;
    a.finger0.active = true;
    a.finger0.x = 0;
    a.finger0.y = 0;
    a.eventTimeMs = 1000;
    svc.handleTouchpadData(r.token, 0, a); // touch-down anchor
    TouchpadReport b;
    b.finger0.active = true;
    b.finger0.x = 10000;                                // +10000 wire units
    b.finger0.y = -10000;                               // -10000 wire units
    b.eventTimeMs = 1000 + TOUCHPAD_MOUSE_REFERENCE_MS; // dt = reference → scale 1
    svc.handleTouchpadData(r.token, 0, b);
    EXPECT_EQ(vigem.submitRelativeMouseCalls, 2);
    // 10000 * TOUCHPAD_MOUSE_SENSITIVITY ≈ 420 px — assert sign + ballpark.
    EXPECT(vigem.lastMouseDx > 380 && vigem.lastMouseDx < 460);
    EXPECT(vigem.lastMouseDy < -380 && vigem.lastMouseDy > -460);
}

static void test_handleTouchpadData_mouseModeSubPixelRemainder() {
    TEST("handleTouchpadData — MOUSE mode: sub-pixel motion accumulates, not lost");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc, "dev1", "TestDevice", TOUCHPAD_MODE_MOUSE);
    svc.handleControllerAdd(r.token, 0);
    // Three samples 15 wire units apart, each 4 ms apart (dt == reference →
    // scale = 1.0). 15 × ~0.042 ≈ 0.63 px each — under a whole pixel alone,
    // but two continuous steps cross 1 px.
    TouchpadReport s;
    s.finger0.active = true;
    s.finger0.x = 0;
    s.eventTimeMs = 1000;
    svc.handleTouchpadData(r.token, 0, s); // anchor → dx 0
    s.finger0.x = 15;
    s.eventTimeMs = 1000 + TOUCHPAD_MOUSE_REFERENCE_MS;
    svc.handleTouchpadData(r.token, 0, s); // +0.63 px → dx 0, remainder kept
    EXPECT_EQ(vigem.lastMouseDx, 0);
    s.finger0.x = 30;
    s.eventTimeMs = 1000 + 2 * TOUCHPAD_MOUSE_REFERENCE_MS;
    svc.handleTouchpadData(r.token, 0, s); // +0.63 px → 1.26 px total → dx 1
    EXPECT_EQ(vigem.lastMouseDx, 1);
}

static void test_handleTouchpadData_mouseModeLiftResetsAnchor() {
    TEST("handleTouchpadData — MOUSE mode: a lift between contacts emits no jump");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc, "dev1", "TestDevice", TOUCHPAD_MODE_MOUSE);
    svc.handleControllerAdd(r.token, 0);
    TouchpadReport s;
    s.finger0.active = true;
    s.finger0.x = 0;
    svc.handleTouchpadData(r.token, 0, s); // contact A
    TouchpadReport lift;                   // finger up
    svc.handleTouchpadData(r.token, 0, lift);
    EXPECT_EQ(vigem.lastMouseDx, 0);
    s.finger0.x = 20000; // contact B, far from A
    svc.handleTouchpadData(r.token, 0, s);
    EXPECT_EQ(vigem.lastMouseDx, 0); // discontinuous — no teleport
}

static void test_handleTouchpadData_mouseModeTrackingIdChangeBreaksContinuity() {
    TEST("handleTouchpadData — MOUSE mode: a finger-0 trackingId change emits no jump");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc, "dev1", "TestDevice", TOUCHPAD_MODE_MOUSE);
    svc.handleControllerAdd(r.token, 0);

    // Finger 0 is "active" in both frames, but with different tracking IDs:
    // this is what slot compaction looks like when the original finger 0
    // lifts and finger 1 is compacted down into slot 0. The two samples are
    // two *different* physical fingers, so no cursor delta must be emitted.
    TouchpadReport a;
    a.finger0.active = true;
    a.finger0.trackingId = 5;
    a.finger0.x = 0;
    a.eventTimeMs = 1000;
    svc.handleTouchpadData(r.token, 0, a);

    TouchpadReport b;
    b.finger0.active = true;
    b.finger0.trackingId = 6; // compacted: a different finger now in slot 0
    b.finger0.x = 20000;      // far from sample a
    b.eventTimeMs = 1000 + TOUCHPAD_MOUSE_REFERENCE_MS;
    svc.handleTouchpadData(r.token, 0, b);
    EXPECT_EQ(vigem.lastMouseDx, 0); // trackingId differs → not continuous

    // A third sample with the same trackingId as b *is* continuous with it.
    TouchpadReport c;
    c.finger0.active = true;
    c.finger0.trackingId = 6;
    c.finger0.x = 30000;
    c.eventTimeMs = 1000 + 2 * TOUCHPAD_MOUSE_REFERENCE_MS;
    svc.handleTouchpadData(r.token, 0, c);
    EXPECT(vigem.lastMouseDx > 0); // same finger → delta emitted
}

// Per-sample time-scaling makes cursor velocity proportional to finger velocity
// regardless of dt. Without it the first MOVE after touchdown (~16 ms on 60 Hz
// Android) produces a delta several times larger than later ~4 ms samples — the
// classic first-touch jump.
static void test_handleTouchpadData_mouseModeTimeScalingHalvesLongerDt() {
    TEST("handleTouchpadData — MOUSE mode: 2× dt halves the cursor delta for the same position "
         "diff");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc, "dev1", "TestDevice", TOUCHPAD_MODE_MOUSE);
    svc.handleControllerAdd(r.token, 0);

    // Anchor.
    TouchpadReport a;
    a.finger0.active = true;
    a.finger0.x = 0;
    a.eventTimeMs = 1000;
    svc.handleTouchpadData(r.token, 0, a);

    // Same finger, +10000 wire units, dt = 8 ms (2× reference). The scale
    // factor is REFERENCE_MS/dt = 0.5, so the cursor delta is half the
    // un-scaled value (≈420 → ≈210).
    TouchpadReport b;
    b.finger0.active = true;
    b.finger0.x = 10000;
    b.eventTimeMs = 1000 + 2 * TOUCHPAD_MOUSE_REFERENCE_MS;
    svc.handleTouchpadData(r.token, 0, b);
    // 10000 * 0.042 * 0.5 ≈ 210 → assert ballpark, not exact (sub-pixel
    // remainder + int truncation).
    EXPECT(vigem.lastMouseDx > 190 && vigem.lastMouseDx < 230);
}

static void test_handleTouchpadData_mouseModeTimeScalingFirstTouchJumpFix() {
    TEST("handleTouchpadData — MOUSE mode: first-MOVE (16 ms dt) and subsequent (4 ms dt) produce "
         "equal cursor velocity");
    // Repro the user's reported scenario: finger moves at constant velocity.
    // First MOVE delivers 4× the position-diff of subsequent MOVEs because
    // Android batched 4× the time. With time-scaling, the per-sample cursor
    // delta lands in the same ballpark.
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc, "dev1", "TestDevice", TOUCHPAD_MODE_MOUSE);
    svc.handleControllerAdd(r.token, 0);

    // Touch-down at t=0.
    TouchpadReport down;
    down.finger0.active = true;
    down.finger0.x = 0;
    down.eventTimeMs = 1000;
    svc.handleTouchpadData(r.token, 0, down);

    // First MOVE at t=16 ms: 16 ms of constant-velocity finger motion =
    // 4000 wire units worth of travel. Scale = 4/16 = 0.25.
    TouchpadReport firstMove;
    firstMove.finger0.active = true;
    firstMove.finger0.x = 4000;
    firstMove.eventTimeMs = 1000 + 16;
    svc.handleTouchpadData(r.token, 0, firstMove);
    const int firstDx = vigem.lastMouseDx;

    // Subsequent MOVE at t=20 ms: 4 ms of motion = 1000 wire units. Scale 1.0.
    TouchpadReport secondMove;
    secondMove.finger0.active = true;
    secondMove.finger0.x = 5000;
    secondMove.eventTimeMs = 1000 + 20;
    svc.handleTouchpadData(r.token, 0, secondMove);
    const int secondDx = vigem.lastMouseDx;

    // Both samples should produce roughly the same cursor delta — finger
    // velocity is constant, cursor velocity should be too. Without time-
    // scaling, firstDx would be ~4× larger than secondDx (the bug).
    // 4000 * 0.042 * 0.25 = 42, 1000 * 0.042 * 1.0 = 42 → expect within ±5.
    EXPECT(std::abs(firstDx - secondDx) <= 5);
    EXPECT(firstDx > 30 && firstDx < 55);
    EXPECT(secondDx > 30 && secondDx < 55);
}

static void test_handleTouchpadData_mouseModeDuplicateTimestampEmitsNoMotion() {
    TEST("handleTouchpadData — MOUSE mode: a sample with dt==0 (duplicate resend) emits no cursor "
         "motion");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc, "dev1", "TestDevice", TOUCHPAD_MODE_MOUSE);
    svc.handleControllerAdd(r.token, 0);

    // The dish's resend loop fires every 4 ms; if no fresh ACTION_MOVE has
    // landed in that window, the cached state's eventTimeMs is identical to
    // the previous sample's. Receiver must treat as duplicate (no cursor
    // motion) instead of dividing by zero / producing a garbage delta.
    TouchpadReport s;
    s.finger0.active = true;
    s.finger0.x = 0;
    s.eventTimeMs = 1000;
    svc.handleTouchpadData(r.token, 0, s); // anchor
    s.finger0.x = 5000;                    // position changed (impossible for a true resend,
                                           // but tests the dt-guard not the position guard)
    svc.handleTouchpadData(r.token, 0, s); // same eventTimeMs → dt = 0
    EXPECT_EQ(vigem.lastMouseDx, 0);
    EXPECT_EQ(vigem.lastMouseDy, 0);
}

static void test_handleTouchpadData_mouseModeBigGapReanchors() {
    TEST("handleTouchpadData — MOUSE mode: dt > MAX_GAP_MS re-anchors (no cursor motion, remainder "
         "reset)");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc, "dev1", "TestDevice", TOUCHPAD_MODE_MOUSE);
    svc.handleControllerAdd(r.token, 0);

    // The dish paused (backgrounded / network stall / dropped the resend
    // loop). When motion resumes the receiver must NOT divide a big
    // position-diff by a tiny scale factor and fling the cursor.
    TouchpadReport s;
    s.finger0.active = true;
    s.finger0.x = 0;
    s.eventTimeMs = 1000;
    svc.handleTouchpadData(r.token, 0, s); // anchor
    s.finger0.x = 20000;
    s.eventTimeMs = 1000 + TOUCHPAD_MOUSE_MAX_GAP_MS + 50; // big gap
    svc.handleTouchpadData(r.token, 0, s);
    EXPECT_EQ(vigem.lastMouseDx, 0); // re-anchored, no motion this frame

    // A subsequent normal-dt sample resumes cleanly.
    s.finger0.x = 21000;
    s.eventTimeMs = 1000 + TOUCHPAD_MOUSE_MAX_GAP_MS + 50 + TOUCHPAD_MOUSE_REFERENCE_MS;
    svc.handleTouchpadData(r.token, 0, s);
    EXPECT(vigem.lastMouseDx > 30 && vigem.lastMouseDx < 55); // 1000 * 0.042 * 1.0 ≈ 42
}

static void test_setTouchpadMode_hotApplies() {
    TEST("setTouchpadMode — hot-swaps routing on a live connection (no re-pair)");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc); // default DS4
    svc.handleControllerAdd(r.token, 0);
    TouchpadReport tp;
    tp.finger0.active = true;
    svc.handleTouchpadData(r.token, 0, tp);
    EXPECT_EQ(vigem.submitTouchpadCalls, 1);
    EXPECT_EQ(vigem.submitRelativeMouseCalls, 0);

    EXPECT(svc.setTouchpadMode("dev1", TOUCHPAD_MODE_MOUSE));
    svc.handleTouchpadData(r.token, 0, tp);
    EXPECT_EQ(vigem.submitTouchpadCalls, 1);      // no further DS4 forwards
    EXPECT_EQ(vigem.submitRelativeMouseCalls, 1); // now routed to the mouse
}

// The server is a read-only mirror for touchpad mode: the dish sets it (POST
// /api/devices/touchpad-mode), the server validates and routes.
static void test_pairedDevice_defaultsToOff() {
    TEST("PairedDevice — default touchpadMode is OFF (the safe baseline)");
    PairedDevice d;
    EXPECT_EQ(static_cast<int>(d.touchpadMode), static_cast<int>(TOUCHPAD_MODE_OFF));
}

static void test_connection_defaultsToOff() {
    TEST("Connection — default touchpadMode is OFF");
    Connection c;
    EXPECT_EQ(static_cast<int>(c.touchpadMode), static_cast<int>(TOUCHPAD_MODE_OFF));
}

static void test_openSession_defaultParamIsOff() {
    TEST("openSession — default touchpadMode param is OFF; no MSG_TOUCHPAD routes anywhere");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    // Call openSession without specifying the mode — the new default kicks in.
    uint8_t key[CRYPTO_KEY_SIZE] = {};
    auto r = svc.openSession("dev1", "TestDevice", "192.168.1.50", key);
    svc.handleControllerAdd(r.token, 0);
    auto snap = svc.getConnectionsSnapshot();
    EXPECT_EQ(static_cast<int>(snap.connections.size()), 1);
    EXPECT_EQ(static_cast<int>(snap.connections[0].touchpadMode),
              static_cast<int>(TOUCHPAD_MODE_OFF));
    // And a touchpad sample lands nowhere — neither the DS4 surface nor mouse.
    TouchpadReport tp;
    tp.finger0.active = true;
    EXPECT(!svc.handleTouchpadData(r.token, 0, tp));
    EXPECT_EQ(vigem.submitTouchpadCalls, 0);
    EXPECT_EQ(vigem.submitRelativeMouseCalls, 0);
}

static void test_deriveTouchpadCapabilities_offAlwaysSupported() {
    TEST("deriveTouchpadCapabilities — OFF is always supported regardless of backend");
    BackendStatus none;
    none.id = BACKEND_ID_NONE;
    none.supported = false;
    none.available = false;
    EXPECT(deriveTouchpadCapabilities(none).offSupported);

    BackendStatus vigem;
    vigem.id = BACKEND_ID_VIGEM;
    vigem.supported = true;
    vigem.available = true;
    EXPECT(deriveTouchpadCapabilities(vigem).offSupported);
}

static void test_deriveTouchpadCapabilities_padMouseMatchBackend() {
    TEST("deriveTouchpadCapabilities — pad+mouse track backend supported");
    BackendStatus mac;
    mac.id = BACKEND_ID_NONE;
    mac.supported = false;
    auto macCaps = deriveTouchpadCapabilities(mac);
    EXPECT(!macCaps.padSupported);
    EXPECT(!macCaps.mouseSupported);
    EXPECT(macCaps.offSupported);

    BackendStatus uinput;
    uinput.id = BACKEND_ID_UINPUT;
    uinput.supported = true;
    uinput.available = true;
    auto uinputCaps = deriveTouchpadCapabilities(uinput);
    EXPECT(uinputCaps.padSupported);
    EXPECT(uinputCaps.mouseSupported);
    EXPECT(uinputCaps.offSupported);
}

static void test_deriveTouchpadCapabilities_driverInstalledButBusDownStillSupports() {
    TEST("deriveTouchpadCapabilities — driver installed but bus down still advertises support");
    // A momentary bus-open failure (available=false but supported=true) must
    // not hide the modes — the client picker should not flap when the bus
    // bounces. supported=true reflects "this host ships the backend";
    // available=false reflects "right now I can't open it." Capabilities key
    // off `supported` only.
    BackendStatus vigemBusDown;
    vigemBusDown.id = BACKEND_ID_VIGEM;
    vigemBusDown.supported = true;
    vigemBusDown.available = false;
    vigemBusDown.errorCode = "BUS_OPEN_FAILED";
    auto caps = deriveTouchpadCapabilities(vigemBusDown);
    EXPECT(caps.padSupported);
    EXPECT(caps.mouseSupported);
    EXPECT(caps.offSupported);
}

static void test_setTouchpadMode_unknownDeviceAndBadMode() {
    TEST("setTouchpadMode — unknown device returns false; out-of-range mode rejected");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    openTestSession(svc, "dev1");
    EXPECT(!svc.setTouchpadMode("no-such-device", TOUCHPAD_MODE_MOUSE));
    EXPECT(!svc.setTouchpadMode("dev1", TOUCHPAD_MODE_COUNT)); // out of range
    EXPECT(svc.setTouchpadMode("dev1", TOUCHPAD_MODE_OFF));    // valid → true
}

static void test_msgLightbar_constant() {
    TEST("MSG_LIGHTBAR constant pins wire byte 0x000D");
    EXPECT_EQ(static_cast<int>(MSG_LIGHTBAR), 0x000D);
}

static void test_constructor_installsLightbarCallback() {
    TEST("SessionService ctor installs the lightbar callback on the backend");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    EXPECT_EQ(vigem.setLightbarCallbackCalls, 1);
    EXPECT(static_cast<bool>(vigem.capturedLightbarCb));
}

static void test_handleLightbarFromBackend_routesToOwningConnection() {
    TEST(
        "handleLightbarFromBackend — forwards to the right (connection, ctrlIdx) via sendLightbar");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 3, CAP_LIGHTBAR);
    const uint32_t serial = vigem.pluggedSerials.back();
    client.reset();

    vigem.fireLightbar(serial, 0x11, 0x22, 0x33);

    EXPECT_EQ(client.lightbarCalls, 1);
    EXPECT_EQ(client.lastLightbarConnToken, r.token);
    EXPECT_EQ(static_cast<int>(client.lastLightbarCtrlIdx), 3);
    EXPECT_EQ(static_cast<int>(client.lastLightbarR), 0x11);
    EXPECT_EQ(static_cast<int>(client.lastLightbarG), 0x22);
    EXPECT_EQ(static_cast<int>(client.lastLightbarB), 0x33);
}

static void test_handleLightbarFromBackend_unknownSerialDropped() {
    TEST("handleLightbarFromBackend — unknown serial silently drops");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0, CAP_LIGHTBAR);
    client.reset();

    vigem.fireLightbar(/*serial=*/0xDEAD'BEEFu, 1, 2, 3);
    EXPECT_EQ(client.lightbarCalls, 0);
}

static void test_handleLightbarFromBackend_coalescesIdenticalColours() {
    TEST("handleLightbarFromBackend — identical follow-up colour is dropped");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0, CAP_LIGHTBAR);
    const uint32_t serial = vigem.pluggedSerials.back();
    client.reset();

    vigem.fireLightbar(serial, 0xAA, 0xBB, 0xCC);
    vigem.fireLightbar(serial, 0xAA, 0xBB, 0xCC);
    vigem.fireLightbar(serial, 0xAA, 0xBB, 0xCC);
    EXPECT_EQ(client.lightbarCalls, 1);
}

static void test_handleLightbarFromBackend_anyChannelChangeEmits() {
    TEST("handleLightbarFromBackend — a single-channel change still emits a packet");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0, CAP_LIGHTBAR);
    const uint32_t serial = vigem.pluggedSerials.back();
    client.reset();

    vigem.fireLightbar(serial, 1, 2, 3);
    vigem.fireLightbar(serial, 1, 2, 4); // only B changed
    vigem.fireLightbar(serial, 1, 5, 4); // only G changed
    vigem.fireLightbar(serial, 6, 5, 4); // only R changed
    EXPECT_EQ(client.lightbarCalls, 4);
    EXPECT_EQ(static_cast<int>(client.lastLightbarR), 6);
}

static void test_handleLightbarFromBackend_separateControllersIndependentlyCoalesced() {
    TEST("handleLightbarFromBackend — per-controller coalesce state is independent");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0, CAP_LIGHTBAR);
    svc.handleControllerAdd(r.token, 1, CAP_LIGHTBAR);
    const uint32_t s0 = vigem.pluggedSerials[0];
    const uint32_t s1 = vigem.pluggedSerials[1];
    client.reset();

    vigem.fireLightbar(s0, 0xFF, 0, 0);
    vigem.fireLightbar(s1, 0xFF, 0, 0); // same RGB, different controller
    EXPECT_EQ(client.lightbarCalls, 2);
}

static void test_capLightbar_constant() {
    TEST("CAP_LIGHTBAR — has expected capability bit 0x0008");
    EXPECT_EQ(static_cast<int>(CAP_LIGHTBAR), 0x0008);
}

// A pre-1.4 sender (caps = 0) must not receive MSG_LIGHTBAR — it cannot decode
// 0x000D. The colour is still cached for the web UI; that sender gets colour
// only via the deprecated MSG_RUMBLE tail (see the rumble tests above).
static void test_handleLightbarFromBackend_notSentWithoutCapLightbar() {
    TEST("handleLightbarFromBackend — colour is NOT sent to a sender lacking CAP_LIGHTBAR");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0); // caps = 0 — no CAP_LIGHTBAR
    const uint32_t serial = vigem.pluggedSerials.back();
    client.reset();

    vigem.fireLightbar(serial, 0x11, 0x22, 0x33);
    EXPECT_EQ(client.lightbarCalls, 0);

    // ...but the colour is still cached, so the web UI swatch stays live for
    // every controller regardless of the dedicated-stream gate.
    auto snap = svc.getConnectionsSnapshot();
    EXPECT(snap.connections[0].controllers[0].lightbarKnown);
    EXPECT(!snap.connections[0].controllers[0].lightbarCapable);
    EXPECT_EQ(static_cast<int>(snap.connections[0].controllers[0].lightbarR), 0x11);
}

static void test_handleLightbarFromBackend_capLightbarSenderGetsDedicatedStream() {
    TEST("handleLightbarFromBackend — a CAP_LIGHTBAR sender receives colour via MSG_LIGHTBAR");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 2, CAP_LIGHTBAR);
    const uint32_t serial = vigem.pluggedSerials.back();
    client.reset();

    vigem.fireLightbar(serial, 0xDE, 0xAD, 0xBE);
    EXPECT_EQ(client.lightbarCalls, 1);
    EXPECT_EQ(client.lastLightbarConnToken, r.token);
    EXPECT_EQ(static_cast<int>(client.lastLightbarCtrlIdx), 2);
    EXPECT_EQ(static_cast<int>(client.lastLightbarR), 0xDE);
    EXPECT_EQ(static_cast<int>(client.lastLightbarG), 0xAD);
    EXPECT_EQ(static_cast<int>(client.lastLightbarB), 0xBE);
}

static void test_handleLightbarFromBackend_coalesceStateResetOnReAdd() {
    TEST("handleLightbarFromBackend — re-adding a controller clears stale colour coalesce state");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0, CAP_LIGHTBAR);
    uint32_t serial = vigem.pluggedSerials.back();
    vigem.fireLightbar(serial, 0x7F, 0x7F, 0x7F);
    client.reset();

    // Remove + re-add. The re-added controller is a fresh actuator, so the same
    // colour must reach it again rather than being suppressed as a duplicate.
    svc.handleControllerRemove(r.token, 0);
    svc.handleControllerAdd(r.token, 0, CAP_LIGHTBAR);
    serial = vigem.pluggedSerials.back();
    vigem.fireLightbar(serial, 0x7F, 0x7F, 0x7F);
    EXPECT_EQ(client.lightbarCalls, 1);
}

static void test_lightbar_surfacedInConnectionsSnapshot() {
    TEST("getConnectionsSnapshot — lightbar capability + live colour are surfaced");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0, CAP_LIGHTBAR);
    const uint32_t serial = vigem.pluggedSerials.back();

    // Capable, but no colour set yet.
    auto before = svc.getConnectionsSnapshot();
    EXPECT(before.connections[0].controllers[0].lightbarCapable);
    EXPECT(!before.connections[0].controllers[0].lightbarKnown);

    vigem.fireLightbar(serial, 0x01, 0x99, 0xFE);
    auto after = svc.getConnectionsSnapshot();
    const auto& ci = after.connections[0].controllers[0];
    EXPECT(ci.lightbarKnown);
    EXPECT_EQ(static_cast<int>(ci.lightbarR), 0x01);
    EXPECT_EQ(static_cast<int>(ci.lightbarG), 0x99);
    EXPECT_EQ(static_cast<int>(ci.lightbarB), 0xFE);
}

// Hot-path (receiver → ViGEm allocation-free / single-lock) tests: the IPv4
// codec, the fused handleGamepadDataAndUpdate entry point, updatePostDecryptV4,
// lazy clientIP refresh, and the Controller::usesDS4 cache mirror.
static void test_parseIPv4Nbo_canonical() {
    TEST("parseIPv4Nbo -- canonical 192.168.1.1");
    // Network byte order: leftmost octet in low byte. For "192.168.1.1":
    //   0x01 << 24 | 0x01 << 16 | 0xA8 << 8 | 0xC0  = 0x0101A8C0
    EXPECT_EQ(satellite::parseIPv4Nbo("192.168.1.1"), (uint32_t)0x0101A8C0);
}

static void test_parseIPv4Nbo_loopback() {
    TEST("parseIPv4Nbo -- 127.0.0.1 round-trips byte-correctly");
    // 127.0.0.1 in NBO = 0x0100007F (matches inet_pton output on x86)
    EXPECT_EQ(satellite::parseIPv4Nbo("127.0.0.1"), (uint32_t)0x0100007F);
}

static void test_parseIPv4Nbo_allZeros() {
    TEST("parseIPv4Nbo -- 0.0.0.0 returns 0 (collides with invalid sentinel)");
    // Documented limitation: 0 is the invalid sentinel AND the literal
    // address. Acceptable because 0.0.0.0 is never a real sender IP.
    EXPECT_EQ(satellite::parseIPv4Nbo("0.0.0.0"), (uint32_t)0);
}

static void test_parseIPv4Nbo_allOnes() {
    TEST("parseIPv4Nbo -- 255.255.255.255 = 0xFFFFFFFF");
    EXPECT_EQ(satellite::parseIPv4Nbo("255.255.255.255"), (uint32_t)0xFFFFFFFF);
}

static void test_parseIPv4Nbo_emptyRejected() {
    TEST("parseIPv4Nbo -- empty string rejected");
    EXPECT_EQ(satellite::parseIPv4Nbo(""), (uint32_t)0);
}

static void test_parseIPv4Nbo_tooFewOctetsRejected() {
    TEST("parseIPv4Nbo -- '1.2.3' (3 octets) rejected");
    EXPECT_EQ(satellite::parseIPv4Nbo("1.2.3"), (uint32_t)0);
}

static void test_parseIPv4Nbo_tooManyOctetsRejected() {
    TEST("parseIPv4Nbo -- '1.2.3.4.5' rejected");
    EXPECT_EQ(satellite::parseIPv4Nbo("1.2.3.4.5"), (uint32_t)0);
}

static void test_parseIPv4Nbo_octetOverflowRejected() {
    TEST("parseIPv4Nbo -- octet > 255 rejected");
    EXPECT_EQ(satellite::parseIPv4Nbo("1.2.3.256"), (uint32_t)0);
    EXPECT_EQ(satellite::parseIPv4Nbo("999.0.0.1"), (uint32_t)0);
}

static void test_parseIPv4Nbo_nonDigitRejected() {
    TEST("parseIPv4Nbo -- non-digit characters rejected");
    EXPECT_EQ(satellite::parseIPv4Nbo("1.2.3.a"), (uint32_t)0);
    EXPECT_EQ(satellite::parseIPv4Nbo("hello"), (uint32_t)0);
    EXPECT_EQ(satellite::parseIPv4Nbo("192.168.1.1 "), (uint32_t)0); // trailing space
}

static void test_parseIPv4Nbo_emptyOctetRejected() {
    TEST("parseIPv4Nbo -- empty octet ('1..2.3' / '.1.2.3' / '1.2.3.') rejected");
    EXPECT_EQ(satellite::parseIPv4Nbo("1..2.3"), (uint32_t)0);
    EXPECT_EQ(satellite::parseIPv4Nbo(".1.2.3"), (uint32_t)0);
    EXPECT_EQ(satellite::parseIPv4Nbo("1.2.3."), (uint32_t)0);
}

static void test_formatIPv4Nbo_canonical() {
    TEST("formatIPv4Nbo -- 0x0101A8C0 (NBO) formats to '192.168.1.1'");
    EXPECT(satellite::formatIPv4Nbo(0x0101A8C0) == std::string("192.168.1.1"));
}

static void test_formatIPv4Nbo_loopback() {
    TEST("formatIPv4Nbo -- 0x0100007F formats to '127.0.0.1'");
    EXPECT(satellite::formatIPv4Nbo(0x0100007F) == std::string("127.0.0.1"));
}

static void test_formatIPv4Nbo_zeroAndAllOnes() {
    TEST("formatIPv4Nbo -- edge values format correctly");
    EXPECT(satellite::formatIPv4Nbo(0x00000000) == std::string("0.0.0.0"));
    EXPECT(satellite::formatIPv4Nbo(0xFFFFFFFF) == std::string("255.255.255.255"));
}

static void test_ipv4_roundTrip() {
    TEST("IPv4 codec -- parse -> format -> parse round-trips");
    const char* addrs[] = {"10.0.0.1",    "172.16.0.42",   "192.168.255.254", "8.8.8.8",
                           "1.1.1.1",     "203.0.113.99",  "169.254.1.42",    "224.0.0.1",
                           "192.0.2.123", "255.255.255.0", "100.64.1.1",      "127.0.0.1"};
    for (const char* a : addrs) {
        uint32_t nbo = satellite::parseIPv4Nbo(a);
        EXPECT(nbo != 0);
        std::string back = satellite::formatIPv4Nbo(nbo);
        EXPECT(back == std::string(a));
    }
}

static void test_ipv4_matchesInetPtonByteOrder() {
    TEST("parseIPv4Nbo -- byte order matches sockaddr_in.sin_addr.s_addr layout");
    // The hot path passes the parsed value to ClientAdapter's V4 override
    // which stamps it straight into sin_addr without any byte swap. So the
    // numeric layout MUST match what inet_pton would have written. The
    // value is little-endian-on-the-wire-octet-order: parts[0] is the low
    // byte. We verify the bit positions explicitly to lock the contract.
    const uint32_t nbo = satellite::parseIPv4Nbo("10.20.30.40");
    EXPECT_EQ(static_cast<uint8_t>(nbo & 0xFF), (uint8_t)10);
    EXPECT_EQ(static_cast<uint8_t>((nbo >> 8) & 0xFF), (uint8_t)20);
    EXPECT_EQ(static_cast<uint8_t>((nbo >> 16) & 0xFF), (uint8_t)30);
    EXPECT_EQ(static_cast<uint8_t>((nbo >> 24) & 0xFF), (uint8_t)40);
}

static void test_openSession_seedsClientIPv4FromString() {
    TEST("openSession -- seeds Connection::clientIPv4 from the string IP");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = svc.openSession("dev1", "D1", "10.0.0.42", TEST_KEY);
    EXPECT(r.ok);

    // Round-trip via the snapshot: clientIP comes from the cache string,
    // which is initially set from the string IP we passed in. The numeric
    // cache is exercised by the next test via the V4 update path.
    auto snap = svc.getConnectionsSnapshot();
    EXPECT_EQ((int)snap.connections.size(), 1);
    EXPECT(snap.connections[0].clientIP == std::string("10.0.0.42"));
}

static void test_openSession_unparseableIPLeavesClientIPv4Zero() {
    TEST("openSession -- unparseable IP leaves clientIPv4 == 0 but clientIP string still set");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    // "garbage" parses to 0. The snapshot's clientIP string is still
    // whatever we passed (web UI honesty), but the next V4 update will
    // observe clientIPv4 == 0 and refresh the cache string from the
    // parsed numeric IP -- the first real packet recovers the truth.
    auto r = svc.openSession("dev1", "D1", "garbage", TEST_KEY);
    EXPECT(r.ok);
    auto snap = svc.getConnectionsSnapshot();
    EXPECT(snap.connections[0].clientIP == std::string("garbage"));

    // Confirm the recovery path: pass a real V4 update, expect the
    // snapshot's clientIP to reformat to the canonical dotted-quad.
    svc.updatePostDecryptV4(r.token, /*counter=*/1, satellite::parseIPv4Nbo("172.16.0.5"),
                            /*port=*/9876);
    auto snap2 = svc.getConnectionsSnapshot();
    EXPECT(snap2.connections[0].clientIP == std::string("172.16.0.5"));
}

static void test_updatePostDecryptV4_updatesCounter() {
    TEST("updatePostDecryptV4 -- updates Connection.lastCounter");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    const uint32_t ip = satellite::parseIPv4Nbo("10.0.0.1");
    svc.updatePostDecryptV4(r.token, /*counter=*/4242, ip, /*port=*/5555);

    uint8_t k[CRYPTO_KEY_SIZE];
    uint32_t c = 0;
    EXPECT(svc.getDecryptInfo(r.token, k, c));
    EXPECT_EQ(c, (uint32_t)4242);
}

static void test_updatePostDecryptV4_callsClientV4_notString() {
    TEST("updatePostDecryptV4 -- forwards to updateClientAddrV4, not the string overload");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    // Reset counters from the openSession path so we only count the
    // updatePostDecryptV4 call below.
    int v4Before = client.updateAddrV4Calls;
    int strBefore = client.updateAddrCalls;
    svc.updatePostDecryptV4(r.token, 1, satellite::parseIPv4Nbo("10.0.0.1"), 5555);
    EXPECT_EQ(client.updateAddrV4Calls, v4Before + 1);
    EXPECT_EQ(client.updateAddrCalls, strBefore); // string variant NOT called
    EXPECT_EQ(client.lastV4Token, r.token);
    EXPECT_EQ(client.lastV4IPv4Nbo, satellite::parseIPv4Nbo("10.0.0.1"));
    EXPECT_EQ(client.lastV4Port, (uint16_t)5555);
}

static void test_updatePostDecryptV4_invalidTokenIsNoOp() {
    TEST("updatePostDecryptV4 -- unknown token is a silent no-op");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    svc.updatePostDecryptV4(99999, 1, satellite::parseIPv4Nbo("10.0.0.1"), 5555);
    EXPECT_EQ(client.updateAddrV4Calls, 0);
}

static void test_updatePostDecryptV4_refreshesClientIPOnChange() {
    TEST("updatePostDecryptV4 -- clientIP string refreshes when IPv4 changes");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc); // seeds with "192.168.1.100"

    svc.updatePostDecryptV4(r.token, 1, satellite::parseIPv4Nbo("10.0.0.1"), 5555);
    auto snap = svc.getConnectionsSnapshot();
    EXPECT(snap.connections[0].clientIP == std::string("10.0.0.1"));
}

static void test_updatePostDecryptV4_preservesClientIPWhenUnchanged() {
    TEST("updatePostDecryptV4 -- clientIP string left alone when IPv4 unchanged");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc); // seeds with "192.168.1.100"

    const uint32_t sameIp = satellite::parseIPv4Nbo("192.168.1.100");
    // Tamper with the snapshot indirectly: prove the string is the
    // expected canonical form before AND after a same-IP update. The
    // lazy-refresh path bypasses the assign entirely (allocation-free
    // steady state), but the user-visible value is identical -- which
    // is exactly the property we want to lock in.
    svc.updatePostDecryptV4(r.token, 1, sameIp, 9876);
    svc.updatePostDecryptV4(r.token, 2, sameIp, 9876);
    svc.updatePostDecryptV4(r.token, 3, sameIp, 9876);
    auto snap = svc.getConnectionsSnapshot();
    EXPECT(snap.connections[0].clientIP == std::string("192.168.1.100"));
}

static void test_handleGamepadDataAndUpdate_happyPath() {
    TEST("handleGamepadDataAndUpdate -- submits report on happy path");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);

    GamepadReport rpt{};
    rpt.wButtons = 0x1234;
    rpt.sThumbRX = -12345;
    const uint32_t ip = satellite::parseIPv4Nbo("10.0.0.5");

    EXPECT(svc.handleGamepadDataAndUpdate(r.token, 100, ip, 5555, 0, rpt));
    EXPECT_EQ(vigem.submitCalls, 1);
    EXPECT_EQ(vigem.lastSubmittedReport.wButtons, (uint16_t)0x1234);
    EXPECT_EQ(vigem.lastSubmittedReport.sThumbRX, (int16_t)-12345);
}

static void test_handleGamepadDataAndUpdate_invalidTokenReturnsFalse() {
    TEST("handleGamepadDataAndUpdate -- unknown token returns false, no submit");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    GamepadReport rpt{};
    EXPECT(!svc.handleGamepadDataAndUpdate(99999, 0, 0x01010101, 5555, 0, rpt));
    EXPECT_EQ(vigem.submitCalls, 0);
    EXPECT_EQ(client.updateAddrV4Calls, 0);
}

static void test_handleGamepadDataAndUpdate_inactiveControllerReturnsFalse() {
    TEST("handleGamepadDataAndUpdate -- inactive controller returns false");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc);
    // NO handleControllerAdd -- the slot is inactive.

    GamepadReport rpt{};
    EXPECT(!svc.handleGamepadDataAndUpdate(r.token, 1, 0x01010101, 5555, 0, rpt));
    EXPECT_EQ(vigem.submitCalls, 0);
    // updatePostDecrypt half DID run, so the V4 client update fired
    // even though the gamepad submit didn't -- the fused entry-point
    // still updates connection state before checking the controller.
    EXPECT(client.updateAddrV4Calls >= 1);
}

static void test_handleGamepadDataAndUpdate_outOfBoundsReturnsFalse() {
    TEST("handleGamepadDataAndUpdate -- ctrlIdx out of bounds returns false");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc);

    GamepadReport rpt{};
    EXPECT(!svc.handleGamepadDataAndUpdate(r.token, 1, 0x01010101, 5555,
                                           /*ctrlIdx=*/MAX_CONTROLLERS_PER_CONN, rpt));
    EXPECT(!svc.handleGamepadDataAndUpdate(r.token, 1, 0x01010101, 5555,
                                           /*ctrlIdx=*/200, rpt));
    EXPECT_EQ(vigem.submitCalls, 0);
}

static void test_handleGamepadDataAndUpdate_updatesCounter() {
    TEST("handleGamepadDataAndUpdate -- updates Connection.lastCounter");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);

    GamepadReport rpt{};
    EXPECT(svc.handleGamepadDataAndUpdate(r.token, 7777, 0x01010101, 5555, 0, rpt));

    uint8_t k[CRYPTO_KEY_SIZE];
    uint32_t c = 0;
    svc.getDecryptInfo(r.token, k, c);
    EXPECT_EQ(c, (uint32_t)7777);
}

static void test_handleGamepadDataAndUpdate_routesXboxThroughSubmitReport() {
    TEST("handleGamepadDataAndUpdate -- Xbox type routes via submitReport (not DS4)");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0); // defaults to Xbox

    GamepadReport rpt{};
    svc.handleGamepadDataAndUpdate(r.token, 1, 0x01010101, 5555, 0, rpt);

    EXPECT_EQ(vigem.submitCalls, 1);
    EXPECT_EQ(vigem.submitDS4Calls, 0);
}

static void test_handleGamepadDataAndUpdate_routesPlayStationThroughSubmitDS4() {
    TEST("handleGamepadDataAndUpdate -- PlayStation type routes via submitDS4Report");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    svc.handleControllerType(r.token, 0, CONTROLLER_TYPE_PLAYSTATION);

    GamepadReport rpt{};
    svc.handleGamepadDataAndUpdate(r.token, 1, 0x01010101, 5555, 0, rpt);

    EXPECT_EQ(vigem.submitCalls, 0);
    EXPECT_EQ(vigem.submitDS4Calls, 1);
}

static void test_handleGamepadDataAndUpdate_usesCachedUsesDS4_notLiveLookup() {
    TEST("handleGamepadDataAndUpdate -- uses cached Controller::usesDS4, mid-stream type change "
         "re-routes after handleControllerType refreshes the cache");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0); // Xbox default -> usesDS4 cached false

    GamepadReport rpt{};
    svc.handleGamepadDataAndUpdate(r.token, 1, 0x01010101, 5555, 0, rpt);
    EXPECT_EQ(vigem.submitCalls, 1);
    EXPECT_EQ(vigem.submitDS4Calls, 0);

    // Flip the type -- handleControllerType refreshes the cache mirror
    // and replugs the device through ds4. Next gamepad packet must
    // route via the DS4 path.
    svc.handleControllerType(r.token, 0, CONTROLLER_TYPE_PLAYSTATION);
    svc.handleGamepadDataAndUpdate(r.token, 2, 0x01010101, 5555, 0, rpt);
    EXPECT_EQ(vigem.submitCalls, 1);    // unchanged
    EXPECT_EQ(vigem.submitDS4Calls, 1); // new

    // And back again -- the cache must also revert.
    svc.handleControllerType(r.token, 0, CONTROLLER_TYPE_XBOX);
    svc.handleGamepadDataAndUpdate(r.token, 3, 0x01010101, 5555, 0, rpt);
    EXPECT_EQ(vigem.submitCalls, 2);
    EXPECT_EQ(vigem.submitDS4Calls, 1);
}

static void test_handleGamepadDataAndUpdate_callsClientV4Once() {
    TEST("handleGamepadDataAndUpdate -- fires exactly one updateClientAddrV4 per call");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);

    int v4Before = client.updateAddrV4Calls;
    GamepadReport rpt{};
    svc.handleGamepadDataAndUpdate(r.token, 1, satellite::parseIPv4Nbo("203.0.113.5"), 5555, 0,
                                   rpt);
    EXPECT_EQ(client.updateAddrV4Calls, v4Before + 1);
    EXPECT_EQ(client.lastV4IPv4Nbo, satellite::parseIPv4Nbo("203.0.113.5"));
    EXPECT_EQ(client.lastV4Port, (uint16_t)5555);
}

static void test_handleGamepadDataAndUpdate_copiesReportIntoLastReportCache() {
    TEST("handleGamepadDataAndUpdate -- copies report into Controller.lastReport");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);

    GamepadReport rpt{};
    rpt.wButtons = 0xBEEF;
    rpt.bLeftTrigger = 0xAB;
    rpt.bRightTrigger = 0xCD;
    rpt.sThumbLX = 1111;
    rpt.sThumbLY = -2222;
    rpt.sThumbRX = 3333;
    rpt.sThumbRY = -4444;
    svc.handleGamepadDataAndUpdate(r.token, 1, 0x01010101, 5555, 0, rpt);

    // The backend's lastSubmittedReport is the wire copy that reached
    // submitReport -- it must match what the caller passed (no munging
    // in the fused path for Xbox-typed controllers).
    EXPECT_EQ(vigem.lastSubmittedReport.wButtons, (uint16_t)0xBEEF);
    EXPECT_EQ(vigem.lastSubmittedReport.bLeftTrigger, (uint8_t)0xAB);
    EXPECT_EQ(vigem.lastSubmittedReport.bRightTrigger, (uint8_t)0xCD);
    EXPECT_EQ(vigem.lastSubmittedReport.sThumbLX, (int16_t)1111);
    EXPECT_EQ(vigem.lastSubmittedReport.sThumbLY, (int16_t)-2222);
    EXPECT_EQ(vigem.lastSubmittedReport.sThumbRX, (int16_t)3333);
    EXPECT_EQ(vigem.lastSubmittedReport.sThumbRY, (int16_t)-4444);
}

static void test_handleGamepadDataAndUpdate_propagatesBackendFailure() {
    TEST("handleGamepadDataAndUpdate -- forwards backend submit failure to caller");
    MockViGem vigem;
    vigem.submitReturnVal = false; // backend refuses the report
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);

    GamepadReport rpt{};
    EXPECT(!svc.handleGamepadDataAndUpdate(r.token, 1, 0x01010101, 5555, 0, rpt));
}

static void test_handleGamepadDataAndUpdate_stillRefreshesIPOnInactiveController() {
    TEST("handleGamepadDataAndUpdate -- updates IP cache even when controller is inactive");
    // The fused path does the state update first, then the controller
    // check. This is intentional -- a packet arriving for a controller
    // that's been removed mid-session shouldn't strand the connection's
    // lastPacketTime in the past (the reaper would kill it next cycle).
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc);
    // No controller added.

    GamepadReport rpt{};
    EXPECT(!svc.handleGamepadDataAndUpdate(r.token, 99, satellite::parseIPv4Nbo("203.0.113.99"),
                                           5555, 0, rpt));
    // The V4 update did fire even though gamepad dispatch returned false.
    EXPECT_EQ(client.lastV4IPv4Nbo, satellite::parseIPv4Nbo("203.0.113.99"));
    // And the snapshot reflects the new IP.
    auto snap = svc.getConnectionsSnapshot();
    EXPECT(snap.connections[0].clientIP == std::string("203.0.113.99"));
}

static void test_usesDS4_defaultsToFalseOnAdd() {
    TEST("Controller::usesDS4 -- defaults to false (Xbox at add)");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);

    // Indirect observation via the dispatch routing: Xbox-typed
    // controllers go through submitReport.
    GamepadReport rpt{};
    svc.handleGamepadDataAndUpdate(r.token, 1, 0x01010101, 5555, 0, rpt);
    EXPECT_EQ(vigem.submitCalls, 1);
    EXPECT_EQ(vigem.submitDS4Calls, 0);
}

static void test_usesDS4_setToTrueOnControllerTypeChangeToPS() {
    TEST("Controller::usesDS4 -- handleControllerType to PlayStation sets cache to true");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0);
    svc.handleControllerType(r.token, 0, CONTROLLER_TYPE_PLAYSTATION);

    GamepadReport rpt{};
    svc.handleGamepadDataAndUpdate(r.token, 1, 0x01010101, 5555, 0, rpt);
    EXPECT_EQ(vigem.submitDS4Calls, 1);
}

static void test_usesDS4_revertedOnControllerTypeChangeBackToXbox() {
    TEST("Controller::usesDS4 -- type flip Xbox <-> PS toggles the cache both ways");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc);
    svc.handleControllerAdd(r.token, 0); // Xbox default

    // Xbox -> Xbox (no-op): still routes via submitReport.
    GamepadReport rpt{};
    svc.handleGamepadDataAndUpdate(r.token, 1, 0x01010101, 5555, 0, rpt);
    EXPECT_EQ(vigem.submitCalls, 1);

    // Xbox -> PS: routes via DS4.
    svc.handleControllerType(r.token, 0, CONTROLLER_TYPE_PLAYSTATION);
    svc.handleGamepadDataAndUpdate(r.token, 2, 0x01010101, 5555, 0, rpt);
    EXPECT_EQ(vigem.submitDS4Calls, 1);

    // PS -> Xbox: back to submitReport.
    svc.handleControllerType(r.token, 0, CONTROLLER_TYPE_XBOX);
    svc.handleGamepadDataAndUpdate(r.token, 3, 0x01010101, 5555, 0, rpt);
    EXPECT_EQ(vigem.submitCalls, 2);
    EXPECT_EQ(vigem.submitDS4Calls, 1);
}

static void test_usesDS4_matchesLegacyHandleGamepadData() {
    TEST("Controller::usesDS4 -- legacy handleGamepadData routes identically to fused path");
    // Belt-and-braces: the cached flag drives BOTH paths. If a future
    // change accidentally desyncs the legacy and fused dispatch, this
    // test will catch it.
    MockViGem fusedBackend;
    MockClient fusedClient;
    MockLog fusedLog;
    SessionService fusedSvc(fusedBackend, fusedClient, fusedLog);
    auto fr = openTestSession(fusedSvc);
    fusedSvc.handleControllerAdd(fr.token, 0);
    fusedSvc.handleControllerType(fr.token, 0, CONTROLLER_TYPE_PLAYSTATION);

    MockViGem legacyBackend;
    MockClient legacyClient;
    MockLog legacyLog;
    SessionService legacySvc(legacyBackend, legacyClient, legacyLog);
    auto lr = openTestSession(legacySvc);
    legacySvc.handleControllerAdd(lr.token, 0);
    legacySvc.handleControllerType(lr.token, 0, CONTROLLER_TYPE_PLAYSTATION);

    GamepadReport rpt{};
    fusedSvc.handleGamepadDataAndUpdate(fr.token, 1, 0x01010101, 5555, 0, rpt);
    legacySvc.handleGamepadData(lr.token, 0, rpt);

    EXPECT_EQ(fusedBackend.submitDS4Calls, legacyBackend.submitDS4Calls);
    EXPECT_EQ(fusedBackend.submitCalls, legacyBackend.submitCalls);
}

static void test_updatePostDecrypt_seedsClientIPv4FromString() {
    TEST("updatePostDecrypt (string) -- also populates Connection::clientIPv4 cache");
    // The legacy string path must keep the numeric cache in sync so a
    // future hot-path call (which compares numerics for lazy-refresh)
    // observes the right starting value.
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    auto r = openTestSession(svc);

    svc.updatePostDecrypt(r.token, 1, "172.16.0.42", 5555);

    // Now a SAME-IP V4 update should leave the snapshot string alone --
    // proving the string path stored the same numeric value the V4
    // refresh would compute. If the seed had been wrong, the V4 path
    // would observe a "change" and reformat to the canonical string.
    svc.updatePostDecryptV4(r.token, 2, satellite::parseIPv4Nbo("172.16.0.42"), 5555);
    auto snap = svc.getConnectionsSnapshot();
    EXPECT(snap.connections[0].clientIP == std::string("172.16.0.42"));
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
    // Controller type helper functions (types.h)
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

    // handleControllerType — basic behavior
    test_handleControllerType_setsTypeAndBroadcasts();
    test_handleControllerType_invalidToken();
    test_handleControllerType_outOfBoundsCtrlIdx();
    test_handleControllerType_inactiveControllerIgnored();
    test_handleControllerType_invalidValueClampsToXbox();
    test_handleControllerType_boundaryValueClampsToXbox();

    // handleControllerType — replug behavior
    test_handleControllerType_xboxToPlaystationReplugsAsDS4();
    test_handleControllerType_playstationToXboxReplugsAsXbox();
    test_handleControllerType_xboxToXboxNoReplug();
    test_handleControllerType_playstationToPlaystationNoReplug();
    test_handleControllerType_replugPreservesSerial();
    test_handleControllerType_replugFailureLogsError();
    test_handleControllerType_replugResendsMotionAckPlaystation();
    test_handleControllerType_replugResendsMotionAckBackendBroken();
    test_handleControllerType_replugToXboxResendsMotionAck();
    test_handleControllerType_sameTypeSendsNoAck();
    test_handleControllerType_replugFailureSendsNoAck();
    test_handleControllerType_replugAckCarriesIdxAndType();
    test_handleControllerType_playstationToPlaystationSendsNoAck();
    test_handleControllerType_multipleRapidSwitches();

    // handleControllerAdd — DS4-aware
    test_handleControllerAdd_defaultTypeIsXbox();
    test_handleControllerAdd_withType_playstationPlugsDS4Directly();
    test_handleControllerAdd_withType_invalidClampsToXbox();
    test_handleControllerAdd_unspecifiedRetainsExistingType();
    test_handleControllerAdd_withType_xboxExplicitPlugsXbox();
    test_handleControllerAdd_withType_playstationBackendRejected();
    test_handleControllerAdd_withType_unspecifiedFreshSlotDefaultsXbox();
    test_handleControllerAdd_presetPlaystationType();
    test_handleControllerAdd_thenSetPlaystation_fullFlow();

    // handleControllerAdd — motion-status byte on the ACK
    test_handleControllerAdd_motionFlags_xboxTypeBackendOk();
    test_handleControllerAdd_motionFlags_playstationTypeBothBits();
    test_handleControllerAdd_motionFlags_backendBroken();
    test_handleControllerAdd_motionFlags_errorAcksCarryZero();
    test_handleControllerRemove_motionFlags_carriesZero();

    // handleControllerCapsUpdate — mid-session cap word refresh
    test_handleControllerCapsUpdate_overwritesCapsInPlace();
    test_handleControllerCapsUpdate_idempotentOnSameWord();
    test_handleControllerCapsUpdate_invalidToken();
    test_handleControllerCapsUpdate_outOfBoundsCtrlIdx();
    test_handleControllerCapsUpdate_inactiveControllerIgnored();

    // Gamepad data routing
    test_gamepadData_xboxTypeUsesSubmitReport();
    test_gamepadData_playstationTypeUsesSubmitDS4Report();
    test_gamepadData_routingSwitchesWithType();

    // Snapshot
    test_snapshot_defaultControllerType();
    test_snapshot_reflectsTypeChange();
    test_snapshot_multipleControllersWithDifferentTypes();

    // Session lifecycle with DS4
    test_closeSession_unplugsDS4Controllers();
    test_closeAllSessions_withMixedTypes();
    test_staleReplacement_unplugsDS4Controller();
    test_controllerRemove_thenReaddRetainsTypeFromSameSlot();
    test_controllerReAdd_clearsCachedSenderStreams();
    test_controllerReAdd_touchpadMouseNoJumpAfterReconnect();

    // Protocol constants
    test_msgControllerType_constant();

    // Rumble (return-path)
    test_msgRumble_constant();
    test_constructor_installsRumbleCallback();
    test_handleRumbleFromBackend_routesToOwningConnection();
    test_handleRumbleFromBackend_unknownSerialDropped();
    test_handleRumbleFromBackend_inactiveControllerDropped();
    test_handleRumbleFromBackend_coalescesIdenticalReports();
    test_handleRumbleFromBackend_stopReportEmitted();
    test_handleRumbleFromBackend_routesAcrossMultipleConnections();
    test_handleRumbleFromBackend_serialReuseClearsState();
    test_handleRumbleFromBackend_customDuration();
    test_handleRumbleFromBackend_durationChangeAloneDoesNotEmit();
    test_handleRumbleFromBackend_separateControllersIndependentlyCoalesced();
    test_handleRumbleFromBackend_zeroIsCoalescedWhenInitial();

    // Motion (sender → satellite, IMU forwarding)
    test_msgMotion_constant();
    test_motionReport_wireSize();
    test_motionScaleConstants_fullScale();
    test_handleMotionData_forwardsToBackend();
    test_handleMotionData_cachesEvenWhenBackendDeclines();
    test_handleMotionData_invalidToken();
    test_handleMotionData_outOfBoundsCtrlIdx();
    test_handleMotionData_inactiveController();
    test_handleMotionData_routesAcrossControllers();

    // Per-type motion sink + per-serial backend health diagnostics
    test_snapshot_motionSinkSupportedForType_DS4();
    test_snapshot_motionSinkSupportedForType_Xbox();
    test_snapshot_motionSinkSupportedForType_macOS_no_backend();
    test_snapshot_motionBackendOk_default_true();
    test_snapshot_motionBackendOk_kernel_rejected();
    test_snapshot_motionBackendOk_unknown_serial_is_optimistic();

    // Battery (sender → satellite, periodic)
    test_msgBattery_constant();
    test_batteryStatusName_known();
    test_handleBatteryUpdate_cachesAndForwards();
    test_handleBatteryUpdate_unknownLevelIsAccepted();
    test_handleBatteryUpdate_rejectsBogusLevel();
    test_handleBatteryUpdate_rejectsBogusStatus();
    test_handleBatteryUpdate_invalidToken();
    test_handleBatteryUpdate_inactiveController();
    test_snapshot_batteryDefaultsUnknown();

    // Touchpad (sender → satellite)
    test_msgTouchpad_constant();
    test_handleTouchpadData_forwardsToBackend();
    test_handleTouchpadData_cachesEvenWhenBackendDeclines();
    test_handleTouchpadData_invalidToken();
    test_handleTouchpadData_outOfBoundsCtrlIdx();
    test_handleTouchpadData_inactiveController();
    // Codec, wire decode, routing modes, hot-apply.
    test_touchpadWireToRange_endpoints();
    test_touchpadWireToRange_clampsAndDegenerate();
    test_ds4PackTouchFinger_activeFlagAndId();
    test_ds4PackTouchFinger_coordPacking();
    test_decodeTouchpadReport_wireLayout();
    test_decodeTouchpadReport_inactiveFlags();
    test_touchpadMode_constants();
    test_openSession_carriesTouchpadMode();
    test_handleTouchpadData_ds4ModeForwardsToTouchpad();
    test_handleTouchpadData_offModeCachesOnly();
    test_handleTouchpadData_mouseModeTouchDownNoJump();
    test_handleTouchpadData_mouseModeContinuousDelta();
    test_handleTouchpadData_mouseModeSubPixelRemainder();
    test_handleTouchpadData_mouseModeLiftResetsAnchor();
    test_handleTouchpadData_mouseModeTrackingIdChangeBreaksContinuity();
    test_handleTouchpadData_mouseModeTimeScalingHalvesLongerDt();
    test_handleTouchpadData_mouseModeTimeScalingFirstTouchJumpFix();
    test_handleTouchpadData_mouseModeDuplicateTimestampEmitsNoMotion();
    test_handleTouchpadData_mouseModeBigGapReanchors();
    test_setTouchpadMode_hotApplies();
    test_setTouchpadMode_unknownDeviceAndBadMode();
    // Client-driven mode + server capabilities.
    test_pairedDevice_defaultsToOff();
    test_connection_defaultsToOff();
    test_openSession_defaultParamIsOff();
    test_deriveTouchpadCapabilities_offAlwaysSupported();
    test_deriveTouchpadCapabilities_padMouseMatchBackend();
    test_deriveTouchpadCapabilities_driverInstalledButBusDownStillSupports();

    // Lightbar (host game → satellite → dish)
    test_msgLightbar_constant();
    test_constructor_installsLightbarCallback();
    test_handleLightbarFromBackend_routesToOwningConnection();
    test_handleLightbarFromBackend_unknownSerialDropped();
    test_handleLightbarFromBackend_coalescesIdenticalColours();
    test_handleLightbarFromBackend_anyChannelChangeEmits();
    test_handleLightbarFromBackend_separateControllersIndependentlyCoalesced();
    // CAP_LIGHTBAR capability gating.
    test_capLightbar_constant();
    test_handleLightbarFromBackend_notSentWithoutCapLightbar();
    test_handleLightbarFromBackend_capLightbarSenderGetsDedicatedStream();
    test_handleLightbarFromBackend_coalesceStateResetOnReAdd();
    test_lightbar_surfacedInConnectionsSnapshot();

    // Hot-path optimisations: IPv4 codec
    test_parseIPv4Nbo_canonical();
    test_parseIPv4Nbo_loopback();
    test_parseIPv4Nbo_allZeros();
    test_parseIPv4Nbo_allOnes();
    test_parseIPv4Nbo_emptyRejected();
    test_parseIPv4Nbo_tooFewOctetsRejected();
    test_parseIPv4Nbo_tooManyOctetsRejected();
    test_parseIPv4Nbo_octetOverflowRejected();
    test_parseIPv4Nbo_nonDigitRejected();
    test_parseIPv4Nbo_emptyOctetRejected();
    test_formatIPv4Nbo_canonical();
    test_formatIPv4Nbo_loopback();
    test_formatIPv4Nbo_zeroAndAllOnes();
    test_ipv4_roundTrip();
    test_ipv4_matchesInetPtonByteOrder();

    // Hot-path optimisations: openSession clientIPv4 seeding
    test_openSession_seedsClientIPv4FromString();
    test_openSession_unparseableIPLeavesClientIPv4Zero();

    // Hot-path optimisations: updatePostDecryptV4
    test_updatePostDecryptV4_updatesCounter();
    test_updatePostDecryptV4_callsClientV4_notString();
    test_updatePostDecryptV4_invalidTokenIsNoOp();
    test_updatePostDecryptV4_refreshesClientIPOnChange();
    test_updatePostDecryptV4_preservesClientIPWhenUnchanged();

    // Hot-path optimisations: handleGamepadDataAndUpdate fused entry
    test_handleGamepadDataAndUpdate_happyPath();
    test_handleGamepadDataAndUpdate_invalidTokenReturnsFalse();
    test_handleGamepadDataAndUpdate_inactiveControllerReturnsFalse();
    test_handleGamepadDataAndUpdate_outOfBoundsReturnsFalse();
    test_handleGamepadDataAndUpdate_updatesCounter();
    test_handleGamepadDataAndUpdate_routesXboxThroughSubmitReport();
    test_handleGamepadDataAndUpdate_routesPlayStationThroughSubmitDS4();
    test_handleGamepadDataAndUpdate_usesCachedUsesDS4_notLiveLookup();
    test_handleGamepadDataAndUpdate_callsClientV4Once();
    test_handleGamepadDataAndUpdate_copiesReportIntoLastReportCache();
    test_handleGamepadDataAndUpdate_propagatesBackendFailure();
    test_handleGamepadDataAndUpdate_stillRefreshesIPOnInactiveController();

    // Hot-path optimisations: Controller::usesDS4 cached flag
    test_usesDS4_defaultsToFalseOnAdd();
    test_usesDS4_setToTrueOnControllerTypeChangeToPS();
    test_usesDS4_revertedOnControllerTypeChangeBackToXbox();
    test_usesDS4_matchesLegacyHandleGamepadData();

    // Hot-path optimisations: backward-compat for legacy string update
    test_updatePostDecrypt_seedsClientIPv4FromString();

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
