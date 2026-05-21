// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * inner_dispatch.h — decrypted inner-message parser.
 *
 * Lightweight, socket-free header: depends only on a forward declaration of
 * SessionService, so the portable test build (tests/test_receiver.cpp) can
 * exercise the per-message length guards without pulling in the receiver's
 * UDP / crypto / globals surface.
 */
#pragma once

#include <cstdint>

class SessionService;

// Outcome of dispatchInnerMessage — lets the receiver loop fold the gamepad
// hot-path result into its telemetry counters without re-parsing the message.
struct DispatchResult {
    bool wasGamepadData = false; // the message was MSG_GAMEPAD_DATA
    bool gamepadOk = false;      // (only meaningful when wasGamepadData) backend accepted it
};

// Parse one decrypted inner message and delegate to SessionService. `payload`
// points at exactly `msgLen` valid bytes (the bytes after the 4-byte inner
// header). Every per-type length guard rejects a short / truncated payload
// before the matching decoder runs, so a malformed or oversized packet cannot
// read past `payload + msgLen`. Exposed for unit testing the length guards
// with raw byte buffers (tests/test_receiver.cpp).
DispatchResult dispatchInnerMessage(SessionService& svc, uint32_t token, uint16_t msgType,
                                    const uint8_t* payload, uint16_t msgLen);
