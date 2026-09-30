// SPDX-License-Identifier: LGPL-3.0-or-later

// Decrypted inner-message parser. Socket-free (only a SessionService forward
// decl) so the portable test build can exercise the length guards without the
// receiver's UDP/crypto/globals surface.
#pragma once

#include <cstddef>
#include <cstdint>

class SessionService;

// Lets the receiver loop fold the gamepad hot-path result into its telemetry
// counters without re-parsing the message.
struct DispatchResult {
    bool wasGamepadData = false;
    bool gamepadOk = false; // only meaningful when wasGamepadData
    bool handled = false;
};

// Parse one decrypted inner message and delegate to SessionService. `payload`
// points at exactly `msgLen` valid bytes. Every per-type length guard rejects a
// short payload before its decoder runs, so a malformed packet cannot read past
// `payload + msgLen`.
DispatchResult dispatchInnerMessage(SessionService& svc, uint32_t token, uint16_t msgType,
                                    const uint8_t* payload, uint16_t msgLen);

// The four bytes every decrypted message starts with: type and payload
// length, big-endian. False when the plaintext cannot hold the header or the
// length runs past it, which is what a malformed datagram looks like once it
// has decrypted.
struct InnerHeader {
    uint16_t msgType = 0;
    uint16_t msgLen = 0;
};
bool parseInnerHeader(const uint8_t* plaintext, size_t ptLen, InnerHeader& out);
