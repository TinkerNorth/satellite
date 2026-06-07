// SPDX-License-Identifier: LGPL-3.0-or-later

// Split out of receiver.cpp so the length guards have no socket/crypto/globals
// dependency and can be unit tested with raw byte buffers.
#include "inner_dispatch.h"

#include "core/session_service.h"

#include <cstring>

// Every per-type length guard below MUST reject a short payload before its
// decoder runs, so a malformed packet can never read past `payload + msgLen`.
DispatchResult dispatchInnerMessage(SessionService& svc, uint32_t token, uint16_t msgType,
                                    const uint8_t* payload, uint16_t msgLen) {
    DispatchResult result;
    switch (msgType) {
    case MSG_GAMEPAD_DATA: {
        // ctrlIdx(1) + GamepadReport(12) = 13 bytes minimum.
        if (msgLen < 13) break;
        uint8_t ctrlIdx = payload[0];
        GamepadReport report;
        memcpy(&report, payload + 1, sizeof(GamepadReport));
        result.wasGamepadData = true;
        result.gamepadOk = svc.handleGamepadData(token, ctrlIdx, report);
        break;
    }
    case MSG_HEARTBEAT_PING:
        svc.handleHeartbeat(token);
        break;

    case MSG_CONTROLLER_ADD: {
        if (msgLen < 1) break;
        uint8_t ctrlIdx = payload[0];
        // caps: 2 bytes BE, optional — a pre-cap dish sends only ctrlIdx
        // (msgLen 1), caps default to 0.
        uint16_t caps = 0;
        if (msgLen >= 3) {
            caps = static_cast<uint16_t>((static_cast<uint16_t>(payload[1]) << 8) |
                                         static_cast<uint16_t>(payload[2]));
        }
        svc.handleControllerAdd(token, ctrlIdx, caps);
        break;
    }
    case MSG_CONTROLLER_REMOVE: {
        if (msgLen < 1) break;
        uint8_t ctrlIdx = payload[0];
        svc.handleControllerRemove(token, ctrlIdx);
        break;
    }
    case MSG_CONTROLLER_TYPE: {
        if (msgLen < 2) break;
        uint8_t ctrlIdx = payload[0];
        uint8_t ctrlType = payload[1];
        svc.handleControllerType(token, ctrlIdx, ctrlType);
        break;
    }
    case MSG_CONTROLLER_CAPS_UPDATE: {
        // ctrlIdx(1) + caps(2 BE) = 3 bytes. Lets the dish push a mid-session
        // capability change without unplugging the controller.
        if (msgLen < 3) break;
        uint8_t ctrlIdx = payload[0];
        uint16_t caps = static_cast<uint16_t>((static_cast<uint16_t>(payload[1]) << 8) |
                                              static_cast<uint16_t>(payload[2]));
        svc.handleControllerCapsUpdate(token, ctrlIdx, caps);
        break;
    }
    case MSG_MOTION: {
        // ctrlIdx(1) + MOTION_WIRE_PAYLOAD_BYTES(16) = 17 bytes. decodeMotionReport
        // does explicit LE shifts (not a struct memcpy) so the wire stays
        // byte-order-independent against MotionReport layout changes.
        if (msgLen < 1 + MOTION_WIRE_PAYLOAD_BYTES) break;
        uint8_t ctrlIdx = payload[0];
        MotionReport report = decodeMotionReport(payload + 1);
        svc.handleMotionData(token, ctrlIdx, report);
        break;
    }
    case MSG_BATTERY: {
        // Wire payload: ctrlIdx(1) + level(1) + status(1) = 3 bytes.
        if (msgLen < 3) break;
        uint8_t ctrlIdx = payload[0];
        BatteryReport report;
        report.level = payload[1];
        report.status = payload[2];
        svc.handleBatteryUpdate(token, ctrlIdx, report);
        break;
    }
    case MSG_TOUCHPAD: {
        // ctrlIdx(1) + TOUCHPAD_WIRE_PAYLOAD_BYTES(15) = 16 bytes; explicit LE
        // decode like MOTION. Trailing 4 bytes carry eventTimeMs, used by
        // mouse-mode time-scaling to fix the first-touch jump.
        if (msgLen < 1 + TOUCHPAD_WIRE_PAYLOAD_BYTES) break;
        uint8_t ctrlIdx = payload[0];
        TouchpadReport report = decodeTouchpadReport(payload + 1);
        svc.handleTouchpadData(token, ctrlIdx, report);
        break;
    }
    default:
        break;
    }
    return result;
}
