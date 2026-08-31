// SPDX-License-Identifier: LGPL-3.0-or-later

// platform/windows/hidmaestro_report — the pure per-profile packers and
// output decoders for the HIDMaestro backend. Byte-exact vectors pin the
// x360 HID+GIP slices (round-tripped through the companion's documented
// decode math), the DS4/DS5 payloads (sticks, buttons, IMU rescale, touch,
// battery), the Switch 0x30 body, and the ring-packet rumble/lightbar
// decoders. Pure, so the wire contracts are verified on every CI platform.
#include "test_util.h"

#include "../src/platform/windows/hidmaestro_report.h"

#include <cstring>
#include <string>

using namespace satellite::hidmaestro;

namespace {

uint16_t u16at(const uint8_t* p, size_t off) {
    return static_cast<uint16_t>(p[off] | (p[off + 1] << 8));
}

int16_t s16at(const uint8_t* p, size_t off) { return static_cast<int16_t>(u16at(p, off)); }

// The companion's IOCTL_XUSB_GET_STATE conversion (DecodeGipToXInput):
// X = u16 - 32768, Y = 32767 - u16, trigger = (u16 & 0x3FF) * 255 / 1023.
int gipToXinputX(uint16_t v) { return static_cast<int>(v) - 32768; }
int gipToXinputY(uint16_t v) { return 32767 - static_cast<int>(v); }
int gipToXinputTrigger(uint16_t v) { return ((v & 0x03FF) * 255) / 1023; }

} // namespace

static void test_profile_mapping() {
    TEST("profileForIdentity covers all four identities");
    EXPECT_EQ(std::string(profileForIdentity(GamepadIdentity::Xbox)),
              std::string("xbox-360-wired"));
    EXPECT_EQ(std::string(profileForIdentity(GamepadIdentity::DS4)), std::string("dualshock-4-v2"));
    EXPECT_EQ(std::string(profileForIdentity(GamepadIdentity::DualSense)),
              std::string("dualsense"));
    EXPECT_EQ(std::string(profileForIdentity(GamepadIdentity::SwitchPro)),
              std::string("switch-pro"));
}

static void test_x360_neutral_report() {
    TEST("packX360Report — neutral frame: centred axes, no buttons, hat null");
    GamepadReport pad{};
    uint8_t out[X360_REPORT_BYTES];
    packX360Report(pad, out);
    EXPECT_EQ(u16at(out, 0), (uint16_t)32768); // LX
    EXPECT_EQ(u16at(out, 2), (uint16_t)32767); // LY (0 maps to 32767 on the inverted axis)
    EXPECT_EQ(u16at(out, 4), (uint16_t)32768); // RX
    EXPECT_EQ(u16at(out, 6), (uint16_t)32767); // RY
    EXPECT_EQ(u16at(out, 8), (uint16_t)32768); // combined Z centred
    EXPECT_EQ(u16at(out, 10), (uint16_t)0);    // Vx
    EXPECT_EQ(u16at(out, 12), (uint16_t)0);    // Vy
    EXPECT_EQ(out[14], (uint8_t)0);
    EXPECT_EQ(out[15], (uint8_t)0); // hat null (0), no LS/RS
    EXPECT_EQ(out[16], (uint8_t)0);
    EXPECT_EQ(out[17], (uint8_t)0);
}

static void test_x360_sticks_roundtrip_extremes() {
    TEST("packX360Report — stick extremes survive the companion's decode math");
    GamepadReport pad{};
    pad.sThumbLX = 32767;
    pad.sThumbLY = 32767;
    pad.sThumbRX = -32768;
    pad.sThumbRY = -32768;
    uint8_t out[X360_REPORT_BYTES];
    packX360Report(pad, out);
    EXPECT_EQ(gipToXinputX(u16at(out, 0)), 32767);
    EXPECT_EQ(gipToXinputY(u16at(out, 2)), 32767);
    EXPECT_EQ(gipToXinputX(u16at(out, 4)), -32768);
    EXPECT_EQ(gipToXinputY(u16at(out, 6)), -32768);
}

static void test_x360_buttons_and_hat() {
    TEST("packX360Report — button bits and hat encoding");
    GamepadReport pad{};
    pad.wButtons = 0x1000 | 0x0010 | 0x0040 | 0x0001; // A + Start + LS + dpad-up
    uint8_t out[X360_REPORT_BYTES];
    packX360Report(pad, out);
    EXPECT_EQ(out[14], (uint8_t)(0x01 | 0x80));     // A + Start
    EXPECT_EQ(out[15], (uint8_t)(0x01 | (1 << 2))); // LS + hat N (1)
    pad.wButtons = 0x0002 | 0x0004;                 // dpad down+left -> SW (6)
    packX360Report(pad, out);
    EXPECT_EQ(out[15] >> 2, 6);
}

static void test_x360_triggers() {
    TEST("packX360Report — combined Z plus separate Vx/Vy");
    GamepadReport pad{};
    pad.bLeftTrigger = 255;
    uint8_t out[X360_REPORT_BYTES];
    packX360Report(pad, out);
    EXPECT_EQ(u16at(out, 8), (uint16_t)1); // full LT pulls combined Z to the floor
    EXPECT_EQ(u16at(out, 10), (uint16_t)65535);
    EXPECT_EQ(u16at(out, 12), (uint16_t)0);

    pad.bLeftTrigger = 0;
    pad.bRightTrigger = 255;
    packX360Report(pad, out);
    EXPECT_EQ(u16at(out, 8), (uint16_t)65535);
    EXPECT_EQ(u16at(out, 12), (uint16_t)65535);
}

static void test_gip_roundtrip() {
    TEST("packGip — companion decode reproduces the XUSB frame");
    GamepadReport pad{};
    pad.sThumbLX = 1234;
    pad.sThumbLY = -4321;
    pad.sThumbRX = -32768;
    pad.sThumbRY = 32767;
    pad.bLeftTrigger = 255;
    pad.bRightTrigger = 128;
    pad.wButtons = 0x1000 | 0x0400 | 0x0020 | 0x0001; // A + Guide + Back + dpad-up
    uint8_t gip[INPUT_GIP_LENGTH];
    packGip(pad, gip);

    EXPECT_EQ(gipToXinputX(u16at(gip, 0)), 1234);
    EXPECT_EQ(gipToXinputY(u16at(gip, 2)), -4321);
    EXPECT_EQ(gipToXinputX(u16at(gip, 4)), -32768);
    EXPECT_EQ(gipToXinputY(u16at(gip, 6)), 32767);
    EXPECT_EQ(gipToXinputTrigger(u16at(gip, 8)), 255);
    // Mid-scale triggers round within the companion's 10-bit integer math.
    const int rt = gipToXinputTrigger(u16at(gip, 10));
    EXPECT(rt >= 127 && rt <= 128);
    EXPECT_EQ(gip[12], (uint8_t)0x01);                     // A
    EXPECT_EQ(gip[13], (uint8_t)(0x01 | (1 << 2) | 0x40)); // Back + hat N + Guide
}

static void test_sony_imu_rescale() {
    TEST("sonyImuFromWire — x40000/32767 with saturation");
    EXPECT_EQ(sonyImuFromWire(0), (int16_t)0);
    // 1 deg/s on the wire = 16.38 LSB -> 20 raw LSB under the stub calibration.
    EXPECT_EQ(sonyImuFromWire(16), (int16_t)19);
    EXPECT_EQ(sonyImuFromWire(-16), (int16_t)-19);
    EXPECT_EQ(sonyImuFromWire(32767), (int16_t)32767); // saturates, no wrap
    EXPECT_EQ(sonyImuFromWire(-32768), (int16_t)-32768);
    EXPECT_EQ(sonyImuFromWire(26843), (int16_t)32767); // just past the clip knee
    EXPECT_EQ(sonyImuFromWire(26000), (int16_t)31739);
}

static void test_ds4_payload() {
    TEST("packDs4Payload — RID stripped, sticks/hat/triggers/IMU at DS4 offsets");
    Ds4InputState st{};
    st.pad.sThumbLX = 0;
    st.pad.sThumbLY = 0;
    st.pad.bLeftTrigger = 200;
    st.pad.wButtons = 0x1000 | 0x0001; // Cross + dpad up
    st.motion.gyroX = 100;
    st.batteryByte = 0x1B;
    uint8_t out[DS4_PAYLOAD_BYTES];
    packDs4Payload(st, out);

    EXPECT_EQ(out[0], (uint8_t)127); // LX centre
    EXPECT_EQ(out[1], (uint8_t)128); // LY centre (inverted axis)
    // Byte 4 (full byte 5): hat 0 (N) | Cross bit (1<<5 in the 14-bit run).
    EXPECT_EQ(out[4], (uint8_t)(0x00 | 0x20));
    // L2 digital bit rides byte 5 (full 6) bit 2; analog at payload 7.
    EXPECT_EQ(out[5] & 0x04, 0x04);
    EXPECT_EQ(out[7], (uint8_t)200);
    EXPECT_EQ(out[8], (uint8_t)0);
    EXPECT_EQ(out[11], (uint8_t)0x1B);               // battery (full byte 12)
    EXPECT_EQ(s16at(out, 12), sonyImuFromWire(100)); // gyroX (full 13), rescaled
    EXPECT_EQ(out[29], (uint8_t)0x1B);               // hid-sony battery mirror (full 30)
    EXPECT_EQ(out[32], (uint8_t)1);                  // one touch frame declared (full 33)
    EXPECT_EQ(out[34] & 0x80, 0x80);                 // finger 0 inactive
}

static void test_ds5_payload_neutral() {
    TEST("packDs5Payload — neutral frame at real DualSense offsets");
    Ds5InputState st{};
    st.seq = 7;
    uint8_t out[DS5_PAYLOAD_BYTES];
    packDs5Payload(st, out);
    EXPECT_EQ(out[0], (uint8_t)127);
    EXPECT_EQ(out[1], (uint8_t)128);
    EXPECT_EQ(out[2], (uint8_t)127);
    EXPECT_EQ(out[3], (uint8_t)128);
    EXPECT_EQ(out[4], (uint8_t)0);
    EXPECT_EQ(out[5], (uint8_t)0);
    EXPECT_EQ(out[6], (uint8_t)7); // seq counter
    EXPECT_EQ(out[7], (uint8_t)8); // hat released, no face buttons
    EXPECT_EQ(out[8], (uint8_t)0);
    EXPECT_EQ(out[9], (uint8_t)0);
    EXPECT_EQ(out[32] & 0x80, 0x80); // touch fingers inactive
    EXPECT_EQ(out[36] & 0x80, 0x80);
    EXPECT_EQ(out[52], (uint8_t)(0x20 | 10)); // full/wired default
}

static void test_ds5_payload_buttons_motion_touch() {
    TEST("packDs5Payload — buttons, IMU rescale + timestamp, touch, battery");
    Ds5InputState st{};
    st.pad.wButtons = 0x1000 | 0x8000 | 0x0100 | 0x0020 | 0x0400 | 0x0008;
    // Cross + Triangle + L1 + Create + PS + dpad-right
    st.pad.bRightTrigger = 55;
    st.motion.gyroY = -200;
    st.motion.accelZ = 1000;
    st.sensorTimestamp = 0x01020304;
    st.finger0.active = true;
    st.finger0.x = 0; // centre -> 960 of 1920
    st.finger0.y = 0;
    st.touchTrackingId0 = 5;
    st.touchpadButtonPressed = true;
    BatteryReport batt;
    batt.level = 40;
    batt.status = BATTERY_STATUS_CHARGING;
    st.batteryByte = ds5BatteryByte(batt);
    uint8_t out[DS5_PAYLOAD_BYTES];
    packDs5Payload(st, out);

    EXPECT_EQ(out[7], (uint8_t)(2 /*hat E*/ | 0x20 /*Cross*/ | 0x80 /*Triangle*/));
    EXPECT_EQ(out[8], (uint8_t)(0x01 /*L1*/ | 0x08 /*R2 digital*/ | 0x10 /*Create*/));
    EXPECT_EQ(out[9], (uint8_t)(0x01 /*PS*/ | 0x02 /*touchpad click*/));
    EXPECT_EQ(out[5], (uint8_t)55);
    EXPECT_EQ(s16at(out, 17), sonyImuFromWire(-200)); // gyroY (full 18)
    EXPECT_EQ(s16at(out, 25), sonyImuFromWire(1000)); // accelZ (full 26)
    EXPECT_EQ(out[27], (uint8_t)0x04);                // timestamp LE (full 28)
    EXPECT_EQ(out[30], (uint8_t)0x01);
    EXPECT_EQ(out[32], (uint8_t)5); // finger 0 active, id 5
    const int touchX = out[33] | ((out[34] & 0x0F) << 8);
    EXPECT_EQ(touchX, 960);
    EXPECT_EQ(out[52], (uint8_t)(0x10 | 4)); // charging at 40%
}

static void test_switch_body_neutral() {
    TEST("packSwitchBody — neutral body: centred 12-bit sticks, zero IMU");
    GamepadReport pad{};
    MotionReport motion{};
    uint8_t out[SWITCH_BODY_BYTES];
    packSwitchBody(pad, motion, out);
    EXPECT_EQ(out[2], (uint8_t)0);
    EXPECT_EQ(out[3], (uint8_t)0);
    EXPECT_EQ(out[4], (uint8_t)0);
    // 0x800 centre packs as 00 08 80 (the driver's own neutral bytes).
    EXPECT_EQ(out[5], (uint8_t)0x00);
    EXPECT_EQ(out[6], (uint8_t)0x08);
    EXPECT_EQ(out[7], (uint8_t)0x80);
    EXPECT_EQ(out[8], (uint8_t)0x00);
    EXPECT_EQ(out[9], (uint8_t)0x08);
    EXPECT_EQ(out[10], (uint8_t)0x80);
    for (size_t i = 12; i < SWITCH_BODY_BYTES; i++) EXPECT_EQ(out[i], (uint8_t)0);
}

static void test_switch_body_buttons_positional() {
    TEST("packSwitchBody — SDL's Nintendo mapping (A/B + X/Y swapped)");
    GamepadReport pad{};
    pad.wButtons = 0x1000; // XUSB A (south) -> wire B
    uint8_t out[SWITCH_BODY_BYTES];
    MotionReport motion{};
    packSwitchBody(pad, motion, out);
    EXPECT_EQ(out[2], (uint8_t)0x04);

    pad.wButtons = 0x2000; // XUSB B (east) -> wire A
    packSwitchBody(pad, motion, out);
    EXPECT_EQ(out[2], (uint8_t)0x08);

    pad.wButtons = 0x4000 | 0x8000; // X (west) -> Y, Y (north) -> X
    packSwitchBody(pad, motion, out);
    EXPECT_EQ(out[2], (uint8_t)(0x01 | 0x02));

    pad.wButtons = 0x0020 | 0x0010 | 0x0400 | 0x0040; // Back/Start/Guide/LS
    packSwitchBody(pad, motion, out);
    EXPECT_EQ(out[3], (uint8_t)(0x01 | 0x02 | 0x10 | 0x08));

    pad.wButtons = 0x0001 | 0x0008 | 0x0100; // dpad up + right + LB
    pad.bLeftTrigger = 255;
    packSwitchBody(pad, motion, out);
    EXPECT_EQ(out[4], (uint8_t)(0x02 | 0x04 | 0x40 | 0x80));
    EXPECT_EQ(out[2], (uint8_t)0);
}

static void test_switch_body_sticks_and_imu() {
    TEST("packSwitchBody — 12-bit stick range and IMU frame permutation");
    GamepadReport pad{};
    pad.sThumbLX = 32767; // full right -> 0x800 + 0x600 = 0xE00
    pad.sThumbLY = 32767; // full up -> wire up-positive keeps + = 0xE00
    uint8_t out[SWITCH_BODY_BYTES];
    MotionReport motion{};
    packSwitchBody(pad, motion, out);
    const uint16_t lx = static_cast<uint16_t>(out[5] | ((out[6] & 0x0F) << 8));
    const uint16_t ly = static_cast<uint16_t>((out[6] >> 4) | (out[7] << 4));
    EXPECT_EQ(lx, (uint16_t)0xE00); // 0x800 + full-scale 0x600
    EXPECT_EQ(ly, (uint16_t)0xE00);

    // IMU: +Y-up wire accel of +4 g lands on wire Z at 4*4096; gyro +2000
    // deg/s on wire Y lands on wire Z at 2000 * 13371/936.
    motion.accelY = 32767;
    motion.gyroY = 32767;
    packSwitchBody(pad, motion, out);
    EXPECT_EQ(s16at(out, 12 + 4), (int16_t)16384);  // accel wire Z = +4 g
    EXPECT_EQ(s16at(out, 12 + 10), (int16_t)28571); // gyro wire Z ~= 2000 deg/s
    // Three identical IMU frames.
    EXPECT_EQ(std::memcmp(out + 12, out + 24, 12), 0);
    EXPECT_EQ(std::memcmp(out + 12, out + 36, 12), 0);
}

static void test_decode_xinput_rumble() {
    TEST("decodeOutputPacket — XUSB SET_STATE motors, wrong-source guarded");
    OutputPacket pkt;
    pkt.source = OUTPUT_SOURCE_XINPUT;
    pkt.reportId = 0;
    pkt.size = 5;
    pkt.data[2] = 0x40;
    pkt.data[3] = 0x80;
    DecodedOutput d = decodeOutputPacket(GamepadIdentity::Xbox, pkt);
    EXPECT(d.hasRumble);
    EXPECT_EQ(d.rumble.strongMagnitude, (uint16_t)(0x40 * 257));
    EXPECT_EQ(d.rumble.weakMagnitude, (uint16_t)(0x80 * 257));
    EXPECT(!d.hasLightbar);

    // Too-short and wrong-source packets decode to nothing.
    pkt.size = 3;
    EXPECT(!decodeOutputPacket(GamepadIdentity::Xbox, pkt).hasRumble);
    pkt.size = 5;
    pkt.source = OUTPUT_SOURCE_HID_OUTPUT;
    EXPECT(!decodeOutputPacket(GamepadIdentity::Xbox, pkt).hasRumble);
}

static void test_decode_ds4_output() {
    TEST("decodeOutputPacket — DS4 0x05 motors + lightbar, flag-gated");
    OutputPacket pkt;
    pkt.source = OUTPUT_SOURCE_HID_OUTPUT;
    pkt.reportId = 0x05;
    pkt.size = 31;
    pkt.data[0] = 0x03; // motors + lightbar valid
    pkt.data[3] = 10;   // small/weak
    pkt.data[4] = 20;   // large/strong
    pkt.data[5] = 1;
    pkt.data[6] = 2;
    pkt.data[7] = 3;
    DecodedOutput d = decodeOutputPacket(GamepadIdentity::DS4, pkt);
    EXPECT(d.hasRumble);
    EXPECT_EQ(d.rumble.strongMagnitude, (uint16_t)(20 * 257));
    EXPECT_EQ(d.rumble.weakMagnitude, (uint16_t)(10 * 257));
    EXPECT(d.hasLightbar);
    EXPECT_EQ(d.r, (uint8_t)1);
    EXPECT_EQ(d.g, (uint8_t)2);
    EXPECT_EQ(d.b, (uint8_t)3);

    pkt.data[0] = 0x02; // lightbar only
    d = decodeOutputPacket(GamepadIdentity::DS4, pkt);
    EXPECT(!d.hasRumble);
    EXPECT(d.hasLightbar);
}

static void test_decode_ds5_output() {
    TEST("decodeOutputPacket — DualSense 0x02 motors + lightbar");
    OutputPacket pkt;
    pkt.source = OUTPUT_SOURCE_HID_OUTPUT;
    pkt.reportId = 0x02;
    pkt.size = 47;
    pkt.data[0] = 0x03;
    pkt.data[2] = 11; // right/weak
    pkt.data[3] = 22; // left/strong
    pkt.data[38] = 0x04;
    pkt.data[44] = 9;
    pkt.data[45] = 8;
    pkt.data[46] = 7;
    DecodedOutput d = decodeOutputPacket(GamepadIdentity::DualSense, pkt);
    EXPECT(d.hasRumble);
    EXPECT_EQ(d.rumble.strongMagnitude, (uint16_t)(22 * 257));
    EXPECT_EQ(d.rumble.weakMagnitude, (uint16_t)(11 * 257));
    EXPECT(d.hasLightbar);
    EXPECT_EQ(d.r, (uint8_t)9);

    // No lightbar flag: colour is stale bytes, not a lightbar write.
    pkt.data[38] = 0;
    d = decodeOutputPacket(GamepadIdentity::DualSense, pkt);
    EXPECT(!d.hasLightbar);
    EXPECT(d.hasRumble);
}

static void test_decode_ds5_trigger_effects() {
    TEST("decodeOutputPacket — DS5 trigger-effect blocks, per-trigger flag-gated");
    OutputPacket pkt;
    pkt.source = OUTPUT_SOURCE_HID_OUTPUT;
    pkt.reportId = 0x02;
    pkt.size = 47;
    pkt.data[0] = 0x04 | 0x08; // right + left trigger effect valid
    for (int i = 0; i < 11; i++) pkt.data[10 + i] = (uint8_t)(0x40 + i); // right block
    for (int i = 0; i < 11; i++) pkt.data[21 + i] = (uint8_t)(0x60 + i); // left block
    DecodedOutput d = decodeOutputPacket(GamepadIdentity::DualSense, pkt);
    EXPECT(d.hasRightTriggerEffect);
    EXPECT(d.hasLeftTriggerEffect);
    EXPECT(!d.hasRumble);
    EXPECT_EQ(d.rightTriggerEffect[0], (uint8_t)0x40);
    EXPECT_EQ(d.rightTriggerEffect[10], (uint8_t)0x4A);
    EXPECT_EQ(d.leftTriggerEffect[0], (uint8_t)0x60);
    EXPECT_EQ(d.leftTriggerEffect[10], (uint8_t)0x6A);

    // One-sided write: only the flagged trigger decodes.
    pkt.data[0] = 0x08;
    d = decodeOutputPacket(GamepadIdentity::DualSense, pkt);
    EXPECT(!d.hasRightTriggerEffect);
    EXPECT(d.hasLeftTriggerEffect);

    // A report too short to hold the blocks decodes neither.
    pkt.data[0] = 0x04 | 0x08;
    pkt.size = 31;
    d = decodeOutputPacket(GamepadIdentity::DualSense, pkt);
    EXPECT(!d.hasRightTriggerEffect);
    EXPECT(!d.hasLeftTriggerEffect);
}

static void test_decode_ds5_player_leds() {
    TEST("decodeOutputPacket — DS5 player LEDs, flag-gated and masked to 5 bits");
    OutputPacket pkt;
    pkt.source = OUTPUT_SOURCE_HID_OUTPUT;
    pkt.reportId = 0x02;
    pkt.size = 47;
    pkt.data[1] = 0x10; // player-indicator control enable
    pkt.data[43] = 0xE5; // high bits are DS5 fade/etc. flags, masked off
    DecodedOutput d = decodeOutputPacket(GamepadIdentity::DualSense, pkt);
    EXPECT(d.hasPlayerLeds);
    EXPECT_EQ(d.playerLeds, (uint8_t)0x05);
    EXPECT(!d.hasLightbar);
    EXPECT(!d.hasRumble);

    pkt.data[1] = 0x00; // no flag: byte 43 is stale, not a LED write
    d = decodeOutputPacket(GamepadIdentity::DualSense, pkt);
    EXPECT(!d.hasPlayerLeds);

    pkt.data[1] = 0x10;
    pkt.size = 43; // too short to hold the LED byte
    d = decodeOutputPacket(GamepadIdentity::DualSense, pkt);
    EXPECT(!d.hasPlayerLeds);
}

static void test_decode_ds5_lightbar_valid_flag1() {
    TEST("decodeOutputPacket — DS5 lightbar honors valid_flag1 bit 2 (SDL/hid-playstation)");
    OutputPacket pkt;
    pkt.source = OUTPUT_SOURCE_HID_OUTPUT;
    pkt.reportId = 0x02;
    pkt.size = 47;
    pkt.data[1] = 0x04; // LIGHTBAR_CONTROL_ENABLE, valid_flag2 untouched
    pkt.data[44] = 5;
    pkt.data[45] = 6;
    pkt.data[46] = 7;
    DecodedOutput d = decodeOutputPacket(GamepadIdentity::DualSense, pkt);
    EXPECT(d.hasLightbar);
    EXPECT_EQ(d.r, (uint8_t)5);
    EXPECT_EQ(d.g, (uint8_t)6);
    EXPECT_EQ(d.b, (uint8_t)7);
}

static void test_decode_switch_player_leds() {
    TEST("decodeOutputPacket — Switch subcommand 0x30 player lights, low nibble only");
    OutputPacket pkt;
    pkt.source = OUTPUT_SOURCE_HID_OUTPUT;
    pkt.reportId = 0x01;
    pkt.size = 11;
    // Neutral rumble blocks so the amplitude decode stays quiet.
    pkt.data[1] = 0x00;
    pkt.data[2] = 0x01;
    pkt.data[3] = 0x40;
    pkt.data[4] = 0x40;
    pkt.data[5] = 0x00;
    pkt.data[6] = 0x01;
    pkt.data[7] = 0x40;
    pkt.data[8] = 0x40;
    pkt.data[9] = 0x30;  // subcommand: set player lights
    pkt.data[10] = 0xF3; // flash bits in the high nibble are dropped
    DecodedOutput d = decodeOutputPacket(GamepadIdentity::SwitchPro, pkt);
    EXPECT(d.hasPlayerLeds);
    EXPECT_EQ(d.playerLeds, (uint8_t)0x03);

    pkt.data[9] = 0x48; // a different subcommand (enable IMU) is not a LED write
    d = decodeOutputPacket(GamepadIdentity::SwitchPro, pkt);
    EXPECT(!d.hasPlayerLeds);

    pkt.data[9] = 0x30;
    pkt.reportId = 0x10; // the rumble stream carries no subcommand bytes
    d = decodeOutputPacket(GamepadIdentity::SwitchPro, pkt);
    EXPECT(!d.hasPlayerLeds);
}

static void test_decode_switch_rumble() {
    TEST("decodeOutputPacket — Switch HD-rumble amplitude reduction");
    OutputPacket pkt;
    pkt.source = OUTPUT_SOURCE_HID_OUTPUT;
    pkt.reportId = 0x10;
    pkt.size = 9;
    // Left block: HF byte 0xC8 -> amplitude 1.0. Right block: neutral.
    pkt.data[1] = 0x00;
    pkt.data[2] = 0xC8;
    pkt.data[5] = 0x00;
    pkt.data[6] = 0x01;
    pkt.data[7] = 0x40;
    pkt.data[8] = 0x40;
    DecodedOutput d = decodeOutputPacket(GamepadIdentity::SwitchPro, pkt);
    EXPECT(d.hasRumble);
    EXPECT_EQ(d.rumble.strongMagnitude, (uint16_t)65535);
    EXPECT_EQ(d.rumble.weakMagnitude, (uint16_t)0);

    pkt.reportId = 0x21; // subcommand replies are not rumble
    EXPECT(!decodeOutputPacket(GamepadIdentity::SwitchPro, pkt).hasRumble);
}

int main() {
    test_profile_mapping();
    test_x360_neutral_report();
    test_x360_sticks_roundtrip_extremes();
    test_x360_buttons_and_hat();
    test_x360_triggers();
    test_gip_roundtrip();
    test_sony_imu_rescale();
    test_ds4_payload();
    test_ds5_payload_neutral();
    test_ds5_payload_buttons_motion_touch();
    test_switch_body_neutral();
    test_switch_body_buttons_positional();
    test_switch_body_sticks_and_imu();
    test_decode_xinput_rumble();
    test_decode_ds4_output();
    test_decode_ds5_output();
    test_decode_ds5_trigger_effects();
    test_decode_ds5_player_leds();
    test_decode_ds5_lightbar_valid_flag1();
    test_decode_switch_player_leds();
    test_decode_switch_rumble();

    std::cout << "hidmaestro_report: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
