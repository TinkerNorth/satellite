// SPDX-License-Identifier: LGPL-3.0-or-later
// The XUSB-to-Sony button layout has one home, and the two packers that
// used to spell it out separately (the DualShock 4 input report and the
// DualSense payload) must agree byte for byte on what a button becomes.
#include "../src/core/ds4_report.h"
#include "../src/platform/windows/hidmaestro_report.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

#include "test_util.h"

using satellite::hidmaestro::DS5_PAYLOAD_BYTES;
using satellite::hidmaestro::Ds5InputState;
using satellite::hidmaestro::packDs5Payload;

namespace {

constexpr size_t MAPPED_BUTTONS = sizeof(XUSB_TO_SONY_BUTTONS) / sizeof(XUSB_TO_SONY_BUTTONS[0]);
constexpr uint16_t HAT_NIBBLE = 0x000F;
constexpr uint16_t XUSB_DPAD_BITS = 0x000F;
// Every XUSB bit the table must account for: the ten buttons the pads share.
// The d-pad is the hat, the mute bit is the DualSense's alone.
constexpr uint16_t XUSB_MAPPED_BITS = 0xF000 | 0x0300 | 0x0030 | 0x00C0;

// The button word as each packer writes it: bytes 5..6 of the DS4 report,
// bytes 7..8 of the DS5 payload.
uint16_t ds4ButtonWord(uint16_t wButtons, uint8_t lt, uint8_t rt) {
    Ds4InputState st{};
    st.pad.wButtons = wButtons;
    st.pad.bLeftTrigger = lt;
    st.pad.bRightTrigger = rt;
    uint8_t out[DS4V2_INPUT_REPORT_BYTES];
    ds4PackInputReport(st, out);
    return satellite::readLE16(out + 5);
}

uint16_t ds5ButtonWord(uint16_t wButtons, uint8_t lt, uint8_t rt) {
    Ds5InputState st{};
    st.pad.wButtons = wButtons;
    st.pad.bLeftTrigger = lt;
    st.pad.bRightTrigger = rt;
    uint8_t out[DS5_PAYLOAD_BYTES];
    packDs5Payload(st, out);
    return static_cast<uint16_t>(out[7] | (out[8] << 8));
}

} // namespace

int main() {
    {
        TEST("the table maps each shared XUSB button once, to its own Sony bit above the hat");
        EXPECT_EQ(MAPPED_BUTTONS, static_cast<size_t>(10));
        uint16_t xusbSeen = 0;
        uint16_t sonySeen = 0;
        for (const auto& m : XUSB_TO_SONY_BUTTONS) {
            EXPECT((xusbSeen & m.xusb) == 0);
            EXPECT((sonySeen & m.sony) == 0);
            EXPECT((m.sony & HAT_NIBBLE) == 0);
            EXPECT((m.sony & (SONY_BUTTON_L2 | SONY_BUTTON_R2)) == 0);
            EXPECT((m.xusb & XUSB_DPAD_BITS) == 0);
            xusbSeen |= m.xusb;
            sonySeen |= m.sony;
        }
        EXPECT_EQ(xusbSeen, XUSB_MAPPED_BITS);
    }
    {
        TEST("the hat rides in the low nibble and nothing else touches it");
        EXPECT_EQ(sonyButtonsFromXusb(0x0001, 0, 0) & HAT_NIBBLE, 0); // up
        EXPECT_EQ(sonyButtonsFromXusb(0x0009, 0, 0) & HAT_NIBBLE, 1); // up + right
        EXPECT_EQ(sonyButtonsFromXusb(0x0000, 0, 0) & HAT_NIBBLE, 8); // released
        EXPECT_EQ(sonyButtonsFromXusb(0xFFF0, 255, 255) & HAT_NIBBLE, 8);
    }
    {
        TEST("the digital L2/R2 bits follow the analog triggers, any pressure at all");
        EXPECT_EQ(sonyButtonsFromXusb(0, 0, 0) & (SONY_BUTTON_L2 | SONY_BUTTON_R2), 0);
        EXPECT_EQ(sonyButtonsFromXusb(0, 1, 0) & (SONY_BUTTON_L2 | SONY_BUTTON_R2), SONY_BUTTON_L2);
        EXPECT_EQ(sonyButtonsFromXusb(0, 0, 1) & (SONY_BUTTON_L2 | SONY_BUTTON_R2), SONY_BUTTON_R2);
    }
    {
        TEST("the DualShock 4 report and the DualSense payload agree on every single button");
        for (int bit = 0; bit < 16; bit++) {
            const uint16_t b = static_cast<uint16_t>(1u << bit);
            EXPECT_EQ(ds4ButtonWord(b, 0, 0), ds5ButtonWord(b, 0, 0));
        }
        EXPECT_EQ(ds4ButtonWord(0, 90, 0), ds5ButtonWord(0, 90, 0));
        EXPECT_EQ(ds4ButtonWord(0, 0, 3), ds5ButtonWord(0, 0, 3));
    }
    {
        TEST("and on combinations, including every mapped button at once");
        const std::vector<uint16_t> combos = {0x1000 | 0x8000 | 0x0100 | 0x0020 | 0x0008, 0xF000,
                                              0x0300 | 0x00C0, XUSB_MAPPED_BITS | 0x0005, 0xFFFF};
        for (const uint16_t b : combos) {
            EXPECT_EQ(ds4ButtonWord(b, 200, 200), ds5ButtonWord(b, 200, 200));
        }
    }
    {
        TEST("the word both packers write is the table's, not a coincidence of the test");
        // Cross + Triangle + L1 + Create + dpad-right, R2 pressed: hat E, and the bits the
        // HIDMaestro suite already pins for the DualSense.
        const uint16_t word =
            sonyButtonsFromXusb(0x1000 | 0x8000 | 0x0100 | 0x0020 | 0x0008, 0, 55);
        EXPECT_EQ(word & 0xFF, 2 | 0x20 | 0x80);
        EXPECT_EQ(word >> 8, 0x01 | 0x08 | 0x10);
        EXPECT_EQ(ds4ButtonWord(0x1000 | 0x8000 | 0x0100 | 0x0020 | 0x0008, 0, 55), word);
    }

    std::cout << "test_sony_buttons: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
