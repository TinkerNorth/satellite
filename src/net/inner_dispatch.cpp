// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * inner_dispatch.cpp — decrypted inner-message parser.
 *
 * Split out of receiver.cpp so the per-message length guards have no socket /
 * crypto / globals dependency and can be unit tested directly with raw byte
 * buffers (tests/test_receiver.cpp). receiver.cpp owns the UDP socket, recv
 * loop and decryption; once it has a decrypted plaintext it hands the inner
 * message here.
 */
#include "inner_dispatch.h"

#include "core/session_service.h"

#include <cstring>

// Parses the decrypted inner-message payload and delegates to SessionService.
//
// `payload` points at exactly `msgLen` valid bytes (the bytes after the 4-byte
// inner header). The caller has already verified `INNER_HEADER_SIZE + msgLen`
// fits the decrypted plaintext. Every per-type length guard below MUST reject
// a short / truncated payload before the matching decoder runs, so a malformed
// or oversized packet can never read past `payload + msgLen`.
//
// Returns whether this was a MSG_GAMEPAD_DATA packet and, if so, whether the
// backend accepted the report — the caller folds that into the g_submitOk /
// g_submitFail telemetry counters.
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
        // Capability word: 2 bytes big-endian, optional. A pre-cap
        // dish sends only ctrlIdx (msgLen 1) — caps default to 0.
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
    case MSG_MOTION: {
        // Wire payload: ctrlIdx(1) + MOTION_WIRE_PAYLOAD_BYTES(16) = 17 bytes.
        // Decoded with explicit little-endian shifts — NOT a struct memcpy —
        // so the wire stays byte-order-independent and a future change to
        // MotionReport's layout/padding can't silently corrupt it. Mirrors
        // the MSG_TOUCHPAD decode.
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
        // Wire payload: ctrlIdx(1) + TOUCHPAD_WIRE_PAYLOAD_BYTES(11) = 12
        // bytes. decodeTouchpadReport does the explicit little-endian decode
        // (see core/types.h) — same pattern as MOTION.
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
