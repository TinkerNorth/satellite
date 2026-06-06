// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * net/pairing_service.h — the crypto + persistence side of pairing acceptance.
 *
 * net/pairing.cpp is a pure (sodium-free, config-free) request registry so it
 * stays portably testable. This module is the glue that mints the session key
 * and writes the PairedDevice — shared by the HTTPS dashboard route AND the
 * platform-native tray prompts, so "accept a pairing" means the same thing, and
 * persists identically, regardless of where the operator clicked.
 */
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
