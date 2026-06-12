// SPDX-License-Identifier: LGPL-3.0-or-later

// Crypto + persistence side of pairing acceptance: mints the session key and
// writes the PairedDevice. Shared by the HTTPS dashboard route AND the native
// tray prompts so "accept a pairing" persists identically either way.
// (net/pairing.cpp is the pure, testable request registry.)
#pragma once

#include <string>

// Persist (or replace) a paired device. Both pairing paths land here.
void upsertPairedDevice(const std::string& deviceId, const std::string& deviceName,
                        const std::string& clientIP, const std::string& sharedKeyHex);

// Mint and persist a fresh pairing key for an already-paired device (the
// hmacProof-authed rotation path). False when the device isn't paired.
bool rotatePairedDeviceKey(const std::string& deviceId, const std::string& clientIP,
                           std::string& outKeyHex);

// Accept (dashboard or native prompt): the operator confirmed by sight that
// the shown PIN matches the dish. Mints a key, persists. False when no
// pending request exists (e.g. it already expired).
bool confirmPairing(const std::string& deviceId);

// Decline a pending request. True iff one existed.
bool declinePairing(const std::string& deviceId);
