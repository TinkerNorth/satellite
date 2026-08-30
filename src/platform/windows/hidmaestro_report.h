// SPDX-License-Identifier: LGPL-3.0-or-later

// Pure per-profile report packers + output decoders for the HIDMaestro
// backend: XUSB-shaped wire input (GamepadReport et al) in, byte-exact
// HIDMaestro Data[]/GipData payloads out, and ring OutputPacket bytes back to
// RumbleReport/lightbar. OS-free (same pure-codec/IO-shell doctrine as
// vigem_submit_policy.h and core/ds4_report.h) so every packer is verified on
// every CI platform; the I/O shell is hidmaestro_adapter.cpp.
//
// Byte layouts are pinned to HIDMaestro v1.7.0's shipped profiles:
// xbox-360-wired (18-byte no-RID report + the 14-byte GIP slice),
// dualshock-4-v2 (DS4 v2 USB report 0x01 — identical to core/ds4_report.h),
// dualsense (DS5 USB report 0x01), and switch-pro (the 48-byte 0x30 body the
// driver's protocol responder streams).
#pragma once

#include "hidmaestro_wire.h"

#include "core/ds4_report.h"
#include "core/types.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace satellite {
namespace hidmaestro {

// Profile ids from HIDMaestro's embedded catalog, one per materializable
// identity. nullptr = identity not mapped (never the case today).
inline const char* profileForIdentity(GamepadIdentity identity) {
    switch (identity) {
    case GamepadIdentity::Xbox:
        return "xbox-360-wired";
    case GamepadIdentity::DS4:
        return "dualshock-4-v2";
    case GamepadIdentity::DualSense:
        return "dualsense";
    case GamepadIdentity::SwitchPro:
        return "switch-pro";
    }
    return nullptr;
}

// ── xbox-360-wired ─────────────────────────────────────────────────────────
//
// The profile's descriptor declares no Report ID, so Data[] carries the full
// 18-byte report. Sticks are u16 with the SDK's convention (X: signed+32768;
// Y: 32767-signed — u16 0 is stick-up on both the HID and GIP slices), the
// combined DirectInput Z axis carries (RT-LT) around 32768, and Vx/Vy are the
// separate-trigger velocity usages.

inline constexpr size_t X360_REPORT_BYTES = 18;

inline uint16_t x360StickX(int16_t v) { return static_cast<uint16_t>(static_cast<int>(v) + 32768); }
inline uint16_t x360StickY(int16_t v) { return static_cast<uint16_t>(32767 - static_cast<int>(v)); }

// XUSB dpad bits -> the shared GIP/HID hat encoding: 1=N .. 8=NW, 0=released.
// Branch ordering mirrors ds4HatFromButtons so contradictory bit combinations
// resolve to the same direction on every backend.
inline uint8_t x360HatFromButtons(uint16_t wButtons) {
    const bool up = (wButtons & 0x0001) != 0;
    const bool down = (wButtons & 0x0002) != 0;
    const bool left = (wButtons & 0x0004) != 0;
    const bool right = (wButtons & 0x0008) != 0;
    if (up && right) return 2;
    if (up && left) return 8;
    if (down && right) return 4;
    if (down && left) return 6;
    if (up) return 1;
    if (down) return 5;
    if (left) return 7;
    if (right) return 3;
    return 0;
}

inline void packX360Report(const GamepadReport& pad, uint8_t out[X360_REPORT_BYTES]) {
    std::memset(out, 0, X360_REPORT_BYTES);
    auto le16 = [&out](int off, uint16_t v) {
        out[off] = static_cast<uint8_t>(v & 0xFF);
        out[off + 1] = static_cast<uint8_t>(v >> 8);
    };
    le16(0, x360StickX(pad.sThumbLX));
    le16(2, x360StickY(pad.sThumbLY));
    le16(4, x360StickX(pad.sThumbRX));
    le16(6, x360StickY(pad.sThumbRY));
    // Combined DirectInput Z: 0.5 + (RT-LT)/2 over the u16 range; 257 widens a
    // trigger byte to u16 (255*257 = 65535), halved into the centred axis.
    const int combined =
        32768 +
        ((static_cast<int>(pad.bRightTrigger) - static_cast<int>(pad.bLeftTrigger)) * 257) / 2;
    le16(8, static_cast<uint16_t>(combined < 0 ? 0 : (combined > 65535 ? 65535 : combined)));
    le16(10, static_cast<uint16_t>(pad.bLeftTrigger * 257));
    le16(12, static_cast<uint16_t>(pad.bRightTrigger * 257));

    const uint16_t b = pad.wButtons;
    uint8_t btns = 0;
    if (b & 0x1000) btns |= 0x01; // A
    if (b & 0x2000) btns |= 0x02; // B
    if (b & 0x4000) btns |= 0x04; // X
    if (b & 0x8000) btns |= 0x08; // Y
    if (b & 0x0100) btns |= 0x10; // LB
    if (b & 0x0200) btns |= 0x20; // RB
    if (b & 0x0020) btns |= 0x40; // Back
    if (b & 0x0010) btns |= 0x80; // Start
    out[14] = btns;
    uint8_t high = 0;
    if (b & 0x0040) high |= 0x01; // LS
    if (b & 0x0080) high |= 0x02; // RS
    high |= static_cast<uint8_t>((x360HatFromButtons(b) & 0x0F) << 2);
    out[15] = high;
}

// The 14-byte XUSB-companion slice the HMXInput.dll reads on GET_STATE.
// Trigger range is 10-bit; Guide rides btnHigh bit 6 (its only path — the HID
// report has no Guide usage).
inline void packGip(const GamepadReport& pad, uint8_t out[INPUT_GIP_LENGTH]) {
    auto le16 = [&out](int off, uint16_t v) {
        out[off] = static_cast<uint8_t>(v & 0xFF);
        out[off + 1] = static_cast<uint8_t>(v >> 8);
    };
    le16(0, x360StickX(pad.sThumbLX));
    le16(2, x360StickY(pad.sThumbLY));
    le16(4, x360StickX(pad.sThumbRX));
    le16(6, x360StickY(pad.sThumbRY));
    le16(8, static_cast<uint16_t>((pad.bLeftTrigger * 1023) / 255));
    le16(10, static_cast<uint16_t>((pad.bRightTrigger * 1023) / 255));

    const uint16_t b = pad.wButtons;
    uint8_t low = 0;
    if (b & 0x1000) low |= 0x01; // A
    if (b & 0x2000) low |= 0x02; // B
    if (b & 0x4000) low |= 0x04; // X
    if (b & 0x8000) low |= 0x08; // Y
    if (b & 0x0100) low |= 0x10; // LB
    if (b & 0x0200) low |= 0x20; // RB
    if (b & 0x0040) low |= 0x40; // LS
    if (b & 0x0080) low |= 0x80; // RS
    out[12] = low;
    uint8_t high = 0;
    if (b & 0x0020) high |= 0x01; // Back
    if (b & 0x0010) high |= 0x02; // Start
    high |= static_cast<uint8_t>((x360HatFromButtons(b) & 0x0F) << 2);
    if (b & 0x0400) high |= 0x40; // Guide
    out[13] = high;
}

// ── Sony IMU scale ─────────────────────────────────────────────────────────
//
// The HIDMaestro driver serves a neutral Sony calibration stub whose divisor
// math lands consumers on 20 LSB per deg/s and 10000 LSB per g. From the wire
// MotionReport LSBs (2000/32767 deg/s, 4/32767 g) both conversions reduce to
// the same x40000/32767 — saturated, so full-scale wire motion clips at
// ~1638 deg/s / ~3.28 g instead of wrapping.
inline int16_t sonyImuFromWire(int16_t v) {
    const int scaled = (static_cast<int>(v) * 40000) / 32767;
    if (scaled > 32767) return 32767;
    if (scaled < -32768) return -32768;
    return static_cast<int16_t>(scaled);
}

inline MotionReport sonyMotionFromWire(const MotionReport& m) {
    MotionReport out = m;
    out.gyroX = sonyImuFromWire(m.gyroX);
    out.gyroY = sonyImuFromWire(m.gyroY);
    out.gyroZ = sonyImuFromWire(m.gyroZ);
    out.accelX = sonyImuFromWire(m.accelX);
    out.accelY = sonyImuFromWire(m.accelY);
    out.accelZ = sonyImuFromWire(m.accelZ);
    return out;
}

// ── dualshock-4-v2 ─────────────────────────────────────────────────────────
//
// Same DS4 v2 USB report 0x01 the macOS backend synthesizes; the driver
// prepends the RID, so Data[] carries the 63 payload bytes. Motion is
// rescaled from the wire's identity-calibration convention to the driver's
// stub-calibration units (core/ds4_report.h packs verbatim because macOS
// serves its own identity calibration blob).

inline constexpr size_t DS4_PAYLOAD_BYTES = 63;

inline void packDs4Payload(const Ds4InputState& st, uint8_t out[DS4_PAYLOAD_BYTES]) {
    Ds4InputState scaled = st;
    scaled.motion = sonyMotionFromWire(st.motion);
    uint8_t full[DS4V2_INPUT_REPORT_BYTES];
    ds4PackInputReport(scaled, full);
    std::memcpy(out, full + 1, DS4_PAYLOAD_BYTES);
}

// ── dualsense ──────────────────────────────────────────────────────────────
//
// DS5 USB report 0x01 (RID stripped -> 63 payload bytes). Consumers key the
// vendor-region offsets on VID/PID exactly as with real hardware, so gyro,
// accel, touch, and battery live at the real DualSense byte positions.

inline constexpr size_t DS5_PAYLOAD_BYTES = 63;

// DS5 battery byte (full-report byte 53): low nibble = level in tenths, high
// nibble = status (0 discharging, 1 charging, 2 full).
inline uint8_t ds5BatteryByte(const BatteryReport& report) {
    int nibble = (report.level == BATTERY_LEVEL_UNKNOWN) ? 5 : static_cast<int>(report.level) / 10;
    if (nibble > 10) nibble = 10;
    switch (report.status) {
    case BATTERY_STATUS_CHARGING:
        return static_cast<uint8_t>(0x10 | nibble);
    case BATTERY_STATUS_FULL:
    case BATTERY_STATUS_WIRED:
        return static_cast<uint8_t>(0x20 | 10);
    default:
        return static_cast<uint8_t>(nibble);
    }
}

struct Ds5InputState {
    GamepadReport pad{};
    MotionReport motion{};           // wire scale; rescaled during packing
    uint8_t batteryByte = 0x20 | 10; // full/wired until a battery stream arrives
    TouchpadFinger finger0{};
    TouchpadFinger finger1{};
    uint8_t touchTrackingId0 = 0;
    uint8_t touchTrackingId1 = 0;
    bool touchpadButtonPressed = false;
    uint8_t seq = 0;              // full-report byte 7, bumped per packed report
    uint32_t sensorTimestamp = 0; // 3 MHz units (0.333 us), free-running
};

inline void packDs5Payload(const Ds5InputState& st, uint8_t out[DS5_PAYLOAD_BYTES]) {
    std::memset(out, 0, DS5_PAYLOAD_BYTES);

    out[0] = ds4StickByte(st.pad.sThumbLX);
    out[1] = ds4StickByteInverted(st.pad.sThumbLY);
    out[2] = ds4StickByte(st.pad.sThumbRX);
    out[3] = ds4StickByteInverted(st.pad.sThumbRY);
    out[4] = st.pad.bLeftTrigger;
    out[5] = st.pad.bRightTrigger;
    out[6] = st.seq;

    const uint16_t b = st.pad.wButtons;
    uint8_t face = ds4HatFromButtons(b); // shared 0..7 + 8-released encoding
    if (b & 0x4000) face |= 0x10;        // X     -> Square
    if (b & 0x1000) face |= 0x20;        // A     -> Cross
    if (b & 0x2000) face |= 0x40;        // B     -> Circle
    if (b & 0x8000) face |= 0x80;        // Y     -> Triangle
    out[7] = face;

    uint8_t mid = 0;
    if (b & 0x0100) mid |= 0x01;               // LB -> L1
    if (b & 0x0200) mid |= 0x02;               // RB -> R1
    if (st.pad.bLeftTrigger > 0) mid |= 0x04;  // L2 digital
    if (st.pad.bRightTrigger > 0) mid |= 0x08; // R2 digital
    if (b & 0x0020) mid |= 0x10;               // Back  -> Create
    if (b & 0x0010) mid |= 0x20;               // Start -> Options
    if (b & 0x0040) mid |= 0x40;               // LS -> L3
    if (b & 0x0080) mid |= 0x80;               // RS -> R3
    out[8] = mid;

    uint8_t meta = 0;
    if (b & 0x0400) meta |= 0x01; // Guide -> PS
    if (st.touchpadButtonPressed) meta |= 0x02;
    out[9] = meta;

    const MotionReport motion = sonyMotionFromWire(st.motion);
    auto le16 = [&out](int off, int16_t v) {
        out[off] = static_cast<uint8_t>(static_cast<uint16_t>(v) & 0xFF);
        out[off + 1] = static_cast<uint8_t>(static_cast<uint16_t>(v) >> 8);
    };
    le16(15, motion.gyroX);
    le16(17, motion.gyroY);
    le16(19, motion.gyroZ);
    le16(21, motion.accelX);
    le16(23, motion.accelY);
    le16(25, motion.accelZ);
    out[27] = static_cast<uint8_t>(st.sensorTimestamp & 0xFF);
    out[28] = static_cast<uint8_t>((st.sensorTimestamp >> 8) & 0xFF);
    out[29] = static_cast<uint8_t>((st.sensorTimestamp >> 16) & 0xFF);
    out[30] = static_cast<uint8_t>((st.sensorTimestamp >> 24) & 0xFF);

    const auto f0 = ds4PackTouchFinger(st.finger0, st.touchTrackingId0);
    const auto f1 = ds4PackTouchFinger(st.finger1, st.touchTrackingId1);
    std::memcpy(out + 32, f0.data(), 4);
    std::memcpy(out + 36, f1.data(), 4);

    out[52] = st.batteryByte;
}

// ── switch-pro ─────────────────────────────────────────────────────────────
//
// The 48-byte 0x30 body the driver's protocol responder streams at ~60 Hz
// (it stamps timer/battery/vibrator bytes itself). Buttons follow SDL's
// Nintendo mapping — A/B and X/Y swapped positionally, identical to the
// Linux adapter's submitSwitchLocked — sticks are 12-bit packed around
// centre 0x800 with the factory-calibration half-range 0x600, and the IMU
// rides three copies of one sample in the Switch sensor frame with the
// fabricated-SPI calibration scales (4096 per g, 13371/936 per deg/s).

inline constexpr size_t SWITCH_BODY_BYTES = 48;
inline constexpr uint8_t SWITCH_TRIGGER_ON_HM = 30; // ZL/ZR digital threshold

inline uint16_t switchStickRaw(int16_t v) {
    const int raw = 0x800 + (static_cast<int>(v) * 0x600) / 32767;
    if (raw < 0) return 0;
    if (raw > 0xFFF) return 0xFFF;
    return static_cast<uint16_t>(raw);
}

inline void switchPackStick(uint8_t* dst, uint16_t x, uint16_t y) {
    dst[0] = static_cast<uint8_t>(x & 0xFF);
    dst[1] = static_cast<uint8_t>(((x >> 8) & 0x0F) | ((y & 0x0F) << 4));
    dst[2] = static_cast<uint8_t>(y >> 4);
}

inline int16_t switchImuRaw(float value, float scale) {
    const float scaled = std::round(value * scale);
    if (scaled > 32767.0f) return 32767;
    if (scaled < -32768.0f) return -32768;
    return static_cast<int16_t>(scaled);
}

inline void packSwitchBody(const GamepadReport& pad, const MotionReport& motion,
                           uint8_t out[SWITCH_BODY_BYTES]) {
    std::memset(out, 0, SWITCH_BODY_BYTES);

    const uint16_t b = pad.wButtons;
    uint8_t right = 0;
    if (b & 0x4000) right |= 0x01;                               // X (west)  -> Y
    if (b & 0x8000) right |= 0x02;                               // Y (north) -> X
    if (b & 0x1000) right |= 0x04;                               // A (south) -> B
    if (b & 0x2000) right |= 0x08;                               // B (east)  -> A
    if (b & 0x0200) right |= 0x40;                               // RB -> R
    if (pad.bRightTrigger > SWITCH_TRIGGER_ON_HM) right |= 0x80; // ZR
    out[2] = right;

    uint8_t shared = 0;
    if (b & 0x0020) shared |= 0x01; // Back  -> Minus
    if (b & 0x0010) shared |= 0x02; // Start -> Plus
    if (b & 0x0080) shared |= 0x04; // RS -> right stick click
    if (b & 0x0040) shared |= 0x08; // LS -> left stick click
    if (b & 0x0400) shared |= 0x10; // Guide -> Home
    out[3] = shared;

    uint8_t left = 0;
    if (b & 0x0002) left |= 0x01;                              // dpad down
    if (b & 0x0001) left |= 0x02;                              // dpad up
    if (b & 0x0008) left |= 0x04;                              // dpad right
    if (b & 0x0004) left |= 0x08;                              // dpad left
    if (b & 0x0100) left |= 0x40;                              // LB -> L
    if (pad.bLeftTrigger > SWITCH_TRIGGER_ON_HM) left |= 0x80; // ZL
    out[4] = left;

    switchPackStick(out + 5, switchStickRaw(pad.sThumbLX), switchStickRaw(pad.sThumbLY));
    switchPackStick(out + 8, switchStickRaw(pad.sThumbRX), switchStickRaw(pad.sThumbRY));

    // Wire MotionReport (DualSense/SDL frame: +X right, +Y up, +Z toward the
    // player) -> Switch wire frame, the exact inverse of the permutation SDL
    // applies when reading a real Pro Controller.
    const float gX = motion.accelX * MOTION_ACCEL_SCALE_G;
    const float gY = motion.accelY * MOTION_ACCEL_SCALE_G;
    const float gZ = motion.accelZ * MOTION_ACCEL_SCALE_G;
    const float dpsX = motion.gyroX * MOTION_GYRO_SCALE_DEG_S;
    const float dpsY = motion.gyroY * MOTION_GYRO_SCALE_DEG_S;
    const float dpsZ = motion.gyroZ * MOTION_GYRO_SCALE_DEG_S;
    const float gyroScale = 13371.0f / 936.0f;
    const int16_t ax = switchImuRaw(-gZ, 4096.0f);
    const int16_t ay = switchImuRaw(-gX, 4096.0f);
    const int16_t az = switchImuRaw(gY, 4096.0f);
    const int16_t gx = switchImuRaw(-dpsZ, gyroScale);
    const int16_t gy = switchImuRaw(-dpsX, gyroScale);
    const int16_t gz = switchImuRaw(dpsY, gyroScale);
    for (int frame = 0; frame < 3; ++frame) {
        uint8_t* p = out + 12 + frame * 12;
        auto le16 = [&p](int off, int16_t v) {
            p[off] = static_cast<uint8_t>(static_cast<uint16_t>(v) & 0xFF);
            p[off + 1] = static_cast<uint8_t>(static_cast<uint16_t>(v) >> 8);
        };
        le16(0, ax);
        le16(2, ay);
        le16(4, az);
        le16(6, gx);
        le16(8, gy);
        le16(10, gz);
    }
}

// ── output-ring decoding ───────────────────────────────────────────────────

struct DecodedOutput {
    bool hasRumble = false;
    RumbleReport rumble{};
    bool hasLightbar = false;
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
};

// Switch HD-rumble block -> a coarse 0..1 amplitude (the same reduction the
// HIDMaestro SDK applies): HF band from byte 1, LF band from bytes 2-3.
inline float switchRumbleAmplitude(const uint8_t* block) {
    float hf = static_cast<float>(block[1] & 0xFE) / 200.0f;
    if (hf > 1.0f) hf = 1.0f;
    int lfRaw = (static_cast<int>(block[3]) - 0x40) * 2 + ((block[2] & 0x80) ? 1 : 0);
    if (lfRaw < 0) lfRaw = 0;
    float lf = static_cast<float>(lfRaw) / 101.0f;
    if (lf > 1.0f) lf = 1.0f;
    return hf > lf ? hf : lf;
}

inline DecodedOutput decodeOutputPacket(GamepadIdentity identity, const OutputPacket& pkt) {
    DecodedOutput out;
    switch (identity) {
    case GamepadIdentity::Xbox:
        if (pkt.source == OUTPUT_SOURCE_XINPUT && pkt.size >= 4) {
            out.hasRumble = true;
            out.rumble.strongMagnitude = static_cast<uint16_t>(pkt.data[2]) * 257;
            out.rumble.weakMagnitude = static_cast<uint16_t>(pkt.data[3]) * 257;
        }
        break;
    case GamepadIdentity::DS4:
        if (pkt.source == OUTPUT_SOURCE_HID_OUTPUT && pkt.reportId == DS4V2_OUTPUT_REPORT_ID) {
            const Ds4OutputReport rpt = ds4ParseOutputReport(pkt.reportId, pkt.data, pkt.size);
            if (rpt.valid && rpt.rumbleValid) {
                out.hasRumble = true;
                out.rumble = ds4RumbleFromOutput(rpt);
            }
            if (rpt.valid && rpt.lightbarValid) {
                out.hasLightbar = true;
                out.r = rpt.r;
                out.g = rpt.g;
                out.b = rpt.b;
            }
        }
        break;
    case GamepadIdentity::DualSense:
        // DS5 output report 0x02, RID stripped: motors at payload 2/3 (right/
        // weak precedes left/strong), lightbar RGB at 44-46 gated on
        // valid_flag2's lightbar-control bit.
        if (pkt.source == OUTPUT_SOURCE_HID_OUTPUT && pkt.reportId == 0x02 && pkt.size >= 47) {
            if (pkt.data[0] & 0x03) {
                out.hasRumble = true;
                out.rumble.weakMagnitude = static_cast<uint16_t>(pkt.data[2]) * 257;
                out.rumble.strongMagnitude = static_cast<uint16_t>(pkt.data[3]) * 257;
            }
            if (pkt.data[38] & 0x04) {
                out.hasLightbar = true;
                out.r = pkt.data[44];
                out.g = pkt.data[45];
                out.b = pkt.data[46];
            }
        }
        break;
    case GamepadIdentity::SwitchPro:
        // Rumble subcommand 0x01 / stream 0x10, RID stripped: payload byte 0
        // is the global packet counter, bytes 1-4 the left HD-rumble block,
        // 5-8 the right.
        if (pkt.source == OUTPUT_SOURCE_HID_OUTPUT &&
            (pkt.reportId == 0x01 || pkt.reportId == 0x10) && pkt.size >= 9) {
            const float left = switchRumbleAmplitude(pkt.data + 1);
            const float right = switchRumbleAmplitude(pkt.data + 5);
            out.hasRumble = true;
            out.rumble.strongMagnitude = static_cast<uint16_t>(left * 65535.0f);
            out.rumble.weakMagnitude = static_cast<uint16_t>(right * 65535.0f);
        }
        break;
    }
    return out;
}

} // namespace hidmaestro
} // namespace satellite
