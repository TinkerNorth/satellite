// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * adapters/client_adapter.cpp — IClientPort implementation.
 */
#include "client_adapter.h"
#include <cstring>

// ── Crypto functions (defined in crypto.cpp) ──────────────────────────────
extern bool encryptPacket(const uint8_t key[32], uint32_t counter, uint32_t token,
                          const uint8_t* plaintext, size_t ptLen, uint8_t* ciphertext,
                          unsigned long long* ctLen);

void ClientAdapter::setSocket(SOCKET sock) { sock_ = sock; }

void ClientAdapter::updateClientAddr(uint32_t token, const std::string& ip, uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

    std::lock_guard<std::mutex> lk(addrMtx_);
    addrs_[token] = addr;
}

void ClientAdapter::removeClientAddr(uint32_t token) {
    std::lock_guard<std::mutex> lk(addrMtx_);
    addrs_.erase(token);
}

bool ClientAdapter::getAddr(uint32_t token, sockaddr_in& out) {
    std::lock_guard<std::mutex> lk(addrMtx_);
    auto it = addrs_.find(token);
    if (it == addrs_.end()) return false;
    out = it->second;
    return true;
}

void ClientAdapter::sendEncryptedPacket(const Connection& conn, const uint8_t* inner,
                                        size_t innerLen) {
    if (sock_ == INVALID_SOCKET) return;

    sockaddr_in addr{};
    if (!getAddr(conn.token, addr)) return;

    uint8_t ct[64 + AUTH_TAG_SIZE]; // max inner we send is 11 bytes (rumble)
    unsigned long long ctLen = 0;
    if (!encryptPacket(conn.sharedKey, 0, conn.token, inner, innerLen, ct, &ctLen)) return;

    uint8_t pkt[HEADER_SIZE + sizeof(ct)];
    uint32_t t = conn.token;
    pkt[0] = (uint8_t)(t >> 24);
    pkt[1] = (uint8_t)(t >> 16);
    pkt[2] = (uint8_t)(t >> 8);
    pkt[3] = (uint8_t)(t);
    pkt[4] = 0;
    pkt[5] = 0;
    pkt[6] = 0;
    pkt[7] = 0; // counter 0
    memcpy(pkt + HEADER_SIZE, ct, ctLen);

    sendto(sock_, reinterpret_cast<const char*>(pkt), (int)(HEADER_SIZE + ctLen), 0,
           reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
}

// ── IClientPort methods ──────────────────────────────────────────────────

void ClientAdapter::sendHeartbeatAck(const Connection& conn) {
    uint8_t inner[4];
    inner[0] = (uint8_t)(MSG_HEARTBEAT_ACK >> 8);
    inner[1] = (uint8_t)(MSG_HEARTBEAT_ACK);
    inner[2] = 0;
    inner[3] = 0;
    sendEncryptedPacket(conn, inner, 4);
}

void ClientAdapter::sendControllerAck(const Connection& conn, uint16_t requestType, uint8_t ctrlIdx,
                                      uint8_t result, uint8_t motionFlags) {
    // Payload: reqType(2) + ctrlIdx(1) + result(1) + motionFlags(1) = 5 bytes.
    // The motionFlags byte is always present (zero or not) so the dish-side
    // parser can distinguish "old satellite (msgLen == 4, motion status
    // unknown)" from "new satellite, flags happen to be zero."
    uint8_t inner[INNER_HEADER_SIZE + 5];
    inner[0] = (uint8_t)(MSG_CONTROLLER_ACK >> 8);
    inner[1] = (uint8_t)(MSG_CONTROLLER_ACK);
    inner[2] = 0;
    inner[3] = 5;
    inner[4] = (uint8_t)(requestType >> 8);
    inner[5] = (uint8_t)(requestType);
    inner[6] = ctrlIdx;
    inner[7] = result;
    inner[8] = motionFlags;
    sendEncryptedPacket(conn, inner, sizeof(inner));
}

void ClientAdapter::sendServerStatus(const Connection& conn, bool backendAvailable,
                                     uint8_t totalActiveControllers) {
    uint8_t inner[6];
    inner[0] = (uint8_t)(MSG_SERVER_STATUS >> 8);
    inner[1] = (uint8_t)(MSG_SERVER_STATUS);
    inner[2] = 0;
    inner[3] = 2;
    inner[4] = backendAvailable ? 1 : 0;
    inner[5] = totalActiveControllers;
    sendEncryptedPacket(conn, inner, 6);
}

void ClientAdapter::broadcastServerStatus(
    const std::vector<std::pair<uint32_t, const Connection*>>& connections, bool backendAvailable,
    uint8_t totalActiveControllers) {
    for (auto& [tok, conn] : connections) {
        sendServerStatus(*conn, backendAvailable, totalActiveControllers);
    }
}

void ClientAdapter::sendRumble(const Connection& conn, uint8_t ctrlIdx,
                               const RumbleReport& report) {
    // Payload: ctrlIdx + strong + weak + duration = 7 bytes. Motor vibration
    // only — lightbar colour has its own message (sendLightbar / 0x000D).
    const uint16_t payloadLen = 7;

    uint8_t inner[4 + 7];
    inner[0] = (uint8_t)(MSG_RUMBLE >> 8);
    inner[1] = (uint8_t)(MSG_RUMBLE);
    inner[2] = (uint8_t)(payloadLen >> 8);
    inner[3] = (uint8_t)(payloadLen);
    inner[4] = ctrlIdx;
    inner[5] = (uint8_t)(report.strongMagnitude >> 8);
    inner[6] = (uint8_t)(report.strongMagnitude);
    inner[7] = (uint8_t)(report.weakMagnitude >> 8);
    inner[8] = (uint8_t)(report.weakMagnitude);
    inner[9] = (uint8_t)(report.durationMs >> 8);
    inner[10] = (uint8_t)(report.durationMs);
    sendEncryptedPacket(conn, inner, sizeof(inner));
}

void ClientAdapter::sendLightbar(const Connection& conn, uint8_t ctrlIdx, uint8_t r, uint8_t g,
                                 uint8_t b) {
    // Wire payload: ctrlIdx(1) + r(1) + g(1) + b(1) = 4 bytes.
    uint8_t inner[4 + 4];
    inner[0] = (uint8_t)(MSG_LIGHTBAR >> 8);
    inner[1] = (uint8_t)(MSG_LIGHTBAR);
    inner[2] = 0; // payload length high
    inner[3] = 4; // payload length low
    inner[4] = ctrlIdx;
    inner[5] = r;
    inner[6] = g;
    inner[7] = b;
    sendEncryptedPacket(conn, inner, sizeof(inner));
}
