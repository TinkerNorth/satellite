// SPDX-License-Identifier: LGPL-3.0-or-later

// Crypto + persistence side of pairing acceptance: mints the session key and
// writes the PairedDevice. Shared by the HTTPS dashboard route AND the native
// tray prompts so "accept a pairing" persists identically either way.
// (net/pairing.cpp is the pure, testable request registry.)
#pragma once

#include <string>

// Persist (or replace) a paired device. Both pairing paths land here.
void upsertPairedDevice(const std::string& deviceId, const std::string& deviceName,
                        const std::string& clientIP, const std::string& sharedKeyHex,
                        const std::string& initialTouchpadMode);

// Dashboard accept: the operator typed the dish's PIN. Mints a key, verifies the
// typed PIN against the pending request, persists on match. False on mismatch /
// no pending request.
bool acceptPairingWithPin(const std::string& deviceId, const std::string& operatorPin);

// Native accept: the operator confirmed by sight that the prompt's PIN matches
// the dish, so nothing is re-typed. Mints a key, persists. False when no pending
// request exists (e.g. it already expired).
bool confirmPairing(const std::string& deviceId);

// Decline a pending request. True iff one existed.
bool declinePairing(const std::string& deviceId);
