// SPDX-License-Identifier: LGPL-3.0-or-later

// Pure DualShock 4 (v2, CUH-ZCT2) report codec for the macOS IOHIDUserDevice
// backend: HID report-descriptor bytes, input-report packing, output-report
// (rumble/lightbar) parsing, and the feature-report blobs served on get-report.
//
// IOKit-free by design (same pure-codec/IO-shell doctrine as the rest of
// src/: see docs/architecture.md "a pure codec, separate from its I/O").
// The IOKit shell lives in mac_hid_gamepad_adapter.cpp; a future DriverKit
// dext transport would reuse this header untouched.
//
// Byte layout is the DS4 USB wire format. The authoritative in-repo reference
// is DS4_REPORT_EX in vigem/include/ViGEm/BusShared.h (the 63-byte report 0x01
// with the report-id byte stripped); this codec emits the same bytes shifted
// one position right by the leading report-id.
#pragma once

#include "core/touchpad_codec.h"
#include "core/types.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

// USB identity of a DualShock 4 v2. Published verbatim so macOS's native DS4
// support (GameController.framework's DualShock profile and games' own VID/PID
// sniffing) adopts the virtual pad exactly like real hardware.
inline const uint16_t DS4V2_VENDOR_ID = 0x054C;  // Sony Interactive Entertainment
inline const uint16_t DS4V2_PRODUCT_ID = 0x09CC; // Wireless Controller (DS4 v2)
inline const uint16_t DS4V2_VERSION_BCD = 0x0100;
inline const char* DS4V2_MANUFACTURER_STRING = "Sony Interactive Entertainment";
inline const char* DS4V2_PRODUCT_STRING = "Wireless Controller";

// Report ids and total transfer sizes (id byte included, USB convention).
inline const uint8_t DS4V2_INPUT_REPORT_ID = 0x01;
inline const uint8_t DS4V2_OUTPUT_REPORT_ID = 0x05;
inline const uint8_t DS4V2_FEATURE_CALIBRATION_ID = 0x02;
inline const uint8_t DS4V2_FEATURE_FIRMWARE_ID = 0xA3;
inline const uint8_t DS4V2_FEATURE_PAIRING_ID = 0x12;
inline const int DS4V2_INPUT_REPORT_BYTES = 64;  // 0x01 + 63 payload bytes
inline const int DS4V2_OUTPUT_REPORT_BYTES = 32; // 0x05 + 31 payload bytes
inline const int DS4V2_FEATURE_CALIBRATION_BYTES = 37;
inline const int DS4V2_FEATURE_FIRMWARE_BYTES = 49;
inline const int DS4V2_FEATURE_PAIRING_BYTES = 16;

// HID report descriptor for the virtual DS4 v2.
//
// Hand-written to be reviewable byte-by-byte (a raw 507-byte hardware dump is
// not), but shape-identical to the real pad where consumers care: report 0x01
// is a 64-byte input whose axes/hat/buttons sit at the exact hardware bit
// positions, report 0x05 is the 32-byte rumble+lightbar output, and the three
// feature reports every DS4 driver probes (0x02 calibration, 0xA3 firmware,
// 0x12 pairing) are declared with hardware sizes. DS4-specific consumers read
// the IMU/touch/battery bytes at fixed offsets keyed on VID/PID, not from the
// descriptor, so those ride in the vendor-page filler block.
//
// Input report 0x01 field map (bit-exact HID packing):
//   bytes 1..4   sticks LX LY RX RY          (Generic Desktop X/Y/Z/Rz)
//   byte  5      bits 0..3 hat, bits 4..7 Square/Cross/Circle/Triangle
//   byte  6      L1 R1 L2 R2 Share Options L3 R3   (buttons 5..12)
//   byte  7      bit0 PS, bit1 touchpad click, bits 2..7 frame counter
//   bytes 8..9   L2 / R2 analog              (Generic Desktop Rx/Ry)
//   bytes 10..63 vendor block: timestamp, battery, IMU, touch frames (below)
inline const uint8_t DS4V2_REPORT_DESCRIPTOR[] = {
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x05,       // Usage (Gamepad)
    0xA1, 0x01,       // Collection (Application)
    0x85, 0x01,       //   Report ID (1) -- input
    0x09, 0x30,       //   Usage (X)  = left stick X
    0x09, 0x31,       //   Usage (Y)  = left stick Y
    0x09, 0x32,       //   Usage (Z)  = right stick X
    0x09, 0x35,       //   Usage (Rz) = right stick Y
    0x15, 0x00,       //   Logical Minimum (0)
    0x26, 0xFF, 0x00, //   Logical Maximum (255)
    0x75, 0x08,       //   Report Size (8)
    0x95, 0x04,       //   Report Count (4)
    0x81, 0x02,       //   Input (Data,Var,Abs)      -> bytes 1..4
    0x09, 0x39,       //   Usage (Hat Switch)
    0x15, 0x00,       //   Logical Minimum (0)
    0x25, 0x07,       //   Logical Maximum (7)
    0x35, 0x00,       //   Physical Minimum (0)
    0x46, 0x3B, 0x01, //   Physical Maximum (315)
    0x65, 0x14,       //   Unit (degrees)
    0x75, 0x04,       //   Report Size (4)
    0x95, 0x01,       //   Report Count (1)
    0x81, 0x42,       //   Input (Data,Var,Abs,Null) -> byte 5 bits 0..3
    0x65, 0x00,       //   Unit (none)
    0x05, 0x09,       //   Usage Page (Button)
    0x19, 0x01,       //   Usage Minimum (Button 1 = Square)
    0x29, 0x0E,       //   Usage Maximum (Button 14 = touchpad click)
    0x15, 0x00,       //   Logical Minimum (0)
    0x25, 0x01,       //   Logical Maximum (1)
    0x75, 0x01,       //   Report Size (1)
    0x95, 0x0E,       //   Report Count (14)
    0x81, 0x02,       //   Input (Data,Var,Abs)      -> byte 5 bit 4 .. byte 7 bit 1
    0x06, 0x00, 0xFF, //   Usage Page (Vendor 0xFF00)
    0x09, 0x20,       //   Usage (0x20) = frame counter
    0x75, 0x06,       //   Report Size (6)
    0x95, 0x01,       //   Report Count (1)
    0x15, 0x00,       //   Logical Minimum (0)
    0x25, 0x3F,       //   Logical Maximum (63)
    0x81, 0x02,       //   Input (Data,Var,Abs)      -> byte 7 bits 2..7
    0x05, 0x01,       //   Usage Page (Generic Desktop)
    0x09, 0x33,       //   Usage (Rx) = L2 analog
    0x09, 0x34,       //   Usage (Ry) = R2 analog
    0x15, 0x00,       //   Logical Minimum (0)
    0x26, 0xFF, 0x00, //   Logical Maximum (255)
    0x75, 0x08,       //   Report Size (8)
    0x95, 0x02,       //   Report Count (2)
    0x81, 0x02,       //   Input (Data,Var,Abs)      -> bytes 8..9
    0x06, 0x00, 0xFF, //   Usage Page (Vendor 0xFF00)
    0x09, 0x21,       //   Usage (0x21) = timestamp/battery/IMU/touch block
    0x95, 0x36,       //   Report Count (54)
    0x75, 0x08,       //   Report Size (8)
    0x15, 0x00,       //   Logical Minimum (0)
    0x26, 0xFF, 0x00, //   Logical Maximum (255)
    0x81, 0x02,       //   Input (Data,Var,Abs)      -> bytes 10..63
    0x85, 0x05,       //   Report ID (5) -- output (rumble + lightbar)
    0x09, 0x22,       //   Usage (0x22)
    0x95, 0x1F,       //   Report Count (31)
    0x91, 0x02,       //   Output (Data,Var,Abs)     -> 32 bytes with the id
    0x85, 0x02,       //   Report ID (2) -- feature: IMU calibration
    0x09, 0x24,       //   Usage (0x24)
    0x95, 0x24,       //   Report Count (36)
    0xB1, 0x02,       //   Feature (Data,Var,Abs)    -> 37 bytes with the id
    0x85, 0xA3,       //   Report ID (0xA3) -- feature: firmware/build info
    0x09, 0x25,       //   Usage (0x25)
    0x95, 0x30,       //   Report Count (48)
    0xB1, 0x02,       //   Feature (Data,Var,Abs)    -> 49 bytes with the id
    0x85, 0x12,       //   Report ID (0x12) -- feature: pairing / MAC info
    0x09, 0x26,       //   Usage (0x26)
    0x95, 0x0F,       //   Report Count (15)
    0xB1, 0x02,       //   Feature (Data,Var,Abs)    -> 16 bytes with the id
    0xC0,             // End Collection
};
inline const size_t DS4V2_REPORT_DESCRIPTOR_BYTES = sizeof(DS4V2_REPORT_DESCRIPTOR);

// DS4 hat nibble from XUSB dpad bits. Encoding: 0 N, 1 NE, 2 E, 3 SE, 4 S,
// 5 SW, 6 W, 7 NW, 8 released. The branch ordering mirrors
// ViGEmAdapter::submitDS4Report exactly so contradictory bit combinations
// (up+down held) resolve identically on both platforms.
inline uint8_t ds4HatFromButtons(uint16_t wButtons) {
    const bool up = (wButtons & 0x0001) != 0;
    const bool down = (wButtons & 0x0002) != 0;
    const bool left = (wButtons & 0x0004) != 0;
    const bool right = (wButtons & 0x0008) != 0;
    if (up && right) return 1;
    if (up && left) return 7;
    if (down && right) return 3;
    if (down && left) return 5;
    if (up) return 0;
    if (down) return 4;
    if (left) return 6;
    if (right) return 2;
    return 8;
}

// Xbox signed int16 stick -> DS4 unsigned byte; Y axes inverted (XUSB Y is
// positive-up, DS4 is positive-down). Same arithmetic as the Windows adapter,
// so a given wire report produces byte-identical stick values on both.
// Note centre (0) lands on 127, not 128; real sticks never sit exactly centred
// and every consumer deadzones, so we keep the single shared formula.
inline uint8_t ds4StickByte(int16_t v) {
    return static_cast<uint8_t>((((int)v + 32768) * 255) / 65535);
}
inline uint8_t ds4StickByteInverted(int16_t v) {
    return static_cast<uint8_t>(255 - (((int)v + 32768) * 255) / 65535);
}

// DS4 battery byte: bit 4 (0x10) = cable connected, low nibble = level in
// tenths (nibble 11 + cable = the "fully charged" sentinel). Same mapping as
// the file-local ds4BatteryByte in platform/windows/vigem_adapter.cpp
// (duplicated because that TU is Windows-only; hoist candidate if a third
// backend ever needs it).
inline uint8_t ds4BatteryByte(const BatteryReport& report) {
    int nibble = (report.level == BATTERY_LEVEL_UNKNOWN)
                     ? 5 // mid-scale so the host still shows something
                     : static_cast<int>(report.level) / 10;
    if (nibble > 10) nibble = 10;

    switch (report.status) {
    case BATTERY_STATUS_CHARGING:
        return static_cast<uint8_t>(0x10 | nibble); // cable connected + charging
    case BATTERY_STATUS_FULL:
    case BATTERY_STATUS_WIRED:
        return static_cast<uint8_t>(0x10 | 11);
    default: // discharging/unknown: on battery, no cable bit
        return static_cast<uint8_t>(nibble);
    }
}

// Everything the input packer folds into one 64-byte report. Owned by the
// adapter per slot (under its lock) and passed here by const ref, so the
// packer itself stays a pure function of its arguments.
//
// motion is copied VERBATIM: the wire convention (gyro +/-2000 deg/s, accel
// +/-4 g over int16, core/types.h) equals the DS4's raw sensor scale
// (16.4 LSB/deg/s, 8192 LSB/g), and the calibration feature report below is
// chosen so consumers' calibration math is the identity.
struct Ds4InputState {
    GamepadReport pad{};
    MotionReport motion{};
    // Both battery bytes default to 0x1B = cable + fully charged, matching the
    // ViGEm plug-in default, so a pad with no battery stream reads "wired".
    uint8_t batteryByte = 0x1B;
    TouchpadFinger finger0{};
    TouchpadFinger finger1{};
    uint8_t touchTrackingId0 = 0; // bumped by the adapter on each touch-down
    uint8_t touchTrackingId1 = 0;
    bool touchpadButtonPressed = false;
    uint8_t touchPacketCounter = 0; // bumped by the adapter per touchpad sample
    uint8_t frameCounter = 0;       // 6-bit rolling, bumped per packed report
    uint16_t timestamp = 0;         // free-running, 5.33 us (16/3) units
};

// Pack the full 64-byte input report (report id 0x01 at out[0]).
//
// Offset map (see DS4_REPORT_EX for the id-stripped equivalent):
//   [0]      0x01                      [12]     battery (cable|level)
//   [1..4]   LX LY RX RY               [13..18] gyro X/Y/Z   (3 x i16 LE)
//   [5]      hat | face buttons        [19..24] accel X/Y/Z  (3 x i16 LE)
//   [6]      shoulder/trigger/meta     [25..29] zeros
//   [7]      PS | tpad | counter<<2    [30]     battery again (hid-sony offset)
//   [8..9]   L2 / R2 analog            [31..32] zeros
//   [10..11] timestamp u16 LE          [33]     touch frame count (1)
//                                      [34..42] current touch frame
//                                      [43..63] zeros (history frames unused)
inline void ds4PackInputReport(const Ds4InputState& st, uint8_t out[DS4V2_INPUT_REPORT_BYTES]) {
    std::memset(out, 0, DS4V2_INPUT_REPORT_BYTES);
    out[0] = DS4V2_INPUT_REPORT_ID;

    out[1] = ds4StickByte(st.pad.sThumbLX);
    out[2] = ds4StickByteInverted(st.pad.sThumbLY);
    out[3] = ds4StickByte(st.pad.sThumbRX);
    out[4] = ds4StickByteInverted(st.pad.sThumbRY);

    // XUSB -> DS4 buttons, identical mapping to ViGEmAdapter::submitDS4Report,
    // plus the digital L2/R2 bits real hardware derives from the analog values
    // (the ViGEm driver synthesizes those bus-side; here we are the hardware).
    const uint16_t x = st.pad.wButtons;
    uint16_t btn = ds4HatFromButtons(x);          // bits 0..3
    if (x & 0x1000) btn |= 1 << 5;                // A      -> Cross
    if (x & 0x2000) btn |= 1 << 6;                // B      -> Circle
    if (x & 0x4000) btn |= 1 << 4;                // X      -> Square
    if (x & 0x8000) btn |= 1 << 7;                // Y      -> Triangle
    if (x & 0x0100) btn |= 1 << 8;                // LB     -> L1
    if (x & 0x0200) btn |= 1 << 9;                // RB     -> R1
    if (x & 0x0020) btn |= 1 << 12;               // Back   -> Share
    if (x & 0x0010) btn |= 1 << 13;               // Start  -> Options
    if (x & 0x0040) btn |= 1 << 14;               // LS     -> L3
    if (x & 0x0080) btn |= 1 << 15;               // RS     -> R3
    if (st.pad.bLeftTrigger > 0) btn |= 1 << 10;  // L2 digital
    if (st.pad.bRightTrigger > 0) btn |= 1 << 11; // R2 digital
    out[5] = static_cast<uint8_t>(btn & 0xFF);
    out[6] = static_cast<uint8_t>(btn >> 8);

    out[7] = static_cast<uint8_t>(((x & 0x0400) ? 0x01 : 0x00) |          // Guide -> PS
                                  (st.touchpadButtonPressed ? 0x02 : 0) | // touchpad click
                                  ((st.frameCounter & 0x3F) << 2));

    out[8] = st.pad.bLeftTrigger;
    out[9] = st.pad.bRightTrigger;

    out[10] = static_cast<uint8_t>(st.timestamp & 0xFF);
    out[11] = static_cast<uint8_t>(st.timestamp >> 8);
    out[12] = st.batteryByte;

    auto le16 = [&out](int off, int16_t v) {
        out[off] = static_cast<uint8_t>(static_cast<uint16_t>(v) & 0xFF);
        out[off + 1] = static_cast<uint8_t>(static_cast<uint16_t>(v) >> 8);
    };
    le16(13, st.motion.gyroX);
    le16(15, st.motion.gyroY);
    le16(17, st.motion.gyroZ);
    le16(19, st.motion.accelX);
    le16(21, st.motion.accelY);
    le16(23, st.motion.accelZ);

    // [25..29] reserved. [30] is where hid-sony-lineage consumers read the
    // battery; mirror [12] so both conventions see the same value.
    out[30] = st.batteryByte;

    // One touch frame carrying the CURRENT contact state (same policy as the
    // ViGEm EX path: bTouchPacketsN = 1, sCurrentTouch only). Coordinate and
    // tracking-id packing reuses the core codec (12-bit x/y pairs).
    out[33] = 1;
    out[34] = st.touchPacketCounter;
    const auto f0 = ds4PackTouchFinger(st.finger0, st.touchTrackingId0);
    const auto f1 = ds4PackTouchFinger(st.finger1, st.touchTrackingId1);
    out[35] = f0[0];
    out[36] = f0[1];
    out[37] = f0[2];
    out[38] = f0[3];
    out[39] = f1[0];
    out[40] = f1[1];
    out[41] = f1[2];
    out[42] = f1[3];
    // [43..60] would be the two touch history frames; zero means "no data"
    // (lift bit clear + tracking id 0 is ignored because [33] declares 1 frame).
}

// Parsed DS4 output report 0x05 (host game -> pad): rumble motors + lightbar.
// The valid flags gate each group so a game that only sets colour does not
// zero the motors with the stale bytes riding in the same report.
struct Ds4OutputReport {
    bool valid = false;
    bool rumbleValid = false;   // flags bit 0
    uint8_t smallMotor = 0;     // weak / high-frequency (payload byte 3)
    uint8_t largeMotor = 0;     // strong / low-frequency (payload byte 4)
    bool lightbarValid = false; // flags bit 1
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
};

// Parse a set-report payload for report id 5. Accepts both transport forms:
// with the leading report-id byte (32 bytes, USB capture form) and without
// (31 bytes, the form IOKit hands a set-report callback after splitting the
// id off). The discriminator is length + leading byte, because a bare flags
// byte can legitimately be 0x05 (motor|flash).
//   [0] flags: 0x01 motors valid, 0x02 lightbar valid, 0x04 flash valid
//   [1..2] reserved   [3] small/weak motor   [4] large/strong motor
//   [5..7] lightbar R G B   [8..9] flash on/off (ignored)
inline Ds4OutputReport ds4ParseOutputReport(uint32_t reportId, const uint8_t* data, size_t len) {
    Ds4OutputReport out;
    if (data == nullptr) return out;
    if (reportId != DS4V2_OUTPUT_REPORT_ID) {
        // Some transports report id 0 and leave the id byte in the payload.
        if (!(reportId == 0 && len >= 1 && data[0] == DS4V2_OUTPUT_REPORT_ID)) return out;
    }
    const uint8_t* p = data;
    size_t n = len;
    if (n >= static_cast<size_t>(DS4V2_OUTPUT_REPORT_BYTES) && p[0] == DS4V2_OUTPUT_REPORT_ID) {
        p += 1; // id-prefixed form
        n -= 1;
    }
    if (n < 8) return out; // need flags..B
    out.valid = true;
    out.rumbleValid = (p[0] & 0x01) != 0;
    out.smallMotor = p[3];
    out.largeMotor = p[4];
    out.lightbarValid = (p[0] & 0x02) != 0;
    out.r = p[5];
    out.g = p[6];
    out.b = p[7];
    return out;
}

// DS4 motor byte -> RumbleReport, on the XINPUT_VIBRATION u16 scale. The x257
// widening matches the ViGEm notification loop, so the dish receives identical
// magnitudes for identical game output on both platforms. durationMs stays 0;
// SessionService stamps the wire-side refresh deadline.
inline RumbleReport ds4RumbleFromOutput(const Ds4OutputReport& o) {
    RumbleReport rr;
    rr.strongMagnitude = static_cast<uint16_t>(o.largeMotor) * 257;
    rr.weakMagnitude = static_cast<uint16_t>(o.smallMotor) * 257;
    return rr;
}

// ── Feature reports (served on get-report) ─────────────────────────────────
//
// DS4 drivers (GameController.framework included) probe these on adoption.
// All blobs carry the report id at [0] and are returned verbatim.

// 0x02: IMU calibration, USB report layout: bias trios, then the gyro
// extremes INTERLEAVED per axis (grouped plus-first is the Bluetooth report
// 0x05 layout — hid-sony/SDL/DS4Windows all branch on transport, and this
// device advertises USB), then gyro speed pair, then accel plus/minus pairs.
// Values are chosen so the standard calibration math (hid-sony lineage) is the
// IDENTITY on our raw values:
//   gyro:  speed_2x = 540+540; denom = plus-minus = 17694 = 1080*32767/2000,
//          so calibrated full-scale == raw full-scale == 2000 deg/s.
//   accel: range_2g = 8192-(-8192) = 2*8192, so calibrated == raw, 8192/g.
// Wire MotionReport values therefore pass through consumers unscaled.
inline const uint8_t DS4V2_FEATURE_CALIBRATION[DS4V2_FEATURE_CALIBRATION_BYTES] = {
    0x02,                               // report id
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // gyro pitch/yaw/roll bias = 0
    0x8F, 0x22, 0x71, 0xDD,             // gyro pitch plus/minus = +8847/-8847
    0x8F, 0x22, 0x71, 0xDD,             // gyro yaw   plus/minus
    0x8F, 0x22, 0x71, 0xDD,             // gyro roll  plus/minus
    0x1C, 0x02, 0x1C, 0x02,             // gyro speed plus/minus = 540 each
    0x00, 0x20, 0x00, 0xE0,             // accel X plus/minus = +8192/-8192
    0x00, 0x20, 0x00, 0xE0,             // accel Y plus/minus
    0x00, 0x20, 0x00, 0xE0,             // accel Z plus/minus
    0x00, 0x00,                         // reserved
};

// 0xA3: firmware/build info. Real pads carry build date/time strings and
// version words; consumers display but do not gate on them, so a stable
// synthetic blob (zero-padded strings, plausible version words) suffices.
inline const uint8_t DS4V2_FEATURE_FIRMWARE[DS4V2_FEATURE_FIRMWARE_BYTES] = {
    0xA3, // report id
    // build date slot (16 bytes, NUL-padded ASCII)
    'S',
    'a',
    't',
    'e',
    'l',
    'l',
    'i',
    't',
    'e',
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    // build time slot (16 bytes)
    'v',
    'i',
    'r',
    't',
    'u',
    'a',
    'l',
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0x01,
    0x00, // hw version major/minor
    0x00,
    0x00,
    0x00,
    0x00, // reserved
    0x01,
    0x00,
    0x00,
    0x00, // fw version
    0x00,
    0x00, // reserved
    0x0B,
    0x00, // sw series
    0x00,
    0x00, // tail
};

// 0x12: pairing info: device Bluetooth MAC (little-endian byte order) +
// padding + paired-host MAC (zeros: USB-only virtual pad). The device MAC is
// synthesized per backend serial below so multiple virtual pads stay distinct.
inline const uint8_t DS4V2_FEATURE_PAIRING_TEMPLATE[DS4V2_FEATURE_PAIRING_BYTES] = {
    0x12,                               // report id
    0x00, 0x00, 0x54, 0x41, 0x53, 0x02, // device MAC LE; [1] gets the serial
    0x08, 0x25, 0x00,                   // padding (matches hardware captures)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // host MAC: never BT-paired
};

// Locally-administered MAC for a backend serial: 02:53:41:54:00:<serial>
// ("SAT" in the middle). Big-endian/display order; the 0x12 blob stores it
// reversed per BT convention.
inline void ds4MacForSerial(uint32_t serial, uint8_t out[6]) {
    out[0] = 0x02; // locally administered, unicast
    out[1] = 0x53; // 'S'
    out[2] = 0x41; // 'A'
    out[3] = 0x54; // 'T'
    out[4] = 0x00;
    out[5] = static_cast<uint8_t>(serial & 0xFF);
}

// "02:53:41:54:00:0c" — the kIOHIDSerialNumberKey string, mirroring how real
// DS4s expose their MAC as the USB serial. `out` must hold >= 18 chars.
inline void ds4SerialString(uint32_t serial, char out[18]) {
    uint8_t mac[6];
    ds4MacForSerial(serial, mac);
    static const char* hexd = "0123456789abcdef";
    int pos = 0;
    for (int i = 0; i < 6; i++) {
        out[pos++] = hexd[mac[i] >> 4];
        out[pos++] = hexd[mac[i] & 0x0F];
        if (i != 5) out[pos++] = ':';
    }
    out[pos] = '\0';
}

// Fill `out` (>= 64 bytes) with the feature report for `reportId`; returns the
// report length, or 0 for an id we do not serve (shell answers unsupported).
inline size_t ds4FeatureReport(uint32_t reportId, uint32_t serial, uint8_t* out) {
    switch (reportId) {
    case DS4V2_FEATURE_CALIBRATION_ID:
        std::memcpy(out, DS4V2_FEATURE_CALIBRATION, DS4V2_FEATURE_CALIBRATION_BYTES);
        return DS4V2_FEATURE_CALIBRATION_BYTES;
    case DS4V2_FEATURE_FIRMWARE_ID:
        std::memcpy(out, DS4V2_FEATURE_FIRMWARE, DS4V2_FEATURE_FIRMWARE_BYTES);
        return DS4V2_FEATURE_FIRMWARE_BYTES;
    case DS4V2_FEATURE_PAIRING_ID: {
        std::memcpy(out, DS4V2_FEATURE_PAIRING_TEMPLATE, DS4V2_FEATURE_PAIRING_BYTES);
        uint8_t mac[6];
        ds4MacForSerial(serial, mac);
        for (int i = 0; i < 6; i++) out[1 + i] = mac[5 - i]; // BT little-endian order
        return DS4V2_FEATURE_PAIRING_BYTES;
    }
    default:
        return 0;
    }
}
