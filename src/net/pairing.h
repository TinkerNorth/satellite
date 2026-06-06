// SPDX-License-Identifier: LGPL-3.0-or-later

// Registry of in-flight client-initiated (Path B) pairing requests: the dish
// shows ITS OWN PIN, POSTs it as a request, and the operator accepts on the
// dashboard by typing it back; the dish polls until they act. (Path A — the
// operator types a satellite-generated PIN into the dish — is handled inline in
// pairRoute.) Deliberately free of libsodium/config so it stays portably
// unit-testable; the webserver route owns key minting and persistence.
#pragma once

#include <functional>
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
// or key: the operator authenticates by typing the PIN shown on the dish, so
// echoing it on the server would let a dashboard shoulder-surfer accept blind.
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

// Operator accepts by typing the dish's PIN. True iff a non-expired Pending
// request existed AND `operatorPin` matched; on success the request flips to
// Approved (staging mintedKeyHex for the next poll) and outDeviceName/
// outClientIP are filled. On false the caller discards mintedKeyHex.
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

// In-process snapshot INCLUDING the PIN — for the native notifier only. The PIN
// is never exposed over the HTTP API; a native prompt *shows* it for visual
// confirmation, safe because it never leaves the process. False if no Pending.
bool pairRequestSnapshot(const std::string& deviceId, std::string& outDeviceName,
                         std::string& outClientIP, std::string& outPin, int& outSecondsRemaining);

// Accept a request the operator confirmed by sight (the native prompt showed
// the PIN and they verified it matches the dish), so no PIN is re-checked.
// Otherwise mirrors acceptPairRequest. Returns false when no Pending request.
bool acceptPairRequestConfirmed(const std::string& deviceId, const std::string& mintedKeyHex,
                                std::string& outDeviceName, std::string& outClientIP);

// Register a callback fired when a new Path-B request arrives, so the platform
// can raise a native notification. Invoked on the submitting thread *outside*
// the registry lock, so the callback may call back in safely. Set-once at
// startup; the deviceId can be handed to pairRequestSnapshot.
void setPairRequestListener(std::function<void(const std::string& deviceId)> cb);

// Test seam: drop all state so a unit test starts from a clean registry.
void resetPairRequestsForTest();
