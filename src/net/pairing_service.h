// SPDX-License-Identifier: LGPL-3.0-or-later

// Crypto + persistence side of pairing acceptance: mints the session key and
// writes the PairedDevice. Shared by the HTTPS dashboard route AND the native
// tray prompts so "accept a pairing" persists identically either way.
// (net/pairing.cpp is the pure request registry.)
#pragma once

#include "core/types.h"

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

// The stored record for a device, copied out under the config lock so a
// concurrent unpair cannot dangle it. False when the device is not paired.
bool findPairedDevice(const std::string& deviceId, PairedDevice& out);

// The last-seen identity a session PUT refreshes: the name can change on
// the client between sessions. A device that is not paired is left alone.
void refreshPairedDeviceIdentity(const std::string& deviceId, const std::string& clientIP,
                                 const std::string& deviceName);

// Removes the record and persists the change; false when there was none.
bool forgetPairedDevice(const std::string& deviceId);
