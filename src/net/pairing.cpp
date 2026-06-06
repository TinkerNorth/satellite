// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

#include "pairing.h"

#include <algorithm>
#include <chrono>
#include <mutex>

namespace {
struct PendingPair {
    std::string deviceId;
    std::string deviceName;
    std::string clientIP;
    std::string clientPin; // the PIN the dish is showing; the operator must type it back
    std::string keyHex;    // staged session key, set on Approved
    PairRequestState state = PairRequestState::Pending;
    std::chrono::steady_clock::time_point createdAt;
};

std::mutex g_pairMtx;
std::vector<PendingPair> g_pending;

// Matches the Path-A PIN window (5 min) loosely, but shorter: a request is a
// live "someone is staring at both screens right now" handshake, so a tight
// TTL keeps a forgotten prompt from lingering on the dashboard.
constexpr auto kPairTtl = std::chrono::minutes(2);
// Cap the list so a flood of unknown devices can't push the operator's real
// request off-screen. Oldest is evicted first.
constexpr size_t kMaxPending = 8;

bool isExpired(const PendingPair& p) {
    return std::chrono::steady_clock::now() - p.createdAt > kPairTtl;
}

void pruneLocked() {
    g_pending.erase(std::remove_if(g_pending.begin(), g_pending.end(),
                                   [](const PendingPair& p) { return isExpired(p); }),
                    g_pending.end());
}

PendingPair* findLocked(const std::string& deviceId) {
    for (auto& p : g_pending) {
        if (p.deviceId == deviceId) return &p;
    }
    return nullptr;
}

void eraseLocked(const std::string& deviceId) {
    g_pending.erase(std::remove_if(g_pending.begin(), g_pending.end(),
                                   [&](const PendingPair& p) { return p.deviceId == deviceId; }),
                    g_pending.end());
}
} // namespace

const char* pairRequestStateName(PairRequestState s) {
    switch (s) {
    case PairRequestState::None:
        return "none";
    case PairRequestState::Pending:
        return "pending";
    case PairRequestState::Approved:
        return "approved";
    case PairRequestState::Denied:
        return "denied";
    }
    return "none";
}

void submitPairRequest(const std::string& deviceId, const std::string& deviceName,
                       const std::string& clientIP, const std::string& clientPin) {
    std::lock_guard<std::mutex> lk(g_pairMtx);
    pruneLocked();

    if (auto* existing = findLocked(deviceId)) {
        // A re-tap from the same dish refreshes its PIN + timer in place; the
        // operator should never see two rows for one phone.
        existing->deviceName = deviceName;
        existing->clientIP = clientIP;
        existing->clientPin = clientPin;
        existing->keyHex.clear();
        existing->state = PairRequestState::Pending;
        existing->createdAt = std::chrono::steady_clock::now();
        return;
    }

    if (g_pending.size() >= kMaxPending) g_pending.erase(g_pending.begin());

    PendingPair p;
    p.deviceId = deviceId;
    p.deviceName = deviceName;
    p.clientIP = clientIP;
    p.clientPin = clientPin;
    p.state = PairRequestState::Pending;
    p.createdAt = std::chrono::steady_clock::now();
    g_pending.push_back(std::move(p));
}

bool acceptPairRequest(const std::string& deviceId, const std::string& operatorPin,
                       const std::string& mintedKeyHex, std::string& outDeviceName,
                       std::string& outClientIP) {
    std::lock_guard<std::mutex> lk(g_pairMtx);
    pruneLocked();

    auto* p = findLocked(deviceId);
    if (p == nullptr || p->state != PairRequestState::Pending) return false;
    // The operator-typed PIN must match the one on the dish — that match is the
    // whole authentication: it proves the operator physically saw the device.
    if (operatorPin != p->clientPin) return false;

    p->state = PairRequestState::Approved;
    p->keyHex = mintedKeyHex;
    outDeviceName = p->deviceName;
    outClientIP = p->clientIP;
    return true;
}

bool denyPairRequest(const std::string& deviceId) {
    std::lock_guard<std::mutex> lk(g_pairMtx);
    auto* p = findLocked(deviceId);
    if (p == nullptr) return false;
    eraseLocked(deviceId);
    return true;
}

PairRequestState pollPairRequest(const std::string& deviceId, std::string& outSharedKeyHex) {
    std::lock_guard<std::mutex> lk(g_pairMtx);
    pruneLocked();

    auto* p = findLocked(deviceId);
    if (p == nullptr) return PairRequestState::None;

    if (p->state == PairRequestState::Approved) {
        outSharedKeyHex = p->keyHex;
        // Hand the key back exactly once, then forget the request so a replayed
        // poll can't re-read it and a later reuse of the id starts clean.
        eraseLocked(deviceId);
        return PairRequestState::Approved;
    }
    return p->state;
}

std::vector<PairRequestView> pendingPairRequests() {
    std::lock_guard<std::mutex> lk(g_pairMtx);
    pruneLocked();

    std::vector<PairRequestView> out;
    const auto now = std::chrono::steady_clock::now();
    for (const auto& p : g_pending) {
        if (p.state != PairRequestState::Pending) continue;
        const int rem = static_cast<int>(
            std::chrono::duration_cast<std::chrono::seconds>(kPairTtl - (now - p.createdAt))
                .count());
        out.push_back({p.deviceId, p.deviceName, p.clientIP, rem < 0 ? 0 : rem});
    }
    return out;
}

void resetPairRequestsForTest() {
    std::lock_guard<std::mutex> lk(g_pairMtx);
    g_pending.clear();
}
