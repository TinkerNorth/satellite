// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * net/pairing.h — client-initiated ("server accepts the dish's PIN") pairing
 * request registry.
 *
 * Two pairing directions now exist, and either completes a pairing:
 *
 *   Path A (pre-existing): the operator generates a PIN on the satellite and
 *     types it into the dish. The dish proves it by POSTing the PIN, the
 *     server verifyPin()s it, pairs immediately. Handled inline in pairRoute.
 *
 *   Path B (this module): the dish shows ITS OWN PIN, POSTs it as a *request*,
 *     and the operator accepts on the satellite dashboard by typing that PIN
 *     back. Asynchronous — the dish polls until the operator acts.
 *
 * This registry holds the in-flight Path-B requests. It is deliberately free
 * of libsodium and config so it stays portably unit-testable; the webserver
 * route owns key minting (it has crypto) and PairedDevice persistence.
 */
#pragma once

#include <string>
#include <vector>

enum class PairRequestState {
    None,     // no request outstanding for this device
    Pending,  // awaiting operator accept/deny on the satellite dashboard
    Approved, // operator accepted; the shared key is staged for one poll
    Denied,   // operator rejected
};

const char* pairRequestStateName(PairRequestState s);

// A pending request as the dashboard renders it. Deliberately carries no PIN
// and no key: the operator authenticates by *typing the PIN shown on the dish*
// (the mirror of Path A), so echoing it on the server would defeat the point
// and let a shoulder-surfer of the dashboard accept without seeing the dish.
struct PairRequestView {
    std::string deviceId;
    std::string deviceName;
    std::string clientIP;
    int secondsRemaining = 0;
};

// Dish → server: "I'm showing <clientPin>; ask the operator to accept." A
// repeat submit for the same deviceId refreshes in place rather than piling up.
void submitPairRequest(const std::string& deviceId, const std::string& deviceName,
                       const std::string& clientIP, const std::string& clientPin);

// Operator accepts by typing the dish's PIN. `mintedKeyHex` is the session key
// the caller generated (the caller owns crypto). Returns true iff a non-expired
// Pending request existed AND `operatorPin` matched the dish's PIN; on success
// the request flips to Approved (staging mintedKeyHex for the dish's next poll)
// and outDeviceName / outClientIP are filled so the caller can persist the
// PairedDevice. On false the caller discards mintedKeyHex.
bool acceptPairRequest(const std::string& deviceId, const std::string& operatorPin,
                       const std::string& mintedKeyHex, std::string& outDeviceName,
                       std::string& outClientIP);

// Operator rejects. Returns true iff a request for deviceId existed. The
// request is erased so the dish's next poll reads None → "declined".
bool denyPairRequest(const std::string& deviceId);

// Dish polls for the operator's decision. Returns the state; when Approved,
// fills outSharedKeyHex exactly once and erases the request (single-use — a
// replayed poll, or a different device reusing the id, gets None).
PairRequestState pollPairRequest(const std::string& deviceId, std::string& outSharedKeyHex);

// Non-expired Pending requests for the dashboard list + SSE push.
std::vector<PairRequestView> pendingPairRequests();

// Test seam: drop all state so a unit test starts from a clean registry.
void resetPairRequestsForTest();
