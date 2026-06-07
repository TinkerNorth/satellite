// SPDX-License-Identifier: LGPL-3.0-or-later

// Decrypted inner-message parser. Socket-free (only a SessionService forward
// decl) so the portable test build can exercise the length guards without the
// receiver's UDP/crypto/globals surface.
#pragma once

#include <cstdint>

class SessionService;

// Lets the receiver loop fold the gamepad hot-path result into its telemetry
// counters without re-parsing the message.
struct DispatchResult {
    bool wasGamepadData = false; // the message was MSG_GAMEPAD_DATA
    bool gamepadOk = false;      // (only meaningful when wasGamepadData) backend accepted it
};

// Parse one decrypted inner message and delegate to SessionService. `payload`
// points at exactly `msgLen` valid bytes. Every per-type length guard rejects a
// short payload before its decoder runs, so a malformed packet cannot read past
// `payload + msgLen`.
DispatchResult dispatchInnerMessage(SessionService& svc, uint32_t token, uint16_t msgType,
                                    const uint8_t* payload, uint16_t msgLen);
