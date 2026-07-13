// SPDX-License-Identifier: LGPL-3.0-or-later

#include "client_adapter.h"

#include "net/session_crypto.h"

#include <cstring>

void ClientAdapter::setSocket(SOCKET sock) { sock_ = sock; }

void ClientAdapter::updateClientAddr(uint32_t token, const std::string& ip, uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

    std::lock_guard<std::mutex> lk(addrMtx_);
    addrs_[token] = addr;
}

void ClientAdapter::updateClientAddrV4(uint32_t token, uint32_t ipv4NetworkOrder, uint16_t port) {
    // Already network byte order, so drop it straight in; avoids the string
    // overload's heap allocation on the hot receive path.
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = ipv4NetworkOrder;

    std::lock_guard<std::mutex> lk(addrMtx_);
    addrs_[token] = addr;
}

void ClientAdapter::removeClientAddr(uint32_t token) {
    std::lock_guard<std::mutex> lk(addrMtx_);
    addrs_.erase(token);
    txCounters_.erase(token);
}

bool ClientAdapter::getAddr(uint32_t token, sockaddr_in& out) {
    std::lock_guard<std::mutex> lk(addrMtx_);
    auto it = addrs_.find(token);
    if (it == addrs_.end()) return false;
    out = it->second;
    return true;
}

uint32_t ClientAdapter::nextTxCounter(uint32_t token) {
    std::lock_guard<std::mutex> lk(addrMtx_);
    return ++txCounters_[token];
}

void ClientAdapter::sendEncryptedPacket(const Connection& conn, const uint8_t* inner,
                                        size_t innerLen) {
    if (sock_ == INVALID_SOCKET) return;

    sockaddr_in addr{};
    if (!getAddr(conn.token, addr)) return;

    // Monotonic per-token counter in the nonce; the direction byte keeps this
    // direction's nonces disjoint from the client's under the shared session key.
    const uint32_t counter = nextTxCounter(conn.token);

    uint8_t ct[64 + AUTH_TAG_SIZE]; // max inner we send is 11 bytes (rumble)
    unsigned long long ctLen = 0;
    if (!encryptPacket(conn.sessionKey, CRYPTO_DIR_SERVER_TO_CLIENT, counter, conn.token, inner,
                       innerLen, ct, &ctLen)) {
        return;
    }

    uint8_t pkt[HEADER_SIZE + sizeof(ct)];
    uint32_t t = conn.token;
    pkt[0] = (uint8_t)(t >> 24);
    pkt[1] = (uint8_t)(t >> 16);
    pkt[2] = (uint8_t)(t >> 8);
    pkt[3] = (uint8_t)(t);
    pkt[4] = (uint8_t)(counter >> 24);
    pkt[5] = (uint8_t)(counter >> 16);
    pkt[6] = (uint8_t)(counter >> 8);
    pkt[7] = (uint8_t)(counter);
    memcpy(pkt + HEADER_SIZE, ct, ctLen);

    sendto(sock_, reinterpret_cast<const char*>(pkt), (int)(HEADER_SIZE + ctLen), 0,
           reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
}

void ClientAdapter::sendHeartbeatAck(const Connection& conn, bool backendAvailable,
                                     uint8_t totalActiveControllers, uint16_t epoch,
                                     uint16_t activeBitmap) {
    // Wire: backendAvailable(1) + totalActiveControllers(1) + epoch(u16 BE) +
    // bitmap(u16 BE) = 6 bytes. The epoch/bitmap pair makes involuntary
    // server-side topology loss self-healing within one heartbeat.
    uint8_t inner[INNER_HEADER_SIZE + 6];
    inner[0] = (uint8_t)(MSG_HEARTBEAT_ACK >> 8);
    inner[1] = (uint8_t)(MSG_HEARTBEAT_ACK);
    inner[2] = 0;
    inner[3] = 6;
    inner[4] = backendAvailable ? 1 : 0;
    inner[5] = totalActiveControllers;
    inner[6] = (uint8_t)(epoch >> 8);
    inner[7] = (uint8_t)(epoch);
    inner[8] = (uint8_t)(activeBitmap >> 8);
    inner[9] = (uint8_t)(activeBitmap);
    sendEncryptedPacket(conn, inner, sizeof(inner));
}

void ClientAdapter::sendSessionClose(const Connection& conn, uint8_t reason) {
    // Wire: reason(1). Best-effort; must go out BEFORE teardown while the
    // session key and address still exist.
    uint8_t inner[INNER_HEADER_SIZE + 1];
    inner[0] = (uint8_t)(MSG_SESSION_CLOSE >> 8);
    inner[1] = (uint8_t)(MSG_SESSION_CLOSE);
    inner[2] = 0;
    inner[3] = 1;
    inner[4] = reason;
    sendEncryptedPacket(conn, inner, sizeof(inner));
}

void ClientAdapter::sendRumble(const Connection& conn, uint8_t ctrlIdx,
                               const RumbleReport& report) {
    // Wire: ctrlIdx+strong+weak+duration=7 bytes. Motor only; lightbar colour
    // is a separate message (sendLightbar / 0x000D).
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
    // Wire: ctrlIdx(1)+r(1)+g(1)+b(1)=4 bytes.
    uint8_t inner[4 + 4];
    inner[0] = (uint8_t)(MSG_LIGHTBAR >> 8);
    inner[1] = (uint8_t)(MSG_LIGHTBAR);
    inner[2] = 0;
    inner[3] = 4;
    inner[4] = ctrlIdx;
    inner[5] = r;
    inner[6] = g;
    inner[7] = b;
    sendEncryptedPacket(conn, inner, sizeof(inner));
}
