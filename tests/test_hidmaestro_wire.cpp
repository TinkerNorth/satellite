// SPDX-License-Identifier: LGPL-3.0-or-later

// platform/windows/hidmaestro_wire — the seqlock input-frame packer for the
// HIDMaestro shared-memory section. Verifies the byte-layout contract with
// driver/driver.h, capacity guards, seqlock progression, and the legacy↔extended
// mode switch. Pure (no <windows.h>), so it runs on every CI platform.
#include "../src/platform/windows/hidmaestro_provisioner.h" // seam must keep parsing
#include "../src/platform/windows/hidmaestro_wire.h"

#include <array>
#include <cstring>
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
                      << "  " << #a << " == " << #b << "  (got " << _a << " vs " << _b << ")\n";   \
        }                                                                                          \
    } while (0)

using namespace satellite::hidmaestro;

static uint32_t u32at(const uint8_t* sec, size_t off) {
    uint32_t v;
    std::memcpy(&v, sec + off, sizeof(v));
    return v;
}

// Layout must match driver/driver.h byte-for-byte; a drift here is a silent
// wire break with the real driver, so pin every offset.
static void test_layout_offsets() {
    TEST("input section offsets match the driver contract");
    EXPECT_EQ(INPUT_SEQNO_OFFSET, (size_t)0);
    EXPECT_EQ(INPUT_DATASIZE_OFFSET, (size_t)4);
    EXPECT_EQ(INPUT_DATA_OFFSET, (size_t)8);
    EXPECT_EQ(INPUT_DATA_CAPACITY, (size_t)256);
    EXPECT_EQ(INPUT_GIP_OFFSET, (size_t)264);
    EXPECT_EQ(INPUT_GIP_LENGTH, (size_t)14);
    EXPECT_EQ(INPUT_EXT_SIZE_OFFSET, (size_t)278);
    EXPECT_EQ(INPUT_EXT_DATA_OFFSET, (size_t)282);
    EXPECT_EQ(INPUT_EXT_DATA_CAPACITY, (size_t)80);
    EXPECT_EQ(INPUT_SECTION_SIZE, (size_t)362);
    // Data and extended regions must not overlap their neighbours.
    EXPECT(INPUT_DATA_OFFSET + INPUT_DATA_CAPACITY <= INPUT_GIP_OFFSET);
    EXPECT(INPUT_EXT_DATA_OFFSET + INPUT_EXT_DATA_CAPACITY <= INPUT_SECTION_SIZE);
}

static void test_legacy_frame_seqlock_and_payload() {
    TEST("writeInputFrame — seqlock 0→2, DataSize + Data set, ExtSize cleared");
    std::array<uint8_t, INPUT_SECTION_SIZE> sec{};
    // Pre-stain ExtendedReportSize so we can prove the legacy path clears it.
    std::memset(sec.data() + INPUT_EXT_SIZE_OFFSET, 0xFF, 4);

    const uint8_t report[] = {0x10, 0x20, 0x30, 0x40};
    EXPECT(writeInputFrame(sec.data(), report, sizeof(report)));

    EXPECT_EQ(readSeqNo(sec.data()), (uint32_t)2); // even = complete
    EXPECT_EQ(u32at(sec.data(), INPUT_DATASIZE_OFFSET), (uint32_t)sizeof(report));
    EXPECT_EQ(std::memcmp(sec.data() + INPUT_DATA_OFFSET, report, sizeof(report)), 0);
    EXPECT_EQ(u32at(sec.data(), INPUT_EXT_SIZE_OFFSET), (uint32_t)0);
}

static void test_seqlock_advances_each_write() {
    TEST("writeInputFrame — counter advances by 2 per frame, stays even");
    std::array<uint8_t, INPUT_SECTION_SIZE> sec{};
    const uint8_t r[] = {1, 2, 3};
    EXPECT(writeInputFrame(sec.data(), r, sizeof(r)));
    EXPECT_EQ(readSeqNo(sec.data()), (uint32_t)2);
    EXPECT(writeInputFrame(sec.data(), r, sizeof(r)));
    EXPECT_EQ(readSeqNo(sec.data()), (uint32_t)4);
    EXPECT(writeInputFrame(sec.data(), r, sizeof(r)));
    EXPECT_EQ(readSeqNo(sec.data()), (uint32_t)6);
    EXPECT_EQ(readSeqNo(sec.data()) % 2, (uint32_t)0);
}

static void test_legacy_capacity_guard() {
    TEST("writeInputFrame — oversized report rejected, section untouched");
    std::array<uint8_t, INPUT_SECTION_SIZE> sec{};
    std::array<uint8_t, INPUT_DATA_CAPACITY + 1> big{};
    EXPECT(!writeInputFrame(sec.data(), big.data(), (uint16_t)big.size()));
    EXPECT_EQ(readSeqNo(sec.data()), (uint32_t)0); // no write happened
    // Exactly at capacity is allowed.
    EXPECT(writeInputFrame(sec.data(), big.data(), (uint16_t)INPUT_DATA_CAPACITY));
    EXPECT_EQ(u32at(sec.data(), INPUT_DATASIZE_OFFSET), (uint32_t)INPUT_DATA_CAPACITY);
}

static void test_extended_frame() {
    TEST("writeExtendedInputFrame — ExtSize + ExtData set, seqlock advances");
    std::array<uint8_t, INPUT_SECTION_SIZE> sec{};
    const uint8_t ext[] = {0x31, 0xAA, 0xBB, 0xCC, 0xDD};
    EXPECT(writeExtendedInputFrame(sec.data(), ext, sizeof(ext)));
    EXPECT_EQ(readSeqNo(sec.data()), (uint32_t)2);
    EXPECT_EQ(u32at(sec.data(), INPUT_EXT_SIZE_OFFSET), (uint32_t)sizeof(ext));
    EXPECT_EQ(std::memcmp(sec.data() + INPUT_EXT_DATA_OFFSET, ext, sizeof(ext)), 0);
}

static void test_extended_capacity_guard() {
    TEST("writeExtendedInputFrame — oversized extended report rejected");
    std::array<uint8_t, INPUT_SECTION_SIZE> sec{};
    std::array<uint8_t, INPUT_EXT_DATA_CAPACITY + 1> big{};
    EXPECT(!writeExtendedInputFrame(sec.data(), big.data(), (uint16_t)big.size()));
    EXPECT_EQ(readSeqNo(sec.data()), (uint32_t)0);
}

// The documented mode-switch bug: an extended arming followed by a legacy frame
// must clear ExtendedReportSize, else the driver reuses stale extended bytes.
static void test_mode_switch_clears_extended() {
    TEST("legacy frame after extended clears ExtendedReportSize");
    std::array<uint8_t, INPUT_SECTION_SIZE> sec{};
    const uint8_t ext[] = {0x31, 0x01, 0x02};
    EXPECT(writeExtendedInputFrame(sec.data(), ext, sizeof(ext)));
    EXPECT(u32at(sec.data(), INPUT_EXT_SIZE_OFFSET) > 0);

    const uint8_t legacy[] = {0x05, 0x06};
    EXPECT(writeInputFrame(sec.data(), legacy, sizeof(legacy)));
    EXPECT_EQ(u32at(sec.data(), INPUT_EXT_SIZE_OFFSET), (uint32_t)0);
    EXPECT_EQ(readSeqNo(sec.data()), (uint32_t)4);
}

static void test_zero_length_frame() {
    TEST("writeInputFrame — zero-length report is a valid (empty) frame");
    std::array<uint8_t, INPUT_SECTION_SIZE> sec{};
    EXPECT(writeInputFrame(sec.data(), nullptr, 0));
    EXPECT_EQ(u32at(sec.data(), INPUT_DATASIZE_OFFSET), (uint32_t)0);
    EXPECT_EQ(readSeqNo(sec.data()), (uint32_t)2);
}

int main() {
    test_layout_offsets();
    test_legacy_frame_seqlock_and_payload();
    test_seqlock_advances_each_write();
    test_legacy_capacity_guard();
    test_extended_frame();
    test_extended_capacity_guard();
    test_mode_switch_clears_extended();
    test_zero_length_frame();

    std::cout << "hidmaestro_wire: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
