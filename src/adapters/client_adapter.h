// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * adapters/client_adapter.h — IClientPort implementation.
 *
 * Wraps encrypted UDP packet sending via the net_compat socket shim.
 * Owns: socket reference, token→sockaddr_in mapping.
 */
#pragma once

#include "core/ports.h"
#include "net/net_compat.h"

#include <unordered_map>
#include <mutex>

class ClientAdapter : public IClientPort {
  public:
    // The adapter borrows the receiver's UDP socket (set after bind).
    void setSocket(SOCKET sock);

    void updateClientAddr(uint32_t token, const std::string& ip, uint16_t port) override;
    void removeClientAddr(uint32_t token) override;

    void sendHeartbeatAck(const Connection& conn) override;
    void sendControllerAck(const Connection& conn, uint16_t requestType, uint8_t ctrlIdx,
                           uint8_t result) override;
    void sendServerStatus(const Connection& conn, bool backendAvailable,
                          uint8_t totalActiveControllers) override;
    void
    broadcastServerStatus(const std::vector<std::pair<uint32_t, const Connection*>>& connections,
                          bool backendAvailable, uint8_t totalActiveControllers) override;
    void sendRumble(const Connection& conn, uint8_t ctrlIdx, const RumbleReport& report) override;

  private:
    SOCKET sock_ = INVALID_SOCKET;
    std::mutex addrMtx_;
    std::unordered_map<uint32_t, sockaddr_in> addrs_;

    // Build a full encrypted packet and send it.
    void sendEncryptedPacket(const Connection& conn, const uint8_t* inner, size_t innerLen);

    // Resolve the sockaddr_in for a token.
    bool getAddr(uint32_t token, sockaddr_in& out);
};
