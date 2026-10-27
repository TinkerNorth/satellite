// SPDX-License-Identifier: LGPL-3.0-or-later

#pragma once

#include "core/ports.h"
#include "net/net_compat.h"

#include <atomic>

class ClientAdapter : public IClientPort {
  public:
    // Borrows the receiver's UDP socket; call after bind.
    void setSocket(SOCKET sock);

    void updateClientAddr(uint32_t token, const std::string& ip, uint16_t port) override;
    void removeClientAddr(uint32_t token) override;

    void sendHeartbeatAck(const Connection& conn, bool backendAvailable,
                          uint8_t totalActiveControllers, uint16_t epoch,
                          uint16_t activeBitmap) override;
    void sendSessionClose(const Connection& conn, uint8_t reason) override;
    void sendRumble(const Connection& conn, uint8_t ctrlIdx, const RumbleReport& report) override;
    void sendTriggerEffects(const Connection& conn, uint8_t ctrlIdx,
                            const TriggerEffectsReport& report) override;
    void sendPlayerLeds(const Connection& conn, uint8_t ctrlIdx, uint8_t ledMask) override;
    void sendSpeakerAudio(const Connection& conn, uint8_t ctrlIdx, uint16_t seq,
                          const uint8_t* opus, size_t opusLen) override;
    void sendHapticAudio(const Connection& conn, uint8_t ctrlIdx, uint16_t seq, const uint8_t* opus,
                         size_t opusLen) override;
    void sendMicLed(const Connection& conn, uint8_t ctrlIdx, uint8_t state) override;
    void sendLightbar(const Connection& conn, uint8_t ctrlIdx, uint8_t r, uint8_t g,
                      uint8_t b) override;

  private:
    std::atomic<SOCKET> sock_{INVALID_SOCKET};

    void sendEncryptedPacket(const Connection& conn, const uint8_t* inner, size_t innerLen);
    // Shared body of the outbound audio lanes (speaker, haptics).
    void sendAudioMessage(const Connection& conn, uint16_t msgType, uint8_t ctrlIdx, uint16_t seq,
                          const uint8_t* opus, size_t opusLen);
};
