// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * net/dsu_protocol.h — Pure encoders/decoders for the Cemuhook DSU protocol
 * (cemuhook.sshnuke.net/padudpserver.html).
 *
 * The satellite re-emits motion data from forwarded controllers on
 * udp://0.0.0.0:26760 (bind address configurable, defaults to 127.0.0.1 for
 * loopback-only access) so emulators that speak the DSU client protocol
 * (Cemu, Citra, Yuzu, Dolphin, Ryujinx, RPCS3, PCSX2) can subscribe to
 * gyro + accel from a remote DualSense / Joy-Con without any per-emulator
 * configuration beyond pointing DSUClient at the satellite host.
 *
 * This header is platform-free. All socket / thread plumbing lives in
 * dsu_server.h. Pure functions here are exercised by unit tests with the
 * canonical packet captures linked in the spec.
 */
#pragma once

#include "core/types.h"

#include <array>
#include <cstdint>
#include <cstring>

namespace dsu {

// ── Header constants ────────────────────────────────────────────────────────

// 4-byte magic for server → client packets ("DSUS"). Client → server is "DSUC".
inline constexpr std::array<uint8_t, 4> MAGIC_SERVER{'D', 'S', 'U', 'S'};
inline constexpr std::array<uint8_t, 4> MAGIC_CLIENT{'D', 'S', 'U', 'C'};

// Protocol version both sides advertise. 1001 is the de-facto current value
// (BetterJoy, SteamDeckGyroDSU, Cemu, Citra all speak 1001).
inline constexpr uint16_t PROTOCOL_VERSION = 1001;

// Default UDP port the spec calls out. Configurable on our side; emulators
// hard-default to this.
inline constexpr int DEFAULT_PORT = 26760;

// Up to 4 "pad slots" — the DSU spec hardcodes this. Senders with more than
// 4 forwarded controllers must pick which subset to advertise via the
// dish slot → DSU slot matrix (web UI in a follow-up PR; default mapping
// is the first 4 active controllers in index order).
inline constexpr int MAX_SLOTS = 4;

// ── Event types ─────────────────────────────────────────────────────────────

inline constexpr uint32_t EVENT_VERSION = 0x100000;
inline constexpr uint32_t EVENT_INFORMATION = 0x100001;
inline constexpr uint32_t EVENT_PAD_DATA = 0x100002;

// ── Slot state / connection / model / battery enums ─────────────────────────

inline constexpr uint8_t SLOT_STATE_DISCONNECTED = 0;
inline constexpr uint8_t SLOT_STATE_RESERVED = 1;
inline constexpr uint8_t SLOT_STATE_CONNECTED = 2;

inline constexpr uint8_t SLOT_MODEL_NONE = 0;
inline constexpr uint8_t SLOT_MODEL_PARTIAL = 1; // No gyro/accel
inline constexpr uint8_t SLOT_MODEL_FULL = 2;

inline constexpr uint8_t SLOT_CONNECTION_NONE = 0;
inline constexpr uint8_t SLOT_CONNECTION_USB = 1;
inline constexpr uint8_t SLOT_CONNECTION_BLUETOOTH = 2;

inline constexpr uint8_t SLOT_BATTERY_NONE = 0;
inline constexpr uint8_t SLOT_BATTERY_DYING = 1;
inline constexpr uint8_t SLOT_BATTERY_LOW = 2;
inline constexpr uint8_t SLOT_BATTERY_MEDIUM = 3;
inline constexpr uint8_t SLOT_BATTERY_HIGH = 4;
inline constexpr uint8_t SLOT_BATTERY_FULL = 5;
inline constexpr uint8_t SLOT_BATTERY_CHARGING = 0xEE;
inline constexpr uint8_t SLOT_BATTERY_CHARGED = 0xEF;

// ── CRC32 (poly 0xEDB88320, IEEE 802.3, init/final XOR 0xFFFFFFFF) ──────────
//
// The DSU spec computes CRC32 over the entire packet with the CRC field
// itself zeroed. Implementation matches the table-less variant used by
// BetterJoy + SteamDeckGyroDSU so packet captures stay byte-identical.
uint32_t crc32(const uint8_t* data, size_t len);

// ── Header layout ───────────────────────────────────────────────────────────
//
// All multi-byte integers in the DSU wire format are little-endian.
//
//   [0..3]   Magic        (4B) "DSUS" (server) or "DSUC" (client)
//   [4..5]   Version      (2B LE) = 1001
//   [6..7]   Length       (2B LE) = total packet size - 16  (size after the
//                                    16-byte header up to end of packet)
//   [8..11]  CRC32        (4B LE) computed with this field zeroed
//   [12..15] ID           (4B LE) server ID (or client ID for DSUC)
//
// EVENT_TYPE (4B LE) follows the header and is logically part of the payload.
inline constexpr size_t HEADER_SIZE = 16;

// ── Pad Data response payload size ──────────────────────────────────────────
//
//   [0]      slot index (1)
//   [1]      slot state (1)
//   [2]      slot model (1)
//   [3]      slot connection type (1)
//   [4..9]   mac address (6)
//   [10]     battery (1)
//   [11]     connected? (1)
//   [12..15] packet number (4 LE)
//   [16..17] buttons (2)
//   [18]     PS button (1)
//   [19]     touch button (1)
//   [20..23] left stick X / Y (1+1+1+1 — LX, LY, RX, RY)
//   [24..35] analog buttons (12)
//   [36..41] touchpad 1: active(1) + id(1) + x(2 LE) + y(2 LE)
//   [42..47] touchpad 2: same shape
//   [48..55] timestamp microseconds (8 LE)
//   [56..67] accel X/Y/Z (3× float32 LE) — g units
//   [68..79] gyro pitch/yaw/roll (3× float32 LE) — deg/s units
//
// = 80 bytes total following the EVENT_TYPE.
inline constexpr size_t PAD_DATA_PAYLOAD_SIZE = 80;

// Total wire size of a Pad Data packet (header + event type + payload).
inline constexpr size_t PAD_DATA_PACKET_SIZE = HEADER_SIZE + 4 + PAD_DATA_PAYLOAD_SIZE;

// ── Encoders ────────────────────────────────────────────────────────────────

// Build the 16-byte DSUS header in-place. `serverId` is the satellite's
// stable random identifier (4 bytes). `totalLen` is the *total* packet size
// — the encoder writes `totalLen - 16` into the length field as the spec
// requires.
//
// CRC is left zero; call finaliseCrc on the completed packet to stamp it.
void encodeServerHeader(uint8_t* out, uint16_t totalLen, uint32_t serverId);

// Recompute and write the CRC32 over a fully-built packet. Caller must have
// pre-zeroed out[8..11]; this helper writes the final value back.
void finaliseCrc(uint8_t* packet, size_t len);

// Build a Version response (event 0x100000) into `out`. Total bytes written
// returned. Buffer must be at least 26 bytes.
//
// Layout: [16B header] [4B event=0x100000] [2B version=1001] [4B padding 0]
//
// The trailing 4B of padding makes Cemu happy; some clients require the
// payload size be a multiple of 4. Caller is expected to finalise the CRC.
size_t encodeVersionResponse(uint8_t* out, size_t outCap, uint32_t serverId);

// Build an Information response (event 0x100001) for `slotIndex` (0..3).
// `connected` indicates whether the slot has a live controller; the rest
// of the fields are best-effort defaults (DualShock 4 / Bluetooth / full
// model when connected; disconnected/none/none/none otherwise).
//
// Layout (after header): [4B event] [12B slot info]
//   slot info:
//     [0]   slot index
//     [1]   slot state
//     [2]   slot model
//     [3]   connection type
//     [4..9] mac (6B)
//     [10]  battery
//     [11]  0 (terminator)
size_t encodeInformationResponse(uint8_t* out, size_t outCap, uint32_t serverId,
                                 uint8_t slotIndex, bool connected,
                                 const std::array<uint8_t, 6>& mac);

// Build a Pad Data response (event 0x100002) for `slotIndex` from a
// MotionReport. The non-motion fields (buttons, sticks, touchpad, battery,
// timestamp) are filled in by the caller via the `PadDataInputs` struct so
// this encoder stays pure. Caller is expected to finalise the CRC.
//
// `accelToG` and `gyroToDegPerSec` are the scale conversions applied to the
// MotionReport's int16 fixed-point values — they come from core/types.h
// (MOTION_ACCEL_SCALE_G / MOTION_GYRO_SCALE_DEG_S).
struct PadDataInputs {
    uint8_t slotIndex = 0;
    bool connected = false;
    std::array<uint8_t, 6> mac{};
    uint8_t battery = SLOT_BATTERY_NONE;
    uint32_t packetNumber = 0;
    uint16_t buttons = 0;
    uint8_t psButton = 0;
    uint8_t touchButton = 0;
    uint8_t stickLX = 128, stickLY = 128, stickRX = 128, stickRY = 128;
    uint8_t analogButtons[12] = {};
    uint64_t timestampMicros = 0;
    // Motion in DSU's native units (float32 g / deg/s).
    float accelGX = 0.0f, accelGY = 0.0f, accelGZ = 0.0f;
    float gyroPitch = 0.0f, gyroYaw = 0.0f, gyroRoll = 0.0f;
};

size_t encodePadDataResponse(uint8_t* out, size_t outCap, uint32_t serverId,
                             const PadDataInputs& inputs);

// Convert a MotionReport (int16 fixed-point) into the float fields the DSU
// Pad Data response expects. Pure helper — separated so callers that have a
// MotionReport in hand don't need to reinvent the scale conversion.
void applyMotionReport(PadDataInputs& out, const MotionReport& report);

// ── Client request parsers ──────────────────────────────────────────────────

// Validate the incoming packet's magic, version, length, and CRC. Returns
// the event type (0 on validation failure — caller treats as a drop).
uint32_t parseClientHeader(const uint8_t* data, size_t len);

// For EVENT_PAD_DATA subscriptions the client appends 8 bytes of subscription
// flags. Returns the slot index the client wants, or 0xFF if it wants all.
struct SubscriptionRequest {
    bool wantAllSlots = false;
    uint8_t slotIndex = 0; // valid only when !wantAllSlots
    std::array<uint8_t, 6> macFilter{}; // 00:00:00:00:00:00 = no MAC filter
};

// Parse the 8-byte tail of an EVENT_PAD_DATA request. Returns false on
// malformed payload. The flags byte at offset 0 tells the server how to
// interpret the rest: bit 0 = match by slot, bit 1 = match by MAC.
bool parseSubscriptionRequest(const uint8_t* data, size_t len, SubscriptionRequest& out);

} // namespace dsu
