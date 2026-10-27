// SPDX-License-Identifier: LGPL-3.0-or-later

#include "client_adapter.h"
#include "core/byte_order.h"
#include "core/hot_table.h"

#include "net/session_crypto.h"

#include "app/wire_stats.h"

#include <cstring>
#include <thread>

void ClientAdapter::setSocket(SOCKET sock) { sock_.store(sock, std::memory_order_release); }

void ClientAdapter::updateClientAddr(uint32_t, const std::string&, uint16_t) {}

void ClientAdapter::removeClientAddr(uint32_t) {}

namespace {

constexpr uint64_t ACK_PRESENT = uint64_t{1} << 63;
constexpr size_t ACK_PAYLOAD_BYTES = 6;

uint64_t packAck(bool backendAvailable, uint8_t totalActiveControllers, uint16_t epoch,
                 uint16_t activeBitmap) {
    return ACK_PRESENT | (backendAvailable ? 1u : 0u) |
           (static_cast<uint64_t>(totalActiveControllers) << 8) |
           (static_cast<uint64_t>(epoch) << 16) | (static_cast<uint64_t>(activeBitmap) << 32);
}

bool tryHoldGate(HotSlot& slot) {
    uint32_t free = 0;
    return slot.txGate.compare_exchange_strong(free, 1, std::memory_order_seq_cst);
}

void holdGate(HotSlot& slot) {
    while (!tryHoldGate(slot)) std::this_thread::yield();
}

} // namespace

void ClientAdapter::sendEncryptedPacket(const Connection& conn, const uint8_t* inner,
                                        size_t innerLen) {
    sendSealed(conn.token, conn.sessionKey, *conn.hot, inner, innerLen);
}

void ClientAdapter::sendSealed(uint32_t token, const uint8_t* key, HotSlot& slot,
                               const uint8_t* inner, size_t innerLen) {
    holdGate(slot);
    sealAndSend(token, key, slot, inner, innerLen);
    releaseGate(token, key, slot);
}

void ClientAdapter::releaseGate(uint32_t token, const uint8_t* key, HotSlot& slot) {
    for (;;) {
        const uint64_t ack = slot.txDeferredAck.exchange(0, std::memory_order_seq_cst);
        if (ack != 0) {
            uint8_t inner[INNER_HEADER_SIZE + ACK_PAYLOAD_BYTES];
            inner[0] = (uint8_t)(MSG_HEARTBEAT_ACK >> 8);
            inner[1] = (uint8_t)(MSG_HEARTBEAT_ACK);
            inner[2] = 0;
            inner[3] = (uint8_t)ACK_PAYLOAD_BYTES;
            inner[4] = (uint8_t)(ack & 1u);
            inner[5] = (uint8_t)(ack >> 8);
            inner[6] = (uint8_t)(ack >> 24);
            inner[7] = (uint8_t)(ack >> 16);
            inner[8] = (uint8_t)(ack >> 40);
            inner[9] = (uint8_t)(ack >> 32);
            sealAndSend(token, key, slot, inner, sizeof(inner));
        }
        slot.txGate.store(0, std::memory_order_seq_cst);
        if (slot.txDeferredAck.load(std::memory_order_seq_cst) == 0) return;
        if (!tryHoldGate(slot)) return;
    }
}

void ClientAdapter::sealAndSend(uint32_t token, const uint8_t* key, HotSlot& slot,
                                const uint8_t* inner, size_t innerLen) {
    const uint16_t msgType = satellite::readBE16(inner);
    const SOCKET sock = sock_.load(std::memory_order_acquire);
    if (sock == INVALID_SOCKET) {
        satellite::g_wire.txUnroutable.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // The buffers below are sized for MAX_INNER_MESSAGE_BYTES exactly, and
    // encryptPacket has no length of its own to check against; an oversized
    // caller is a bug, and dropping the frame beats overrunning the stack.
    if (innerLen > static_cast<size_t>(MAX_INNER_MESSAGE_BYTES)) {
        satellite::g_wire.txOversize.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const uint64_t clientAddr = slot.clientAddr.load(std::memory_order_acquire);
    if (clientAddr == 0) {
        satellite::g_wire.txUnroutable.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(clientAddrPort(clientAddr));
    addr.sin_addr.s_addr = clientAddrIPv4(clientAddr);

    // Monotonic per-token counter in the nonce; the direction byte keeps this
    // direction's nonces disjoint from the client's under the shared session key.
    const uint32_t counter = slot.txCounter.fetch_add(1, std::memory_order_relaxed) + 1;

    // Sized so the whole datagram lands on UDP_DATAGRAM_MAX_BYTES: the control
    // messages are all under 30 bytes, but MSG_SPEAKER_AUDIO carries a 20 ms
    // Opus packet and the ceiling has to hold a VBR spike, not the average.
    uint8_t ct[MAX_INNER_MESSAGE_BYTES + AUTH_TAG_SIZE];
    unsigned long long ctLen = 0;
    if (!encryptPacket(key, CRYPTO_DIR_SERVER_TO_CLIENT, counter, token, inner, innerLen, ct,
                       &ctLen)) {
        satellite::g_wire.txEncryptFailed.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    uint8_t pkt[HEADER_SIZE + sizeof(ct)];
    static_assert(sizeof(pkt) == UDP_DATAGRAM_MAX_BYTES,
                  "the framed packet must land exactly on the datagram ceiling");
    uint32_t t = token;
    pkt[0] = (uint8_t)(t >> 24);
    pkt[1] = (uint8_t)(t >> 16);
    pkt[2] = (uint8_t)(t >> 8);
    pkt[3] = (uint8_t)(t);
    pkt[4] = (uint8_t)(counter >> 24);
    pkt[5] = (uint8_t)(counter >> 16);
    pkt[6] = (uint8_t)(counter >> 8);
    pkt[7] = (uint8_t)(counter);
    memcpy(pkt + HEADER_SIZE, ct, ctLen);

    const int sent = sendto(sock, reinterpret_cast<const char*>(pkt), (int)(HEADER_SIZE + ctLen), 0,
                            reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (sent == SOCKET_ERROR) {
        satellite::g_wire.txSendFailed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    satellite::g_wire.recordOutbound(msgType, static_cast<size_t>(sent));
}

void ClientAdapter::sendHeartbeatAck(const SessionEndpoint& to, bool backendAvailable,
                                     uint8_t totalActiveControllers, uint16_t epoch,
                                     uint16_t activeBitmap) {
    // Wire: backendAvailable(1) + totalActiveControllers(1) + epoch(u16 BE) +
    // bitmap(u16 BE) = 6 bytes. The epoch/bitmap pair makes involuntary
    // server-side topology loss self-healing within one heartbeat.
    HotSlot& slot = *to.slot;
    slot.txDeferredAck.store(packAck(backendAvailable, totalActiveControllers, epoch, activeBitmap),
                             std::memory_order_seq_cst);
    if (tryHoldGate(slot)) releaseGate(to.token, to.key, slot);
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

void ClientAdapter::sendTriggerEffects(const Connection& conn, uint8_t ctrlIdx,
                                       const TriggerEffectsReport& report) {
    // Wire: ctrlIdx(1) + left block(11) + right block(11) = 23 bytes, raw DS5
    // effect bytes forwarded verbatim.
    const uint16_t payloadLen = 1 + TRIGGER_EFFECTS_WIRE_PAYLOAD_BYTES;
    uint8_t inner[4 + 1 + TRIGGER_EFFECTS_WIRE_PAYLOAD_BYTES];
    inner[0] = (uint8_t)(MSG_TRIGGER_EFFECTS >> 8);
    inner[1] = (uint8_t)(MSG_TRIGGER_EFFECTS);
    inner[2] = (uint8_t)(payloadLen >> 8);
    inner[3] = (uint8_t)(payloadLen);
    inner[4] = ctrlIdx;
    std::memcpy(inner + 5, report.left, TRIGGER_EFFECT_BLOCK_BYTES);
    std::memcpy(inner + 5 + TRIGGER_EFFECT_BLOCK_BYTES, report.right, TRIGGER_EFFECT_BLOCK_BYTES);
    sendEncryptedPacket(conn, inner, sizeof(inner));
}

void ClientAdapter::sendPlayerLeds(const Connection& conn, uint8_t ctrlIdx, uint8_t ledMask) {
    // Wire: ctrlIdx(1) + ledMask(1) = 2 bytes.
    uint8_t inner[4 + 2];
    inner[0] = (uint8_t)(MSG_PLAYER_LEDS >> 8);
    inner[1] = (uint8_t)(MSG_PLAYER_LEDS);
    inner[2] = 0;
    inner[3] = 2;
    inner[4] = ctrlIdx;
    inner[5] = ledMask;
    sendEncryptedPacket(conn, inner, sizeof(inner));
}

void ClientAdapter::sendSpeakerAudio(const SessionEndpoint& to, uint8_t ctrlIdx, uint16_t seq,
                                     const uint8_t* opus, size_t opusLen) {
    sendAudioMessage(to, MSG_SPEAKER_AUDIO, ctrlIdx, seq, opus, opusLen);
}

void ClientAdapter::sendHapticAudio(const SessionEndpoint& to, uint8_t ctrlIdx, uint16_t seq,
                                    const uint8_t* opus, size_t opusLen) {
    sendAudioMessage(to, MSG_HAPTIC_AUDIO, ctrlIdx, seq, opus, opusLen);
}

void ClientAdapter::sendAudioMessage(const SessionEndpoint& to, uint16_t msgType, uint8_t ctrlIdx,
                                     uint16_t seq, const uint8_t* opus, size_t opusLen) {
    // Wire: ctrlIdx(1) + seq(u16 BE) + one Opus packet, the same shape for
    // every outbound audio lane. An empty packet would decode to nothing on
    // the far side, so it never leaves; an oversized one cannot be split (Opus
    // packets are atomic) and is dropped rather than truncated into a frame
    // the decoder would reject anyway.
    if (opusLen == 0) return;
    const size_t payloadLen = (size_t)AUDIO_WIRE_HEADER_BYTES + opusLen;
    if (payloadLen > (size_t)MAX_INNER_PAYLOAD_BYTES) return;

    uint8_t inner[INNER_HEADER_SIZE + MAX_INNER_PAYLOAD_BYTES];
    inner[0] = (uint8_t)(msgType >> 8);
    inner[1] = (uint8_t)(msgType);
    inner[2] = (uint8_t)(payloadLen >> 8);
    inner[3] = (uint8_t)(payloadLen);
    encodeAudioFrameHeader(inner + INNER_HEADER_SIZE, ctrlIdx, seq);
    std::memcpy(inner + INNER_HEADER_SIZE + AUDIO_WIRE_HEADER_BYTES, opus, opusLen);
    sendSealed(to.token, to.key, *to.slot, inner, INNER_HEADER_SIZE + payloadLen);
}

void ClientAdapter::sendMicLed(const Connection& conn, uint8_t ctrlIdx, uint8_t state) {
    // Wire: ctrlIdx(1) + state(1) = 2 bytes, state is MIC_LED_STATE_*.
    uint8_t inner[4 + 2];
    inner[0] = (uint8_t)(MSG_MIC_LED >> 8);
    inner[1] = (uint8_t)(MSG_MIC_LED);
    inner[2] = 0;
    inner[3] = 2;
    inner[4] = ctrlIdx;
    inner[5] = state;
    sendEncryptedPacket(conn, inner, sizeof(inner));
}
