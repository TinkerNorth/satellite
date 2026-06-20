// SPDX-License-Identifier: LGPL-3.0-or-later
#include "../src/net/inner_dispatch.h"
#include "../src/core/session_service.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

#include "test_util.h"

// Bare port stubs — tests assert against the call counters to confirm whether a
// message was accepted or rejected.
struct StubGamepad : IGamepadPort {
    bool ensureBusOpen() override { return true; }
    void closeBus() override {}
    bool isBusOpen() const override { return busOpen; }
    bool pluginDevice(uint32_t) override { return true; }
    bool pluginDeviceDS4(uint32_t) override { return true; }
    bool unplugDevice(uint32_t) override { return true; }
    bool submitReport(uint32_t, const GamepadReport&) override {
        gamepadCalls++;
        return submitReturnVal;
    }
    bool submitDS4Report(uint32_t, const GamepadReport&) override {
        gamepadCalls++;
        return submitReturnVal;
    }
    void setRumbleCallback(RumbleCallback) override {}
    bool submitMotion(uint32_t, const MotionReport& r) override {
        motionCalls++;
        lastMotion = r;
        return false;
    }
    bool submitBattery(uint32_t, const BatteryReport& r) override {
        batteryCalls++;
        lastBattery = r;
        return false;
    }
    bool submitTouchpad(uint32_t, const TouchpadReport& r) override {
        touchpadCalls++;
        lastTouchpad = r;
        return false;
    }
    bool submitRelativeMouse(int, int, bool) override { return false; }
    void setLightbarCallback(LightbarCallback) override {}

    bool busOpen = true;
    bool submitReturnVal = true;
    int gamepadCalls = 0;
    int motionCalls = 0;
    int batteryCalls = 0;
    int touchpadCalls = 0;
    MotionReport lastMotion{};
    BatteryReport lastBattery{};
    TouchpadReport lastTouchpad{};
};

struct StubClient : IClientPort {
    void updateClientAddr(uint32_t, const std::string&, uint16_t) override {}
    void removeClientAddr(uint32_t) override {}
    void sendHeartbeatAck(const Connection&, bool, uint8_t, uint16_t, uint16_t) override {
        heartbeatAcks++;
    }
    void sendSessionClose(const Connection&, uint8_t) override {}
    void sendRumble(const Connection&, uint8_t, const RumbleReport&) override {}
    void sendLightbar(const Connection&, uint8_t, uint8_t, uint8_t, uint8_t) override {}
    int heartbeatAcks = 0;
};

struct StubLog : ILogPort {
    void logMsg(LogLevel, const std::string&, const std::string&) override {}
};

// Open a session with one active controller at index 0. Touchpad mode rides
// the descriptor (DS4 so dispatch tests see pass-through routing — the OFF
// default would drop every MSG_TOUCHPAD before a backend call is observable).
static uint32_t openWithController(SessionService& svc,
                                   const std::string& devId = "dev-receiver-test") {
    uint8_t key[CRYPTO_KEY_SIZE] = {};
    ControllerDescriptor d;
    d.ctrlIdx = 0;
    d.type = CONTROLLER_TYPE_XBOX;
    d.caps = 0;
    d.touchpadMode = TOUCHPAD_MODE_DS4;
    auto r = svc.upsertSession(devId, "ReceiverTest", "192.168.1.50", key, {d}, false);
    return r.token;
}

// Dispatch `msg` from an exact-size heap buffer with no slack, so an over-read
// past `payload + msgLen` runs off the allocation and ASan flags it in CI.
static DispatchResult dispatchTight(SessionService& svc, uint32_t token, uint16_t msgType,
                                    const std::vector<uint8_t>& msg) {
    std::vector<uint8_t> buf(msg);
    return dispatchInnerMessage(svc, token, msgType, buf.empty() ? nullptr : buf.data(),
                                static_cast<uint16_t>(buf.size()));
}

static void test_decodeMotionReport_littleEndian() {
    TEST("decodeMotionReport — decodes little-endian fields");
    // 16 wire bytes: gyroX..gyroZ, accelX..accelZ (6×i16 LE), then u32 LE.
    uint8_t p[MOTION_WIRE_PAYLOAD_BYTES] = {
        0x10, 0x20,            // gyroX  = 0x2010
        0xFF, 0xFF,            // gyroY  = -1
        0x00, 0x80,            // gyroZ  = -32768
        0x01, 0x00,            // accelX = 1
        0x00, 0x40,            // accelY = 0x4000
        0xFF, 0x7F,            // accelZ = 32767
        0x40, 0xE2, 0x01, 0x00 // timestampDeltaUs = 123456
    };
    MotionReport r = decodeMotionReport(p);
    EXPECT_EQ(static_cast<int>(r.gyroX), 0x2010);
    EXPECT_EQ(static_cast<int>(r.gyroY), -1);
    EXPECT_EQ(static_cast<int>(r.gyroZ), -32768);
    EXPECT_EQ(static_cast<int>(r.accelX), 1);
    EXPECT_EQ(static_cast<int>(r.accelY), 0x4000);
    EXPECT_EQ(static_cast<int>(r.accelZ), 32767);
    EXPECT_EQ(static_cast<unsigned long>(r.timestampDeltaUs), 123456UL);
}

static void test_decodeMotionReport_noOverRead() {
    TEST("decodeMotionReport — reads exactly MOTION_WIRE_PAYLOAD_BYTES, no more");
    std::vector<uint8_t> buf(MOTION_WIRE_PAYLOAD_BYTES, 0x5A);
    MotionReport r = decodeMotionReport(buf.data());
    // 0x5A5A = 23130 for every i16 field; u32 = 0x5A5A5A5A.
    EXPECT_EQ(static_cast<int>(r.gyroX), 0x5A5A);
    EXPECT_EQ(static_cast<unsigned long>(r.timestampDeltaUs), 0x5A5A5A5AUL);
}

static void test_decodeTouchpadReport_wireLayout() {
    TEST("decodeTouchpadReport — decodes flags + both fingers + eventTimeMs");
    uint8_t p[TOUCHPAD_WIRE_PAYLOAD_BYTES] = {
        0x07,                   // flags: finger0 + finger1 + button
        0x11,                   // finger0 trackingId
        0x34, 0x12,             // finger0 x = 0x1234
        0xFF, 0xFF,             // finger0 y = -1
        0x22,                   // finger1 trackingId
        0x00, 0x80,             // finger1 x = -32768
        0xFF, 0x7F,             // finger1 y = 32767
        0x78, 0x56, 0x34, 0x12, // eventTimeMs = 0x12345678 (LE)
    };
    TouchpadReport r = decodeTouchpadReport(p);
    EXPECT(r.finger0.active);
    EXPECT(r.finger1.active);
    EXPECT(r.buttonPressed);
    EXPECT_EQ(static_cast<int>(r.finger0.trackingId), 0x11);
    EXPECT_EQ(static_cast<int>(r.finger0.x), 0x1234);
    EXPECT_EQ(static_cast<int>(r.finger0.y), -1);
    EXPECT_EQ(static_cast<int>(r.finger1.trackingId), 0x22);
    EXPECT_EQ(static_cast<int>(r.finger1.x), -32768);
    EXPECT_EQ(static_cast<int>(r.finger1.y), 32767);
    EXPECT_EQ(r.eventTimeMs, 0x12345678u);
}

static void test_decodeTouchpadReport_noOverRead() {
    TEST("decodeTouchpadReport — reads exactly TOUCHPAD_WIRE_PAYLOAD_BYTES");
    std::vector<uint8_t> buf(TOUCHPAD_WIRE_PAYLOAD_BYTES, 0x00);
    TouchpadReport r = decodeTouchpadReport(buf.data());
    EXPECT(!r.finger0.active);
    EXPECT(!r.finger1.active);
}

static void test_dispatch_motion_truncatedRejected() {
    TEST("dispatchInnerMessage — truncated MSG_MOTION is rejected (no decode)");
    StubGamepad gp;
    StubClient cl;
    StubLog lg;
    SessionService svc(gp, cl, lg);
    uint32_t token = openWithController(svc);

    // A full MSG_MOTION payload is 1 + MOTION_WIRE_PAYLOAD_BYTES = 17 bytes.
    // Every length 0..16 must be rejected before decodeMotionReport runs.
    for (int len = 0; len < 1 + MOTION_WIRE_PAYLOAD_BYTES; ++len) {
        std::vector<uint8_t> msg(static_cast<size_t>(len), 0x5A);
        dispatchTight(svc, token, MSG_MOTION, msg);
    }
    EXPECT_EQ(gp.motionCalls, 0); // nothing decoded / forwarded
}

static void test_dispatch_motion_exactLengthAccepted() {
    TEST("dispatchInnerMessage — MSG_MOTION at exact wire length is decoded");
    StubGamepad gp;
    StubClient cl;
    StubLog lg;
    SessionService svc(gp, cl, lg);
    uint32_t token = openWithController(svc);

    std::vector<uint8_t> msg(1 + MOTION_WIRE_PAYLOAD_BYTES, 0x00);
    msg[0] = 0; // ctrlIdx
    msg[1] = 0x39;
    msg[2] = 0x30; // gyroX = 0x3039 = 12345
    dispatchTight(svc, token, MSG_MOTION, msg);
    EXPECT_EQ(gp.motionCalls, 1);
    EXPECT_EQ(static_cast<int>(gp.lastMotion.gyroX), 12345);
}

static void test_dispatch_motion_oversizedAccepted() {
    TEST("dispatchInnerMessage — oversized MSG_MOTION decodes only the leading bytes");
    StubGamepad gp;
    StubClient cl;
    StubLog lg;
    SessionService svc(gp, cl, lg);
    uint32_t token = openWithController(svc);

    // 8 trailing junk bytes past the 17-byte payload — a forward-compatible
    // sender extension. The guard is `>=`, so it is accepted; the decoder
    // still reads only the documented 16 bytes after ctrlIdx.
    std::vector<uint8_t> msg(1 + MOTION_WIRE_PAYLOAD_BYTES + 8, 0x00);
    msg[0] = 0;
    msg[3] = 0x01; // gyroY low byte → gyroY = 1
    dispatchTight(svc, token, MSG_MOTION, msg);
    EXPECT_EQ(gp.motionCalls, 1);
    EXPECT_EQ(static_cast<int>(gp.lastMotion.gyroY), 1);
}

static void test_dispatch_battery_truncatedRejected() {
    TEST("dispatchInnerMessage — truncated MSG_BATTERY is rejected");
    StubGamepad gp;
    StubClient cl;
    StubLog lg;
    SessionService svc(gp, cl, lg);
    uint32_t token = openWithController(svc);

    // Full payload is ctrlIdx(1) + level(1) + status(1) = 3 bytes.
    for (int len = 0; len < 3; ++len) {
        std::vector<uint8_t> msg(static_cast<size_t>(len), 0x01);
        dispatchTight(svc, token, MSG_BATTERY, msg);
    }
    EXPECT_EQ(gp.batteryCalls, 0);
}

static void test_dispatch_battery_exactLengthAccepted() {
    TEST("dispatchInnerMessage — MSG_BATTERY at exact wire length is decoded");
    StubGamepad gp;
    StubClient cl;
    StubLog lg;
    SessionService svc(gp, cl, lg);
    uint32_t token = openWithController(svc);

    std::vector<uint8_t> msg = {/*ctrlIdx*/ 0, /*level*/ 75, /*status*/ BATTERY_STATUS_CHARGING};
    dispatchTight(svc, token, MSG_BATTERY, msg);
    EXPECT_EQ(gp.batteryCalls, 1);
    EXPECT_EQ(static_cast<int>(gp.lastBattery.level), 75);
    EXPECT_EQ(static_cast<int>(gp.lastBattery.status), static_cast<int>(BATTERY_STATUS_CHARGING));
}

static void test_dispatch_touchpad_truncatedRejected() {
    TEST("dispatchInnerMessage — truncated MSG_TOUCHPAD is rejected (no decode)");
    StubGamepad gp;
    StubClient cl;
    StubLog lg;
    SessionService svc(gp, cl, lg);
    uint32_t token = openWithController(svc);

    // Full payload is 1 + TOUCHPAD_WIRE_PAYLOAD_BYTES = 16 bytes.
    for (int len = 0; len < 1 + TOUCHPAD_WIRE_PAYLOAD_BYTES; ++len) {
        std::vector<uint8_t> msg(static_cast<size_t>(len), 0x07);
        dispatchTight(svc, token, MSG_TOUCHPAD, msg);
    }
    EXPECT_EQ(gp.touchpadCalls, 0);
}

static void test_dispatch_touchpad_exactLengthAccepted() {
    TEST("dispatchInnerMessage — MSG_TOUCHPAD at exact wire length is decoded");
    StubGamepad gp;
    StubClient cl;
    StubLog lg;
    SessionService svc(gp, cl, lg);
    uint32_t token = openWithController(svc);

    std::vector<uint8_t> msg(1 + TOUCHPAD_WIRE_PAYLOAD_BYTES, 0x00);
    msg[0] = 0;    // ctrlIdx
    msg[1] = 0x01; // flags: finger0 active
    msg[2] = 0x2A; // finger0 trackingId
    dispatchTight(svc, token, MSG_TOUCHPAD, msg);
    EXPECT_EQ(gp.touchpadCalls, 1);
    EXPECT(gp.lastTouchpad.finger0.active);
    EXPECT_EQ(static_cast<int>(gp.lastTouchpad.finger0.trackingId), 0x2A);
}

static void test_dispatch_gamepad_truncatedRejected() {
    TEST("dispatchInnerMessage — truncated MSG_GAMEPAD_DATA is rejected");
    StubGamepad gp;
    StubClient cl;
    StubLog lg;
    SessionService svc(gp, cl, lg);
    uint32_t token = openWithController(svc);

    // Full payload is ctrlIdx(1) + GamepadReport(12) = 13 bytes.
    for (int len = 0; len < 13; ++len) {
        std::vector<uint8_t> msg(static_cast<size_t>(len), 0x00);
        DispatchResult dr = dispatchTight(svc, token, MSG_GAMEPAD_DATA, msg);
        EXPECT(!dr.wasGamepadData); // guard tripped before the report was read
    }
    EXPECT_EQ(gp.gamepadCalls, 0);
}

static void test_dispatch_gamepad_exactLengthAccepted() {
    TEST("dispatchInnerMessage — MSG_GAMEPAD_DATA at exact wire length is forwarded");
    StubGamepad gp;
    StubClient cl;
    StubLog lg;
    SessionService svc(gp, cl, lg);
    uint32_t token = openWithController(svc);

    std::vector<uint8_t> msg(13, 0x00); // ctrlIdx + 12-byte report
    DispatchResult dr = dispatchTight(svc, token, MSG_GAMEPAD_DATA, msg);
    EXPECT(dr.wasGamepadData);
    EXPECT(dr.gamepadOk);
    EXPECT_EQ(gp.gamepadCalls, 1);
}

static void test_dispatch_unknownTypeIgnored() {
    TEST("dispatchInnerMessage — unknown message type is ignored, no decode");
    StubGamepad gp;
    StubClient cl;
    StubLog lg;
    SessionService svc(gp, cl, lg);
    uint32_t token = openWithController(svc);

    std::vector<uint8_t> msg(40, 0xFF); // arbitrary bytes
    DispatchResult dr = dispatchTight(svc, token, /*msgType=*/0x7FFF, msg);
    EXPECT(!dr.wasGamepadData);
    EXPECT_EQ(gp.motionCalls, 0);
    EXPECT_EQ(gp.batteryCalls, 0);
    EXPECT_EQ(gp.touchpadCalls, 0);
}

static void test_dispatch_heartbeatZeroLength() {
    TEST("dispatchInnerMessage — MSG_HEARTBEAT_PING with empty payload is handled");
    StubGamepad gp;
    StubClient cl;
    StubLog lg;
    SessionService svc(gp, cl, lg);
    uint32_t token = openWithController(svc);

    std::vector<uint8_t> empty;
    dispatchTight(svc, token, MSG_HEARTBEAT_PING, empty);
    EXPECT_EQ(cl.heartbeatAcks, 1);
}

// Topology mutation is REST-only: the deleted registration opcodes (0x0004
// ADD, 0x0005 REMOVE, 0x0008 TYPE, 0x000E CAPS_UPDATE) MUST fall through to
// the default drop — a spoofed or stale datagram can never mutate the
// controller set again.
static void test_dispatch_deletedRegistrationOpcodesAreDropped() {
    TEST("dispatchInnerMessage — deleted registration opcodes mutate nothing");
    StubGamepad gp;
    StubClient cl;
    StubLog lg;
    SessionService svc(gp, cl, lg);
    uint32_t token = openWithController(svc);

    auto countActive = [&] {
        return (int)svc.getConnectionsSnapshot().connections[0].controllers.size();
    };
    auto typeOfSlot0 = [&] {
        return svc.getConnectionsSnapshot().connections[0].controllers[0].controllerType;
    };
    EXPECT_EQ(countActive(), 1);

    // 0x0004 ADD (idx 1, caps, type) — must not plug a second pad.
    std::vector<uint8_t> add = {0x01, 0x00, 0x00, CONTROLLER_TYPE_PLAYSTATION};
    dispatchTight(svc, token, 0x0004, add);
    EXPECT_EQ(countActive(), 1);

    // 0x0005 REMOVE (idx 0) — must not unplug the live pad.
    std::vector<uint8_t> remove = {0x00};
    dispatchTight(svc, token, 0x0005, remove);
    EXPECT_EQ(countActive(), 1);

    // 0x0008 TYPE (idx 0 → PlayStation) — must not switch the family.
    std::vector<uint8_t> type = {0x00, CONTROLLER_TYPE_PLAYSTATION};
    dispatchTight(svc, token, 0x0008, type);
    EXPECT_EQ((int)typeOfSlot0(), (int)CONTROLLER_TYPE_XBOX);

    // 0x000E CAPS_UPDATE (idx 0, CAP_MOTION) — must not rewrite caps.
    const uint16_t caps = CAP_MOTION;
    std::vector<uint8_t> capsMsg = {0x00, (uint8_t)(caps >> 8), (uint8_t)(caps & 0xFF)};
    dispatchTight(svc, token, 0x000E, capsMsg);
    EXPECT(!svc.getConnectionsSnapshot().connections[0].controllers[0].motionCapable);

    // 0x0007 SERVER_STATUS arriving inbound (it was downstream-only anyway).
    std::vector<uint8_t> status = {0x01, 0x02};
    dispatchTight(svc, token, 0x0007, status);
    EXPECT_EQ(countActive(), 1);
}

static void test_dispatch_gamepad_backendRejectReportsNotOk() {
    TEST("dispatchInnerMessage — MSG_GAMEPAD_DATA surfaces a backend reject as gamepadOk=false");
    StubGamepad gp;
    gp.submitReturnVal = false; // backend refuses the submit
    StubClient cl;
    StubLog lg;
    SessionService svc(gp, cl, lg);
    uint32_t token = openWithController(svc);

    std::vector<uint8_t> msg(13, 0); // ctrlIdx + 12-byte report
    DispatchResult dr = dispatchTight(svc, token, MSG_GAMEPAD_DATA, msg);
    EXPECT(dr.wasGamepadData);
    EXPECT(!dr.gamepadOk);
    EXPECT_EQ(gp.gamepadCalls, 1);
}

int main() {
    std::cout << "Running receiver wire-decode tests...\n\n";

    test_decodeMotionReport_littleEndian();
    test_decodeMotionReport_noOverRead();
    test_decodeTouchpadReport_wireLayout();
    test_decodeTouchpadReport_noOverRead();

    test_dispatch_motion_truncatedRejected();
    test_dispatch_motion_exactLengthAccepted();
    test_dispatch_motion_oversizedAccepted();

    test_dispatch_battery_truncatedRejected();
    test_dispatch_battery_exactLengthAccepted();

    test_dispatch_touchpad_truncatedRejected();
    test_dispatch_touchpad_exactLengthAccepted();

    test_dispatch_gamepad_truncatedRejected();
    test_dispatch_gamepad_exactLengthAccepted();

    test_dispatch_unknownTypeIgnored();
    test_dispatch_heartbeatZeroLength();

    test_dispatch_deletedRegistrationOpcodesAreDropped();
    test_dispatch_gamepad_backendRejectReportsNotOk();

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
