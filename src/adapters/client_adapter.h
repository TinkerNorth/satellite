// SPDX-License-Identifier: LGPL-3.0-or-later

#pragma once

#include "core/ports.h"
#include "net/net_compat.h"

#include <unordered_map>
#include <mutex>

class ClientAdapter : public IClientPort {
  public:
    // Borrows the receiver's UDP socket; call after bind.
    void setSocket(SOCKET sock);

    void updateClientAddr(uint32_t token, const std::string& ip, uint16_t port) override;
    // Hot path (one call per received UDP packet): writes the IPv4 directly
    // into sin_addr, skipping the inet_pton + std::string round-trip.
    void updateClientAddrV4(uint32_t token, uint32_t ipv4NetworkOrder, uint16_t port) override;
    void removeClientAddr(uint32_t token) override;

    void sendHeartbeatAck(const Connection& conn) override;
    void sendControllerAck(const Connection& conn, uint16_t requestType, uint8_t ctrlIdx,
                           uint8_t result, uint8_t motionFlags = 0) override;
    void sendServerStatus(const Connection& conn, bool backendAvailable,
                          uint8_t totalActiveControllers) override;
    void
    broadcastServerStatus(const std::vector<std::pair<uint32_t, const Connection*>>& connections,
                          bool backendAvailable, uint8_t totalActiveControllers) override;
    void sendRumble(const Connection& conn, uint8_t ctrlIdx, const RumbleReport& report) override;
    void sendLightbar(const Connection& conn, uint8_t ctrlIdx, uint8_t r, uint8_t g,
                      uint8_t b) override;

  private:
    SOCKET sock_ = INVALID_SOCKET;
    std::mutex addrMtx_;
    std::unordered_map<uint32_t, sockaddr_in> addrs_;

    void sendEncryptedPacket(const Connection& conn, const uint8_t* inner, size_t innerLen);

    bool getAddr(uint32_t token, sockaddr_in& out);
};
