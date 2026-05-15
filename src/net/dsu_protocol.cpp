// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

#include "dsu_protocol.h"

#include <cstring>

namespace dsu {

namespace {

inline void putLE16(uint8_t* dst, uint16_t v) {
    dst[0] = static_cast<uint8_t>(v);
    dst[1] = static_cast<uint8_t>(v >> 8);
}

inline void putLE32(uint8_t* dst, uint32_t v) {
    dst[0] = static_cast<uint8_t>(v);
    dst[1] = static_cast<uint8_t>(v >> 8);
    dst[2] = static_cast<uint8_t>(v >> 16);
    dst[3] = static_cast<uint8_t>(v >> 24);
}

inline void putLE64(uint8_t* dst, uint64_t v) {
    for (int i = 0; i < 8; ++i) { dst[i] = static_cast<uint8_t>(v >> (8 * i)); }
}

inline void putFloatLE(uint8_t* dst, float f) {
    // float and uint32 share endianness on every supported host; on a
    // big-endian host we'd byte-swap the uint32. None of our supported
    // platforms is BE so a memcpy + LE-as-bytes write is correct.
    uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    putLE32(dst, bits);
}

inline uint16_t readLE16(const uint8_t* src) {
    return static_cast<uint16_t>(src[0]) | (static_cast<uint16_t>(src[1]) << 8);
}

inline uint32_t readLE32(const uint8_t* src) {
    return static_cast<uint32_t>(src[0]) | (static_cast<uint32_t>(src[1]) << 8) |
           (static_cast<uint32_t>(src[2]) << 16) | (static_cast<uint32_t>(src[3]) << 24);
}

} // namespace

// ── CRC32 ───────────────────────────────────────────────────────────────────
// Standard IEEE 802.3 CRC32. Table-less loop is fine since DSU packets are
// small (max ~100 bytes) and the server emits at most a few hundred per
// second across all subscribed clients.

uint32_t crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            const uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

// ── Header / CRC helpers ────────────────────────────────────────────────────

void encodeServerHeader(uint8_t* out, uint16_t totalLen, uint32_t serverId) {
    std::memcpy(out, MAGIC_SERVER.data(), 4);
    putLE16(out + 4, PROTOCOL_VERSION);
    // The DSU "length" field counts bytes *after* the length field itself
    // through end of packet — i.e. 4B CRC + 4B serverId + everything else.
    // That equals (total - 8) bytes by construction. Cemu / SteamDeckGyroDSU
    // verify this; some older clients accept (total - 16). We follow the spec.
    putLE16(out + 6, static_cast<uint16_t>(totalLen - 8));
    // CRC zeroed; caller finalises after writing the rest.
    out[8] = 0;
    out[9] = 0;
    out[10] = 0;
    out[11] = 0;
    putLE32(out + 12, serverId);
}

void finaliseCrc(uint8_t* packet, size_t len) {
    // Ensure CRC field is zero before computing — common bug otherwise.
    packet[8] = 0;
    packet[9] = 0;
    packet[10] = 0;
    packet[11] = 0;
    const uint32_t c = crc32(packet, len);
    putLE32(packet + 8, c);
}

// ── Version response ────────────────────────────────────────────────────────

size_t encodeVersionResponse(uint8_t* out, size_t outCap, uint32_t serverId) {
    constexpr size_t total = HEADER_SIZE + 4 + 4; // header + event + (2B ver + 2B pad)
    if (outCap < total) return 0;
    encodeServerHeader(out, total, serverId);
    putLE32(out + 16, EVENT_VERSION);
    putLE16(out + 20, PROTOCOL_VERSION);
    out[22] = 0; // padding
    out[23] = 0;
    finaliseCrc(out, total);
    return total;
}

// ── Information response ────────────────────────────────────────────────────

size_t encodeInformationResponse(uint8_t* out, size_t outCap, uint32_t serverId,
                                 uint8_t slotIndex, bool connected,
                                 const std::array<uint8_t, 6>& mac) {
    constexpr size_t total = HEADER_SIZE + 4 + 12;
    if (outCap < total) return 0;
    encodeServerHeader(out, total, serverId);
    putLE32(out + 16, EVENT_INFORMATION);
    out[20] = slotIndex;
    out[21] = connected ? SLOT_STATE_CONNECTED : SLOT_STATE_DISCONNECTED;
    out[22] = connected ? SLOT_MODEL_FULL : SLOT_MODEL_NONE;
    out[23] = connected ? SLOT_CONNECTION_BLUETOOTH : SLOT_CONNECTION_NONE;
    std::memcpy(out + 24, mac.data(), 6);
    out[30] = connected ? SLOT_BATTERY_FULL : SLOT_BATTERY_NONE;
    out[31] = 0; // terminator byte per spec
    finaliseCrc(out, total);
    return total;
}

// ── Pad Data response ───────────────────────────────────────────────────────

size_t encodePadDataResponse(uint8_t* out, size_t outCap, uint32_t serverId,
                             const PadDataInputs& inputs) {
    if (outCap < PAD_DATA_PACKET_SIZE) return 0;
    encodeServerHeader(out, static_cast<uint16_t>(PAD_DATA_PACKET_SIZE), serverId);
    putLE32(out + 16, EVENT_PAD_DATA);
    uint8_t* p = out + 20;

    p[0] = inputs.slotIndex;
    p[1] = inputs.connected ? SLOT_STATE_CONNECTED : SLOT_STATE_DISCONNECTED;
    p[2] = inputs.connected ? SLOT_MODEL_FULL : SLOT_MODEL_NONE;
    p[3] = inputs.connected ? SLOT_CONNECTION_BLUETOOTH : SLOT_CONNECTION_NONE;
    std::memcpy(p + 4, inputs.mac.data(), 6);
    p[10] = inputs.battery;
    p[11] = inputs.connected ? 1 : 0;
    putLE32(p + 12, inputs.packetNumber);
    putLE16(p + 16, inputs.buttons);
    p[18] = inputs.psButton;
    p[19] = inputs.touchButton;
    p[20] = inputs.stickLX;
    p[21] = inputs.stickLY;
    p[22] = inputs.stickRX;
    p[23] = inputs.stickRY;
    std::memcpy(p + 24, inputs.analogButtons, 12);
    // Touchpad 1 (offset 36..41) — we don't have touchpad data here yet (Task
    // 1.3's MSG_TOUCHPAD lands in a separate roadmap slice). Zeros = inactive.
    std::memset(p + 36, 0, 6);
    // Touchpad 2 (offset 42..47)
    std::memset(p + 42, 0, 6);
    // Timestamp (8B LE microseconds)
    putLE64(p + 48, inputs.timestampMicros);
    // Accelerometer (3× float LE g)
    putFloatLE(p + 56, inputs.accelGX);
    putFloatLE(p + 60, inputs.accelGY);
    putFloatLE(p + 64, inputs.accelGZ);
    // Gyroscope (3× float LE deg/s)
    putFloatLE(p + 68, inputs.gyroPitch);
    putFloatLE(p + 72, inputs.gyroYaw);
    putFloatLE(p + 76, inputs.gyroRoll);

    finaliseCrc(out, PAD_DATA_PACKET_SIZE);
    return PAD_DATA_PACKET_SIZE;
}

void applyMotionReport(PadDataInputs& out, const MotionReport& report) {
    // MOTION_*_SCALE constants live in core/types.h. The cast to float
    // honours the saturation guarantees the comment there describes.
    out.gyroPitch = static_cast<float>(report.gyroX) * MOTION_GYRO_SCALE_DEG_S;
    out.gyroYaw = static_cast<float>(report.gyroY) * MOTION_GYRO_SCALE_DEG_S;
    out.gyroRoll = static_cast<float>(report.gyroZ) * MOTION_GYRO_SCALE_DEG_S;
    out.accelGX = static_cast<float>(report.accelX) * MOTION_ACCEL_SCALE_G;
    out.accelGY = static_cast<float>(report.accelY) * MOTION_ACCEL_SCALE_G;
    out.accelGZ = static_cast<float>(report.accelZ) * MOTION_ACCEL_SCALE_G;
}

// ── Client request parsing ──────────────────────────────────────────────────

uint32_t parseClientHeader(const uint8_t* data, size_t len) {
    if (len < HEADER_SIZE + 4) return 0;
    // Magic
    if (std::memcmp(data, MAGIC_CLIENT.data(), 4) != 0) return 0;
    // Version: be permissive — DSU clients in the wild send 1001 or 1002
    // and the differences don't affect the requests we parse. Reject zero
    // to catch pathological / wrong-protocol packets.
    if (readLE16(data + 4) == 0) return 0;
    // Length field is total - 8; verify it matches what we received so a
    // truncated UDP datagram doesn't slip through.
    const uint16_t advertised = readLE16(data + 6);
    if (advertised + 8u != len) return 0;
    // CRC check — we recompute with the CRC field zeroed. The DSU spec
    // makes this mandatory; some toy clients skip it, in which case we
    // drop. Real Cemu / Citra / Yuzu all compute correctly.
    const uint32_t advertisedCrc = readLE32(data + 8);
    uint8_t scratch[256];
    if (len > sizeof(scratch)) return 0;
    std::memcpy(scratch, data, len);
    scratch[8] = scratch[9] = scratch[10] = scratch[11] = 0;
    if (crc32(scratch, len) != advertisedCrc) return 0;

    return readLE32(data + 16); // event type
}

bool parseSubscriptionRequest(const uint8_t* data, size_t len, SubscriptionRequest& out) {
    // The pad-data subscription appends [4B flags] [1B slot] [6B MAC] = 11B
    // *after* the 20-byte (header + event type) prefix. Tolerate the older
    // 8-byte tail variant (4B flags + 1B slot + 6B MAC truncated) by clamping.
    if (len < HEADER_SIZE + 4 + 5) return false;
    const uint8_t* p = data + HEADER_SIZE + 4;
    const size_t tailLen = len - (HEADER_SIZE + 4);
    const uint32_t flags = readLE32(p);

    // Spec: bit 0 = match all (subscribe to every slot); bit 1 = match by slot;
    // bit 2 = match by MAC. Real clients almost always send "match all" (0)
    // or "match slot" (0x01 in the byte-flags variant some impls use). We
    // accept both: if flags has bit 0 set OR equals 0, treat as wantAll.
    out.wantAllSlots = (flags == 0) || ((flags & 0x01) != 0);
    out.slotIndex = (tailLen > 4) ? p[4] : 0;
    if (tailLen >= 11) {
        std::memcpy(out.macFilter.data(), p + 5, 6);
    } else {
        out.macFilter.fill(0);
    }
    return true;
}

} // namespace dsu
