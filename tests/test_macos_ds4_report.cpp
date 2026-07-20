// SPDX-License-Identifier: LGPL-3.0-or-later

// Pure suite for the macOS DS4 v2 report codec (platform/macos/ds4_report.h):
// descriptor size/shape pins, input-report byte pins (buttons, sticks, hat,
// triggers, IMU, battery, 12-bit touchpad packing), output-report parsing in
// both transport forms, and the feature-report blobs. IOKit-free; runs on the
// bare CI runner with no entitlement.
#include "../src/core/touchpad_codec.h"
#include "../src/platform/macos/ds4_report.h"
// IOKit-free by design: only the pure probe-status seam is used from it.
#include "../src/platform/macos/mac_hid_gamepad_adapter.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

#include "test_util.h"

// XUSB wButtons bits (core/types.h convention, mirrors vigem_adapter.cpp).
namespace {
constexpr uint16_t XUSB_DPAD_UP = 0x0001;
constexpr uint16_t XUSB_DPAD_DOWN = 0x0002;
constexpr uint16_t XUSB_DPAD_LEFT = 0x0004;
constexpr uint16_t XUSB_DPAD_RIGHT = 0x0008;
constexpr uint16_t XUSB_START = 0x0010;
constexpr uint16_t XUSB_BACK = 0x0020;
constexpr uint16_t XUSB_LS = 0x0040;
constexpr uint16_t XUSB_RS = 0x0080;
constexpr uint16_t XUSB_LB = 0x0100;
constexpr uint16_t XUSB_RB = 0x0200;
constexpr uint16_t XUSB_GUIDE = 0x0400;
constexpr uint16_t XUSB_A = 0x1000;
constexpr uint16_t XUSB_B = 0x2000;
constexpr uint16_t XUSB_X = 0x4000;
constexpr uint16_t XUSB_Y = 0x8000;

// Simple additive checksum: pins the descriptor bytes as a whole so any
// accidental edit trips the suite even if the size stays equal.
uint32_t byteSum(const uint8_t* p, size_t n) {
    uint32_t s = 0;
    for (size_t i = 0; i < n; i++) s += p[i];
    return s;
}
} // namespace

static void test_descriptor_shape() {
    TEST("descriptor: size, framing, and report-id inventory are pinned");
    EXPECT_EQ(DS4V2_REPORT_DESCRIPTOR_BYTES, size_t{145});
    // Generic Desktop / Gamepad application collection.
    EXPECT_EQ(DS4V2_REPORT_DESCRIPTOR[0], (uint8_t)0x05);
    EXPECT_EQ(DS4V2_REPORT_DESCRIPTOR[1], (uint8_t)0x01);
    EXPECT_EQ(DS4V2_REPORT_DESCRIPTOR[2], (uint8_t)0x09);
    EXPECT_EQ(DS4V2_REPORT_DESCRIPTOR[3], (uint8_t)0x05);
    EXPECT_EQ(DS4V2_REPORT_DESCRIPTOR[4], (uint8_t)0xA1);
    EXPECT_EQ(DS4V2_REPORT_DESCRIPTOR[DS4V2_REPORT_DESCRIPTOR_BYTES - 1], (uint8_t)0xC0);

    // Exactly one declaration of each report id (0x85 <id>).
    int in1 = 0, out5 = 0, feat02 = 0, featA3 = 0, feat12 = 0;
    for (size_t i = 0; i + 1 < DS4V2_REPORT_DESCRIPTOR_BYTES; i++) {
        if (DS4V2_REPORT_DESCRIPTOR[i] != 0x85) continue;
        switch (DS4V2_REPORT_DESCRIPTOR[i + 1]) {
        case 0x01:
            in1++;
            break;
        case 0x05:
            out5++;
            break;
        case 0x02:
            feat02++;
            break;
        case 0xA3:
            featA3++;
            break;
        case 0x12:
            feat12++;
            break;
        }
    }
    EXPECT_EQ(in1, 1);
    EXPECT_EQ(out5, 1);
    EXPECT_EQ(feat02, 1);
    EXPECT_EQ(featA3, 1);
    EXPECT_EQ(feat12, 1);

    // Whole-descriptor checksum: recompute with
    //   python3 -c "print(sum(bytes))" if the descriptor legitimately changes.
    EXPECT_EQ(byteSum(DS4V2_REPORT_DESCRIPTOR, DS4V2_REPORT_DESCRIPTOR_BYTES), 8097u);

    // Identity constants games sniff on.
    EXPECT_EQ(DS4V2_VENDOR_ID, (uint16_t)0x054C);
    EXPECT_EQ(DS4V2_PRODUCT_ID, (uint16_t)0x09CC);
}

static void test_pack_neutral_frame() {
    TEST("pack: neutral state = centred sticks, hat released, battery wired-full");
    Ds4InputState st;
    uint8_t r[DS4V2_INPUT_REPORT_BYTES];
    ds4PackInputReport(st, r);

    EXPECT_EQ(r[0], (uint8_t)0x01); // report id
    // Stick centre via the shared conversion formula lands on 127 (comment in
    // ds4_report.h); Y inversion gives 128.
    EXPECT_EQ(r[1], (uint8_t)127);
    EXPECT_EQ(r[2], (uint8_t)128);
    EXPECT_EQ(r[3], (uint8_t)127);
    EXPECT_EQ(r[4], (uint8_t)128);
    EXPECT_EQ(r[5], (uint8_t)0x08); // hat released, no face buttons
    EXPECT_EQ(r[6], (uint8_t)0x00);
    EXPECT_EQ(r[7], (uint8_t)0x00); // no PS, no tpad click, counter 0
    EXPECT_EQ(r[8], (uint8_t)0);    // triggers
    EXPECT_EQ(r[9], (uint8_t)0);
    EXPECT_EQ(r[12], (uint8_t)0x1B); // cable + fully charged default
    EXPECT_EQ(r[30], (uint8_t)0x1B); // mirrored battery byte
    EXPECT_EQ(r[33], (uint8_t)1);    // one touch frame, both fingers lifted
    EXPECT_EQ(r[35], (uint8_t)0x80); // finger 0 lift bit, tracking id 0
    EXPECT_EQ(r[39], (uint8_t)0x80); // finger 1
    EXPECT_EQ(r[63], (uint8_t)0);    // tail stays zeroed
}

static void test_pack_buttons() {
    TEST("pack: XUSB button word maps to the DS4 bit positions");
    Ds4InputState st;
    st.pad.wButtons = XUSB_A | XUSB_B | XUSB_X | XUSB_Y | XUSB_LB | XUSB_RB | XUSB_BACK |
                      XUSB_START | XUSB_LS | XUSB_RS | XUSB_GUIDE;
    uint8_t r[DS4V2_INPUT_REPORT_BYTES];
    ds4PackInputReport(st, r);

    // byte 5: hat released (0x8) + Square|Cross|Circle|Triangle.
    EXPECT_EQ(r[5], (uint8_t)(0x08 | 0x10 | 0x20 | 0x40 | 0x80));
    // byte 6: L1|R1|Share|Options|L3|R3 (no analog triggers -> no L2/R2 bits).
    EXPECT_EQ(r[6], (uint8_t)(0x01 | 0x02 | 0x10 | 0x20 | 0x40 | 0x80));
    // byte 7: PS button only.
    EXPECT_EQ(r[7], (uint8_t)0x01);

    // Individual face-button spot checks.
    Ds4InputState a;
    a.pad.wButtons = XUSB_A; // Cross
    ds4PackInputReport(a, r);
    EXPECT_EQ(r[5], (uint8_t)(0x08 | 0x20));
    Ds4InputState x;
    x.pad.wButtons = XUSB_X; // Square
    ds4PackInputReport(x, r);
    EXPECT_EQ(r[5], (uint8_t)(0x08 | 0x10));
}

static void test_pack_triggers_and_digital_bits() {
    TEST("pack: analog triggers land at [8..9] and raise the digital L2/R2 bits");
    Ds4InputState st;
    st.pad.bLeftTrigger = 200;
    st.pad.bRightTrigger = 1;
    uint8_t r[DS4V2_INPUT_REPORT_BYTES];
    ds4PackInputReport(st, r);
    EXPECT_EQ(r[8], (uint8_t)200);
    EXPECT_EQ(r[9], (uint8_t)1);
    EXPECT_EQ(r[6], (uint8_t)(0x04 | 0x08)); // L2 + R2 digital (hardware derives these)
}

static void test_hat_encoding() {
    TEST("hat: 8 directions + released + contradictory bits match the Windows path");
    EXPECT_EQ(ds4HatFromButtons(0), (uint8_t)8);
    EXPECT_EQ(ds4HatFromButtons(XUSB_DPAD_UP), (uint8_t)0);
    EXPECT_EQ(ds4HatFromButtons(XUSB_DPAD_UP | XUSB_DPAD_RIGHT), (uint8_t)1);
    EXPECT_EQ(ds4HatFromButtons(XUSB_DPAD_RIGHT), (uint8_t)2);
    EXPECT_EQ(ds4HatFromButtons(XUSB_DPAD_DOWN | XUSB_DPAD_RIGHT), (uint8_t)3);
    EXPECT_EQ(ds4HatFromButtons(XUSB_DPAD_DOWN), (uint8_t)4);
    EXPECT_EQ(ds4HatFromButtons(XUSB_DPAD_DOWN | XUSB_DPAD_LEFT), (uint8_t)5);
    EXPECT_EQ(ds4HatFromButtons(XUSB_DPAD_LEFT), (uint8_t)6);
    EXPECT_EQ(ds4HatFromButtons(XUSB_DPAD_UP | XUSB_DPAD_LEFT), (uint8_t)7);
    // up+down: the up branch wins first, exactly like ViGEmAdapter's if-chain.
    EXPECT_EQ(ds4HatFromButtons(XUSB_DPAD_UP | XUSB_DPAD_DOWN), (uint8_t)0);
    EXPECT_EQ(ds4HatFromButtons(XUSB_DPAD_LEFT | XUSB_DPAD_RIGHT), (uint8_t)6);
}

static void test_stick_conversion() {
    TEST("sticks: signed int16 -> byte, Y inverted, extremes clamp to 0/255");
    Ds4InputState st;
    st.pad.sThumbLX = -32768;
    st.pad.sThumbLY = -32768; // down on XUSB -> 255 on DS4
    st.pad.sThumbRX = 32767;
    st.pad.sThumbRY = 32767; // up on XUSB -> 0 on DS4
    uint8_t r[DS4V2_INPUT_REPORT_BYTES];
    ds4PackInputReport(st, r);
    EXPECT_EQ(r[1], (uint8_t)0);
    EXPECT_EQ(r[2], (uint8_t)255);
    EXPECT_EQ(r[3], (uint8_t)255);
    EXPECT_EQ(r[4], (uint8_t)0);
}

static void test_pack_timestamp_and_counter() {
    TEST("pack: timestamp u16 LE at [10..11]; 6-bit frame counter at [7] bits 2..7");
    Ds4InputState st;
    st.timestamp = 0xBEEF;
    st.frameCounter = 0x3F;
    uint8_t r[DS4V2_INPUT_REPORT_BYTES];
    ds4PackInputReport(st, r);
    EXPECT_EQ(r[10], (uint8_t)0xEF);
    EXPECT_EQ(r[11], (uint8_t)0xBE);
    EXPECT_EQ(r[7], (uint8_t)(0x3F << 2));

    st.frameCounter = 0x40; // 7th bit must be masked off
    ds4PackInputReport(st, r);
    EXPECT_EQ(r[7], (uint8_t)0x00);
}

static void test_pack_motion_verbatim() {
    TEST("pack: IMU wire values are copied verbatim as i16 LE at [13..24]");
    Ds4InputState st;
    st.motion.gyroX = 1;
    st.motion.gyroY = -1;
    st.motion.gyroZ = 0x1234;
    st.motion.accelX = -32768;
    st.motion.accelY = 32767;
    st.motion.accelZ = -2;
    uint8_t r[DS4V2_INPUT_REPORT_BYTES];
    ds4PackInputReport(st, r);
    EXPECT_EQ(r[13], (uint8_t)0x01);
    EXPECT_EQ(r[14], (uint8_t)0x00);
    EXPECT_EQ(r[15], (uint8_t)0xFF);
    EXPECT_EQ(r[16], (uint8_t)0xFF);
    EXPECT_EQ(r[17], (uint8_t)0x34);
    EXPECT_EQ(r[18], (uint8_t)0x12);
    EXPECT_EQ(r[19], (uint8_t)0x00);
    EXPECT_EQ(r[20], (uint8_t)0x80);
    EXPECT_EQ(r[21], (uint8_t)0xFF);
    EXPECT_EQ(r[22], (uint8_t)0x7F);
    EXPECT_EQ(r[23], (uint8_t)0xFE);
    EXPECT_EQ(r[24], (uint8_t)0xFF);
}

static void test_battery_byte() {
    TEST("battery: same nibble/cable mapping as the Windows adapter");
    BatteryReport b;
    b.level = 47;
    b.status = BATTERY_STATUS_DISCHARGING;
    EXPECT_EQ(ds4BatteryByte(b), (uint8_t)4); // 47/10, no cable bit
    b.status = BATTERY_STATUS_CHARGING;
    EXPECT_EQ(ds4BatteryByte(b), (uint8_t)(0x10 | 4));
    b.status = BATTERY_STATUS_FULL;
    EXPECT_EQ(ds4BatteryByte(b), (uint8_t)0x1B); // cable + 11 sentinel
    b.status = BATTERY_STATUS_WIRED;
    EXPECT_EQ(ds4BatteryByte(b), (uint8_t)0x1B);
    b.level = BATTERY_LEVEL_UNKNOWN;
    b.status = BATTERY_STATUS_UNKNOWN;
    EXPECT_EQ(ds4BatteryByte(b), (uint8_t)5); // mid-scale placeholder
    b.level = 100;
    b.status = BATTERY_STATUS_DISCHARGING;
    EXPECT_EQ(ds4BatteryByte(b), (uint8_t)10); // clamped to nibble 10 on battery

    // Both report offsets carry the byte.
    Ds4InputState st;
    st.batteryByte = 0x14;
    uint8_t r[DS4V2_INPUT_REPORT_BYTES];
    ds4PackInputReport(st, r);
    EXPECT_EQ(r[12], (uint8_t)0x14);
    EXPECT_EQ(r[30], (uint8_t)0x14);
}

static void test_pack_touchpad_12bit() {
    TEST("pack: touch frame reuses the core 12-bit packer at [34..42]");
    Ds4InputState st;
    st.finger0.active = true;
    st.finger0.x = 0; // centre -> device 960 / 471
    st.finger0.y = 0;
    st.touchTrackingId0 = 5;
    st.finger1.active = false;
    st.touchTrackingId1 = 9;
    st.touchPacketCounter = 0x2A;
    st.touchpadButtonPressed = true;
    uint8_t r[DS4V2_INPUT_REPORT_BYTES];
    ds4PackInputReport(st, r);

    EXPECT_EQ(r[33], (uint8_t)1);    // one frame
    EXPECT_EQ(r[34], (uint8_t)0x2A); // packet counter

    // Byte-identical to the core codec the Windows adapter feeds into ViGEm.
    const auto f0 = ds4PackTouchFinger(st.finger0, st.touchTrackingId0);
    EXPECT_EQ(r[35], f0[0]);
    EXPECT_EQ(r[36], f0[1]);
    EXPECT_EQ(r[37], f0[2]);
    EXPECT_EQ(r[38], f0[3]);
    EXPECT_EQ(r[35], (uint8_t)0x05); // active -> lift bit clear + id
    // 12-bit reconstruction: x = 960, y = 471.
    const int gotX = r[36] | ((r[37] & 0x0F) << 8);
    const int gotY = (r[37] >> 4) | (r[38] << 4);
    EXPECT_EQ(gotX, 960);
    EXPECT_EQ(gotY, 471);

    EXPECT_EQ(r[39], (uint8_t)(0x80 | 9)); // finger 1 lifted, id preserved
    EXPECT_EQ(r[7], (uint8_t)0x02);        // touchpad click bit
}

static void test_output_parse_prefixed_and_bare() {
    TEST("output parse: id-prefixed (32 B) and bare (31 B) forms both decode");
    uint8_t prefixed[32] = {};
    prefixed[0] = 0x05; // report id
    prefixed[1] = 0x03; // flags: motors + lightbar valid
    prefixed[4] = 0x40; // small/weak
    prefixed[5] = 0x80; // large/strong
    prefixed[6] = 10;   // R
    prefixed[7] = 20;   // G
    prefixed[8] = 30;   // B
    Ds4OutputReport a = ds4ParseOutputReport(5, prefixed, sizeof(prefixed));
    EXPECT(a.valid);
    EXPECT(a.rumbleValid);
    EXPECT(a.lightbarValid);
    EXPECT_EQ(a.smallMotor, (uint8_t)0x40);
    EXPECT_EQ(a.largeMotor, (uint8_t)0x80);
    EXPECT_EQ(a.r, (uint8_t)10);
    EXPECT_EQ(a.g, (uint8_t)20);
    EXPECT_EQ(a.b, (uint8_t)30);

    uint8_t bare[31] = {};
    bare[0] = 0x01; // flags: motors only
    bare[3] = 0x11;
    bare[4] = 0x22;
    bare[5] = 99; // colour bytes present but flagged invalid
    Ds4OutputReport b = ds4ParseOutputReport(5, bare, sizeof(bare));
    EXPECT(b.valid);
    EXPECT(b.rumbleValid);
    EXPECT(!b.lightbarValid);
    EXPECT_EQ(b.smallMotor, (uint8_t)0x11);
    EXPECT_EQ(b.largeMotor, (uint8_t)0x22);
}

static void test_output_parse_flags_gate_flag05() {
    TEST("output parse: bare form whose flags byte is 0x05 is not mistaken for an id");
    // flags 0x05 = motors + flash; only a 31-byte bare report can start 0x05.
    uint8_t bare[31] = {};
    bare[0] = 0x05;
    bare[3] = 7;
    bare[4] = 8;
    Ds4OutputReport o = ds4ParseOutputReport(5, bare, sizeof(bare));
    EXPECT(o.valid);
    EXPECT(o.rumbleValid);
    EXPECT(!o.lightbarValid);
    EXPECT_EQ(o.smallMotor, (uint8_t)7);
    EXPECT_EQ(o.largeMotor, (uint8_t)8);
}

static void test_output_parse_rejects() {
    TEST("output parse: wrong id, null, and short buffers are rejected");
    uint8_t buf[32] = {};
    buf[0] = 0x05;
    EXPECT(!ds4ParseOutputReport(1, buf, sizeof(buf)).valid);
    EXPECT(!ds4ParseOutputReport(5, nullptr, 32).valid);
    EXPECT(!ds4ParseOutputReport(5, buf, 7).valid); // shorter than flags..B
    // id 0 with the id byte still in the payload (some transports do this).
    uint8_t withId[32] = {};
    withId[0] = 0x05;
    withId[1] = 0x02;
    withId[6] = 200;
    Ds4OutputReport o = ds4ParseOutputReport(0, withId, sizeof(withId));
    EXPECT(o.valid);
    EXPECT(o.lightbarValid);
    EXPECT_EQ(o.r, (uint8_t)200);
}

static void test_rumble_scaling() {
    TEST("rumble: DS4 motor bytes widen x257 to the XInput u16 scale");
    Ds4OutputReport o;
    o.valid = o.rumbleValid = true;
    o.largeMotor = 0xFF;
    o.smallMotor = 0x01;
    RumbleReport rr = ds4RumbleFromOutput(o);
    EXPECT_EQ(rr.strongMagnitude, (uint16_t)65535);
    EXPECT_EQ(rr.weakMagnitude, (uint16_t)257);
    EXPECT_EQ(rr.durationMs, (uint16_t)0); // SessionService stamps the deadline
}

static void test_feature_reports() {
    TEST("feature blobs: ids, sizes, calibration identity constants, per-serial MAC");
    uint8_t buf[64];
    EXPECT_EQ(ds4FeatureReport(0x02, 3, buf), (size_t)37);
    EXPECT_EQ(buf[0], (uint8_t)0x02);
    // gyro plus = +8847 LE, gyro speed = 540 LE, accel plus = +8192 LE.
    EXPECT_EQ(buf[7], (uint8_t)0x8F);
    EXPECT_EQ(buf[8], (uint8_t)0x22);
    // USB interleave: pitch MINUS at [9], roll PLUS at [15]. These are the
    // bytes that differ from the Bluetooth grouped order.
    EXPECT_EQ(buf[9], (uint8_t)0x71);
    EXPECT_EQ(buf[10], (uint8_t)0xDD);
    EXPECT_EQ(buf[15], (uint8_t)0x8F);
    EXPECT_EQ(buf[16], (uint8_t)0x22);
    EXPECT_EQ(buf[19], (uint8_t)0x1C);
    EXPECT_EQ(buf[20], (uint8_t)0x02);
    EXPECT_EQ(buf[23], (uint8_t)0x00);
    EXPECT_EQ(buf[24], (uint8_t)0x20);

    EXPECT_EQ(ds4FeatureReport(0xA3, 3, buf), (size_t)49);
    EXPECT_EQ(buf[0], (uint8_t)0xA3);

    EXPECT_EQ(ds4FeatureReport(0x12, 0x0C, buf), (size_t)16);
    EXPECT_EQ(buf[0], (uint8_t)0x12);
    EXPECT_EQ(buf[1], (uint8_t)0x0C); // MAC low byte (BT LE order) = serial
    EXPECT_EQ(buf[6], (uint8_t)0x02); // locally-administered prefix at the top

    EXPECT_EQ(ds4FeatureReport(0x99, 3, buf), (size_t)0); // unknown id unserved

    char serialStr[18];
    ds4SerialString(0x0C, serialStr);
    EXPECT_EQ(std::string(serialStr), std::string("02:53:41:54:00:0c"));
}

// The parse every USB hid-sony-lineage consumer applies to feature report
// 0x02 (Linux hid-playstation dualshock4_get_calibration_data non-BT branch,
// SDL SDL_hidapi_ps4 LoadCalibrationData USB branch, DS4Windows fromUSB):
// gyro extremes per axis at [7..18], denominator = plus - minus.
static void test_calibration_usb_consumer_parse() {
    TEST("calibration: USB per-axis parse yields non-zero symmetric gyro denominators");
    uint8_t buf[64];
    EXPECT_EQ(ds4FeatureReport(0x02, 3, buf), (size_t)37);
    auto le16 = [&buf](int off) {
        return static_cast<int16_t>(static_cast<uint16_t>(buf[off]) |
                                    (static_cast<uint16_t>(buf[off + 1]) << 8));
    };
    const int pitchDenom = le16(7) - le16(9);
    const int yawDenom = le16(11) - le16(13);
    const int rollDenom = le16(15) - le16(17);
    // Zero here means division-by-zero/NaN or hid-sony's broken-clone
    // rejection in real consumers.
    EXPECT(pitchDenom != 0);
    EXPECT(yawDenom != 0);
    EXPECT(rollDenom != 0);
    // A real DS4's gyro speed/range is symmetric across axes.
    EXPECT_EQ(pitchDenom, yawDenom);
    EXPECT_EQ(rollDenom, yawDenom);
    // Identity constant from the blob comment: denom = 1080*32767/2000.
    EXPECT_EQ(yawDenom, 17694);
}

static void test_probe_status_seam() {
    TEST("probe seam: entitled -> machid backend; unentitled -> the exact legacy stub");
    const BackendStatus on = macHidBackendStatus(true);
    EXPECT_EQ(std::string(on.id), std::string(BACKEND_ID_MAC_HID));
    EXPECT_EQ(std::string(on.id), std::string("machid")); // wire constant, web UI keys on it
    EXPECT(on.supported);
    EXPECT(on.available);
    EXPECT(on.errorCode == nullptr);

    // The unentitled branch must stay byte-identical to the pre-backend stub
    // (platform/macos/gamepad_backend.cpp before this backend existed).
    const BackendStatus off = macHidBackendStatus(false);
    EXPECT_EQ(std::string(off.id), std::string(BACKEND_ID_NONE));
    EXPECT_EQ(std::string(off.id), std::string("none"));
    EXPECT(!off.supported);
    EXPECT(!off.available);
    EXPECT(off.errorCode == nullptr);
}

int main() {
    std::cout << "Running macOS DS4 report codec tests...\n\n";
    test_descriptor_shape();
    test_pack_neutral_frame();
    test_pack_buttons();
    test_pack_triggers_and_digital_bits();
    test_hat_encoding();
    test_stick_conversion();
    test_pack_timestamp_and_counter();
    test_pack_motion_verbatim();
    test_battery_byte();
    test_pack_touchpad_12bit();
    test_output_parse_prefixed_and_bare();
    test_output_parse_flags_gate_flag05();
    test_output_parse_rejects();
    test_rumble_scaling();
    test_feature_reports();
    test_calibration_usb_consumer_parse();
    test_probe_status_seam();

    std::cout << "\n=== Test Results ===\n";
    std::cout << "  Passed: " << g_pass << "\n";
    std::cout << "  Failed: " << g_fail << "\n";
    if (g_fail > 0) {
        std::cout << "  STATUS: FAIL\n";
        return 1;
    }
    std::cout << "  STATUS: ALL PASSED\n";
    return 0;
}
