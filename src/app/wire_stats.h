// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include "core/types.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace satellite {

struct RxCounts {
    uint64_t input = 0;
    uint64_t heartbeat = 0;
    uint64_t motion = 0;
    uint64_t battery = 0;
    uint64_t pointer = 0;
    uint64_t micAudio = 0;
    uint64_t malformed = 0;
    uint64_t unknownType = 0;
    uint64_t runt = 0;
    uint64_t unknownToken = 0;
};

struct TxCounts {
    uint64_t packets = 0;
    uint64_t bytes = 0;
    uint64_t heartbeatAck = 0;
    uint64_t rumble = 0;
    uint64_t lightbar = 0;
    uint64_t triggerEffects = 0;
    uint64_t playerLeds = 0;
    uint64_t speakerAudio = 0;
    uint64_t micLed = 0;
    uint64_t sessionClose = 0;
    uint64_t unroutable = 0;
    uint64_t encryptFailed = 0;
    uint64_t oversize = 0;
    uint64_t sendFailed = 0;
};

struct WireCounts {
    RxCounts rx;
    TxCounts tx;
    uint64_t authNotPaired = 0;
    uint64_t authBadProof = 0;
    uint64_t sessionsReaped = 0;
};

struct WireStats {
    std::atomic<uint64_t> rxHeartbeat{0};
    std::atomic<uint64_t> rxMotion{0};
    std::atomic<uint64_t> rxBattery{0};
    std::atomic<uint64_t> rxPointer{0};
    std::atomic<uint64_t> rxMicAudio{0};
    std::atomic<uint64_t> rxMalformed{0};
    std::atomic<uint64_t> rxUnknownType{0};
    std::atomic<uint64_t> rxRunt{0};
    std::atomic<uint64_t> rxUnknownToken{0};

    std::atomic<uint64_t> txPackets{0};
    std::atomic<uint64_t> txBytes{0};
    std::atomic<uint64_t> txHeartbeatAck{0};
    std::atomic<uint64_t> txRumble{0};
    std::atomic<uint64_t> txLightbar{0};
    std::atomic<uint64_t> txTriggerEffects{0};
    std::atomic<uint64_t> txPlayerLeds{0};
    std::atomic<uint64_t> txSpeakerAudio{0};
    std::atomic<uint64_t> txMicLed{0};
    std::atomic<uint64_t> txSessionClose{0};
    std::atomic<uint64_t> txUnroutable{0};
    std::atomic<uint64_t> txEncryptFailed{0};
    std::atomic<uint64_t> txOversize{0};
    std::atomic<uint64_t> txSendFailed{0};

    std::atomic<uint64_t> authNotPaired{0};
    std::atomic<uint64_t> authBadProof{0};
    std::atomic<uint64_t> sessionsReaped{0};
    std::atomic<uint64_t> peakLoopUs{0};

    static bool isDispatchedInboundType(uint16_t msgType) {
        switch (msgType) {
        case MSG_GAMEPAD_DATA:
        case MSG_HEARTBEAT_PING:
        case MSG_MOTION:
        case MSG_BATTERY:
        case MSG_TOUCHPAD:
        case MSG_MIC_AUDIO:
            return true;
        default:
            return false;
        }
    }

    void recordInbound(uint16_t msgType, bool handled) {
        if (!handled) {
            bump(isDispatchedInboundType(msgType) ? rxMalformed : rxUnknownType);
            return;
        }
        switch (msgType) {
        case MSG_HEARTBEAT_PING:
            bump(rxHeartbeat);
            break;
        case MSG_MOTION:
            bump(rxMotion);
            break;
        case MSG_BATTERY:
            bump(rxBattery);
            break;
        case MSG_TOUCHPAD:
            bump(rxPointer);
            break;
        case MSG_MIC_AUDIO:
            bump(rxMicAudio);
            break;
        default:
            break;
        }
    }

    void recordOutbound(uint16_t msgType, size_t datagramBytes) {
        bump(txPackets);
        txBytes.fetch_add(static_cast<uint64_t>(datagramBytes), std::memory_order_relaxed);
        switch (msgType) {
        case MSG_HEARTBEAT_ACK:
            bump(txHeartbeatAck);
            break;
        case MSG_RUMBLE:
            bump(txRumble);
            break;
        case MSG_LIGHTBAR:
            bump(txLightbar);
            break;
        case MSG_TRIGGER_EFFECTS:
            bump(txTriggerEffects);
            break;
        case MSG_PLAYER_LEDS:
            bump(txPlayerLeds);
            break;
        case MSG_SPEAKER_AUDIO:
            bump(txSpeakerAudio);
            break;
        case MSG_MIC_LED:
            bump(txMicLed);
            break;
        case MSG_SESSION_CLOSE:
            bump(txSessionClose);
            break;
        default:
            break;
        }
    }

    WireCounts snapshot() const {
        WireCounts c;
        c.rx.heartbeat = read(rxHeartbeat);
        c.rx.motion = read(rxMotion);
        c.rx.battery = read(rxBattery);
        c.rx.pointer = read(rxPointer);
        c.rx.micAudio = read(rxMicAudio);
        c.rx.malformed = read(rxMalformed);
        c.rx.unknownType = read(rxUnknownType);
        c.rx.runt = read(rxRunt);
        c.rx.unknownToken = read(rxUnknownToken);
        c.tx.packets = read(txPackets);
        c.tx.bytes = read(txBytes);
        c.tx.heartbeatAck = read(txHeartbeatAck);
        c.tx.rumble = read(txRumble);
        c.tx.lightbar = read(txLightbar);
        c.tx.triggerEffects = read(txTriggerEffects);
        c.tx.playerLeds = read(txPlayerLeds);
        c.tx.speakerAudio = read(txSpeakerAudio);
        c.tx.micLed = read(txMicLed);
        c.tx.sessionClose = read(txSessionClose);
        c.tx.unroutable = read(txUnroutable);
        c.tx.encryptFailed = read(txEncryptFailed);
        c.tx.oversize = read(txOversize);
        c.tx.sendFailed = read(txSendFailed);
        c.authNotPaired = read(authNotPaired);
        c.authBadProof = read(authBadProof);
        c.sessionsReaped = read(sessionsReaped);
        return c;
    }

    uint64_t observePeakLoopUs(uint64_t sampleUs) {
        uint64_t prev = peakLoopUs.load(std::memory_order_relaxed);
        while (sampleUs > prev &&
               !peakLoopUs.compare_exchange_weak(prev, sampleUs, std::memory_order_relaxed)) {}
        return sampleUs > prev ? sampleUs : prev;
    }

    template <typename Fn> void forEachCounter(Fn&& fn) {
        fn(rxHeartbeat);
        fn(rxMotion);
        fn(rxBattery);
        fn(rxPointer);
        fn(rxMicAudio);
        fn(rxMalformed);
        fn(rxUnknownType);
        fn(rxRunt);
        fn(rxUnknownToken);
        fn(txPackets);
        fn(txBytes);
        fn(txHeartbeatAck);
        fn(txRumble);
        fn(txLightbar);
        fn(txTriggerEffects);
        fn(txPlayerLeds);
        fn(txSpeakerAudio);
        fn(txMicLed);
        fn(txSessionClose);
        fn(txUnroutable);
        fn(txEncryptFailed);
        fn(txOversize);
        fn(txSendFailed);
        fn(authNotPaired);
        fn(authBadProof);
        fn(sessionsReaped);
        fn(peakLoopUs);
    }

    void reset() {
        forEachCounter([](std::atomic<uint64_t>& c) { c.store(0, std::memory_order_relaxed); });
    }

  private:
    static void bump(std::atomic<uint64_t>& c) { c.fetch_add(1, std::memory_order_relaxed); }
    static uint64_t read(const std::atomic<uint64_t>& c) {
        return c.load(std::memory_order_relaxed);
    }
};

inline WireStats g_wire;

} // namespace satellite
