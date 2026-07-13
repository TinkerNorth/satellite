// SPDX-License-Identifier: LGPL-3.0-or-later

// Registry of in-flight client-initiated (Path B) pairing requests: the dish
// shows ITS OWN PIN, POSTs it as a request, and the operator accepts or rejects
// on the dashboard (or native prompt) while the dish polls. (Path A, the
// operator typing a satellite-shown PIN into the dish, is handled inline in
// pairRoute.) Free of libsodium/config so it stays portably unit-testable; the
// webserver route owns key minting and persistence.
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

// A pending request as the dashboard renders it: the dish's PIN is shown so the
// operator can confirm it matches the device before accepting. The staged key
// is never included.
struct PairRequestView {
    std::string deviceId;
    std::string deviceName;
    std::string clientIP;
    std::string pin;
    int secondsRemaining = 0;
};

// Dish → server: "I'm showing <clientPin>; ask the operator to accept." A
// repeat submit for the same deviceId refreshes in place rather than piling up.
void submitPairRequest(const std::string& deviceId, const std::string& deviceName,
                       const std::string& clientIP, const std::string& clientPin);

// Operator rejects. Returns true iff a request for deviceId existed. The
// request is erased so the dish's next poll reads None.
bool denyPairRequest(const std::string& deviceId);

// Dish polls for the operator's decision. Returns the state; when Approved,
// fills outSharedKeyHex exactly once and erases the request (single-use: a
// replayed poll, or a different device reusing the id, gets None).
PairRequestState pollPairRequest(const std::string& deviceId, std::string& outSharedKeyHex);

// Non-expired Pending requests for the dashboard list + SSE push.
std::vector<PairRequestView> pendingPairRequests();

// In-process snapshot of a single request for the native notifier. False if
// no Pending request for deviceId.
bool pairRequestSnapshot(const std::string& deviceId, std::string& outDeviceName,
                         std::string& outClientIP, std::string& outPin, int& outSecondsRemaining);

// Accept a request the operator confirmed by sight. True iff a non-expired
// Pending request existed; on success the request flips to Approved (staging
// mintedKeyHex for the next poll) and outDeviceName/outClientIP are filled.
// On false the caller discards mintedKeyHex.
bool acceptPairRequestConfirmed(const std::string& deviceId, const std::string& mintedKeyHex,
                                std::string& outDeviceName, std::string& outClientIP);

// Register a callback fired when a new Path-B request arrives. Invoked on the
// submitting thread OUTSIDE the registry lock, so the callback may call back in
// safely. Set-once at startup.
void setPairRequestListener(std::function<void(const std::string& deviceId)> cb);

// Test seam: drop all state so a unit test starts from a clean registry.
void resetPairRequestsForTest();
