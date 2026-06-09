// SPDX-License-Identifier: LGPL-3.0-or-later
#include "../src/core/touchpad_codec.h"
#include "../src/core/types.h"

#include <cstdint>
#include <iostream>
#include <string>

static int g_pass = 0;
static int g_fail = 0;
static std::string g_currentTest;

#define TEST(name)                                                                                 \
    do { g_currentTest = (name); } while (0)

#define EXPECT(cond)                                                                               \
    do {                                                                                           \
        if (cond) {                                                                                \
            g_pass++;                                                                              \
        } else {                                                                                   \
            g_fail++;                                                                              \
            std::cerr << "  FAIL [" << g_currentTest << "] " << __FILE__ << ":" << __LINE__        \
                      << "  " << #cond << "\n";                                                    \
        }                                                                                          \
    } while (0)

#define EXPECT_EQ(a, b)                                                                            \
    do {                                                                                           \
        auto _a = (a);                                                                             \
        auto _b = (b);                                                                             \
        if (_a == _b) {                                                                            \
            g_pass++;                                                                              \
        } else {                                                                                   \
            g_fail++;                                                                              \
            std::cerr << "  FAIL [" << g_currentTest << "] " << __FILE__ << ":" << __LINE__        \
                      << "  " << #a << " == " << #b << "  (got " << +(_a) << " vs " << +(_b)       \
                      << ")\n";                                                                    \
        }                                                                                          \
    } while (0)

// ---- touchpadWireToRange -----------------------------------------------------

static void test_wireToRange_saturates_at_edges() {
    TEST("touchpadWireToRange — min wire maps to 0, max to res-1");
    EXPECT_EQ(touchpadWireToRange(-32768, DS4_TOUCHPAD_RES_X), 0);
    EXPECT_EQ(touchpadWireToRange(32767, DS4_TOUCHPAD_RES_X), DS4_TOUCHPAD_RES_X - 1);
    EXPECT_EQ(touchpadWireToRange(-32768, DS4_TOUCHPAD_RES_Y), 0);
    EXPECT_EQ(touchpadWireToRange(32767, DS4_TOUCHPAD_RES_Y), DS4_TOUCHPAD_RES_Y - 1);
}

static void test_wireToRange_centre() {
    TEST("touchpadWireToRange — centre wire (0) maps to ~res/2");
    EXPECT_EQ(touchpadWireToRange(0, DS4_TOUCHPAD_RES_X), 960); // 32768*1920/65536
    EXPECT_EQ(touchpadWireToRange(0, DS4_TOUCHPAD_RES_Y), 471); // 32768*943/65536
}

static void test_wireToRange_degenerate_res() {
    TEST("touchpadWireToRange — res<=1 collapses to 0 (no divide-by-tiny)");
    EXPECT_EQ(touchpadWireToRange(0, 1), 0);
    EXPECT_EQ(touchpadWireToRange(32767, 0), 0);
    EXPECT_EQ(touchpadWireToRange(-1, -5), 0);
}

// ---- ds4PackTouchFinger ------------------------------------------------------

static void test_ds4Pack_roundtrips_coordinates() {
    TEST("ds4PackTouchFinger — packed 12-bit x/y reconstruct to device coords");
    TouchpadFinger f;
    f.active = true;
    f.trackingId = 5;
    f.x = 0; // → device x 960
    f.y = 0; // → device y 471
    auto b = ds4PackTouchFinger(f, f.trackingId);

    const int expX = touchpadWireToRange(f.x, DS4_TOUCHPAD_RES_X);
    const int expY = touchpadWireToRange(f.y, DS4_TOUCHPAD_RES_Y);
    const int gotX = b[1] | ((b[2] & 0x0F) << 8);
    const int gotY = (b[2] >> 4) | (b[3] << 4);
    EXPECT_EQ(gotX, expX);
    EXPECT_EQ(gotY, expY);
}

static void test_ds4Pack_tracking_and_lift_bit() {
    TEST("ds4PackTouchFinger — bit7 marks lift, low 7 bits carry tracking id");
    TouchpadFinger down;
    down.active = true;
    auto bd = ds4PackTouchFinger(down, 0x05);
    EXPECT_EQ(bd[0], (uint8_t)0x05); // active → bit7 clear

    TouchpadFinger up;
    up.active = false;
    auto bu = ds4PackTouchFinger(up, 0x05);
    EXPECT_EQ(bu[0], (uint8_t)0x85); // lifted → bit7 set

    TouchpadFinger masked;
    masked.active = true;
    auto bm = ds4PackTouchFinger(masked, 0xFF); // id masked to 7 bits
    EXPECT_EQ(bm[0], (uint8_t)0x7F);
}

// ---- decodeMotionReport ------------------------------------------------------

static void test_decodeMotion_le_fields() {
    TEST("decodeMotionReport — little-endian int16 axes + u32 timestamp");
    uint8_t p[MOTION_WIRE_PAYLOAD_BYTES] = {
        0x01, 0x00,             // gyroX = 1
        0xFF, 0xFF,             // gyroY = -1
        0x34, 0x12,             // gyroZ = 0x1234
        0x02, 0x00,             // accelX = 2
        0xFF, 0x7F,             // accelY = 32767
        0xFE, 0xFF,             // accelZ = -2
        0x04, 0x03, 0x02, 0x01, // timestampDeltaUs = 0x01020304
    };
    MotionReport r = decodeMotionReport(p);
    EXPECT_EQ(r.gyroX, (int16_t)1);
    EXPECT_EQ(r.gyroY, (int16_t)-1);
    EXPECT_EQ(r.gyroZ, (int16_t)0x1234);
    EXPECT_EQ(r.accelX, (int16_t)2);
    EXPECT_EQ(r.accelY, (int16_t)32767);
    EXPECT_EQ(r.accelZ, (int16_t)-2);
    EXPECT_EQ(r.timestampDeltaUs, 0x01020304u);
}

// ---- decodeTouchpadReport ----------------------------------------------------

static void test_decodeTouchpad_flags_fingers_time() {
    TEST("decodeTouchpadReport — flags, both fingers, and eventTimeMs");
    uint8_t p[TOUCHPAD_WIRE_PAYLOAD_BYTES] = {
        0x07,                   // flags: f0 active, f1 active, button pressed
        0x11,                   // finger0 trackingId
        0x10, 0x27,             // finger0.x = 10000
        0x18, 0xFC,             // finger0.y = -1000
        0x22,                   // finger1 trackingId
        0x00, 0x80,             // finger1.x = -32768
        0xFF, 0x7F,             // finger1.y = 32767
        0xD2, 0x04, 0x00, 0x00, // eventTimeMs = 1234
    };
    TouchpadReport r = decodeTouchpadReport(p);
    EXPECT(r.finger0.active);
    EXPECT(r.finger1.active);
    EXPECT(r.buttonPressed);
    EXPECT_EQ(r.finger0.trackingId, (uint8_t)0x11);
    EXPECT_EQ(r.finger0.x, (int16_t)10000);
    EXPECT_EQ(r.finger0.y, (int16_t)-1000);
    EXPECT_EQ(r.finger1.trackingId, (uint8_t)0x22);
    EXPECT_EQ(r.finger1.x, (int16_t)-32768);
    EXPECT_EQ(r.finger1.y, (int16_t)32767);
    EXPECT_EQ(r.eventTimeMs, 1234u);
}

static void test_decodeTouchpad_clear_flags() {
    TEST("decodeTouchpadReport — zero flags means no contact, no button");
    uint8_t p[TOUCHPAD_WIRE_PAYLOAD_BYTES] = {0};
    TouchpadReport r = decodeTouchpadReport(p);
    EXPECT(!r.finger0.active);
    EXPECT(!r.finger1.active);
    EXPECT(!r.buttonPressed);
}

int main() {
    std::cout << "Running codec tests...\n\n";
    test_wireToRange_saturates_at_edges();
    test_wireToRange_centre();
    test_wireToRange_degenerate_res();
    test_ds4Pack_roundtrips_coordinates();
    test_ds4Pack_tracking_and_lift_bit();
    test_decodeMotion_le_fields();
    test_decodeTouchpad_flags_fingers_time();
    test_decodeTouchpad_clear_flags();

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
