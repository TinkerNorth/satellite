// SPDX-License-Identifier: LGPL-3.0-or-later

// platform/windows/hidmaestro_wire — the seqlock input-frame writer and
// output-ring reader for the HIDMaestro shared-memory sections. Verifies the
// byte-layout contract with the driver, capacity guards, seqlock progression,
// the legacy<->extended mode switch, the GIP slice, and the ring's cursor /
// lap-skip / torn-slot rules. Pure (no <windows.h>), so it runs on every CI
// platform.
#include "test_util.h"

#include "../src/platform/windows/hidmaestro_provisioner.h" // seam must keep parsing
#include "../src/platform/windows/hidmaestro_wire.h"

#include <array>
#include <cstring>
#include <string>
#include <vector>

using namespace satellite::hidmaestro;

static uint32_t u32at(const uint8_t* sec, size_t off) {
    uint32_t v;
    std::memcpy(&v, sec + off, sizeof(v));
    return v;
}

static void storeU32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, sizeof(v)); }
static void storeU16(uint8_t* p, uint16_t v) { std::memcpy(p, &v, sizeof(v)); }

// Layout must match the driver byte-for-byte; a drift here is a silent wire
// break with the real driver, so pin every offset.
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
    EXPECT(INPUT_DATA_OFFSET + INPUT_DATA_CAPACITY <= INPUT_GIP_OFFSET);
    EXPECT(INPUT_EXT_DATA_OFFSET + INPUT_EXT_DATA_CAPACITY <= INPUT_SECTION_SIZE);

    TEST("output ring offsets match the driver contract");
    EXPECT_EQ(OUTPUT_RING_SLOTS, (size_t)64);
    EXPECT_EQ(OUTPUT_HEADER_SIZE, (size_t)8);
    EXPECT_EQ(OUTPUT_SLOT_SIZE, (size_t)264);
    EXPECT_EQ(OUTPUT_SECTION_SIZE, (size_t)16904);
    EXPECT_EQ(OUTPUT_SLOT_SEQNO_OFFSET, (size_t)0);
    EXPECT_EQ(OUTPUT_SLOT_SOURCE_OFFSET, (size_t)4);
    EXPECT_EQ(OUTPUT_SLOT_REPORT_ID_OFFSET, (size_t)5);
    EXPECT_EQ(OUTPUT_SLOT_SIZE_OFFSET, (size_t)6);
    EXPECT_EQ(OUTPUT_SLOT_DATA_OFFSET, (size_t)8);
}

static void test_legacy_frame_seqlock_and_payload() {
    TEST("writeInputFrame — seqlock 0->2, DataSize + Data set, ExtSize cleared");
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

static void test_gip_slice() {
    TEST("writeInputFrame — GIP slice written when given, untouched when null");
    std::array<uint8_t, INPUT_SECTION_SIZE> sec{};
    uint8_t gip[INPUT_GIP_LENGTH];
    for (size_t i = 0; i < INPUT_GIP_LENGTH; i++) gip[i] = static_cast<uint8_t>(0xA0 + i);

    const uint8_t report[] = {1, 2, 3};
    EXPECT(writeInputFrame(sec.data(), report, sizeof(report), gip));
    EXPECT_EQ(std::memcmp(sec.data() + INPUT_GIP_OFFSET, gip, INPUT_GIP_LENGTH), 0);

    // A gip-less frame (non-Xbox profile on the same section) leaves the
    // previous GIP bytes alone — the driver never reads them for such
    // profiles, and skipping the copy is the SDK's own behaviour.
    EXPECT(writeInputFrame(sec.data(), report, sizeof(report)));
    EXPECT_EQ(std::memcmp(sec.data() + INPUT_GIP_OFFSET, gip, INPUT_GIP_LENGTH), 0);
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

// ── output ring ────────────────────────────────────────────────────────────

// Publish one packet the way the driver does: reserve Head, fill the slot,
// publish the slot SeqNo last.
static void ringPublish(uint8_t* sec, uint8_t source, uint8_t reportId,
                        const std::vector<uint8_t>& data) {
    const uint32_t newSeq = u32at(sec, 0) + 1;
    storeU32(sec, newSeq);
    uint8_t* slot =
        sec + OUTPUT_HEADER_SIZE + ((newSeq - 1) % OUTPUT_RING_SLOTS) * OUTPUT_SLOT_SIZE;
    slot[OUTPUT_SLOT_SOURCE_OFFSET] = source;
    slot[OUTPUT_SLOT_REPORT_ID_OFFSET] = reportId;
    storeU16(slot + OUTPUT_SLOT_SIZE_OFFSET, static_cast<uint16_t>(data.size()));
    if (!data.empty()) std::memcpy(slot + OUTPUT_SLOT_DATA_OFFSET, data.data(), data.size());
    storeU32(slot + OUTPUT_SLOT_SEQNO_OFFSET, newSeq);
}

static void test_ring_reader_basics() {
    TEST("readNextOutputPacket — drains published packets in order");
    std::vector<uint8_t> sec(OUTPUT_SECTION_SIZE, 0);
    uint32_t cursor = 0;
    OutputPacket pkt;
    EXPECT(!readNextOutputPacket(sec.data(), cursor, pkt)); // empty ring

    ringPublish(sec.data(), OUTPUT_SOURCE_XINPUT, 0, {0x00, 0x08, 0x40, 0x80, 0x00});
    ringPublish(sec.data(), OUTPUT_SOURCE_HID_OUTPUT, 0x05, {0x01, 0, 0, 0x11, 0x22});

    EXPECT(readNextOutputPacket(sec.data(), cursor, pkt));
    EXPECT_EQ(pkt.source, OUTPUT_SOURCE_XINPUT);
    EXPECT_EQ(pkt.size, (uint16_t)5);
    EXPECT_EQ(pkt.data[2], (uint8_t)0x40);
    EXPECT_EQ(pkt.data[3], (uint8_t)0x80);

    EXPECT(readNextOutputPacket(sec.data(), cursor, pkt));
    EXPECT_EQ(pkt.source, OUTPUT_SOURCE_HID_OUTPUT);
    EXPECT_EQ(pkt.reportId, (uint8_t)0x05);
    EXPECT_EQ(pkt.data[3], (uint8_t)0x11);

    EXPECT(!readNextOutputPacket(sec.data(), cursor, pkt)); // drained
    EXPECT_EQ(cursor, (uint32_t)2);
}

static void test_ring_reader_lap_skip() {
    TEST("readNextOutputPacket — a lapped reader skips to the oldest readable");
    std::vector<uint8_t> sec(OUTPUT_SECTION_SIZE, 0);
    uint32_t cursor = 0;
    for (uint32_t i = 0; i < OUTPUT_RING_SLOTS + 10; i++) {
        ringPublish(sec.data(), OUTPUT_SOURCE_HID_OUTPUT, 0x05, {static_cast<uint8_t>(i & 0xFF)});
    }
    OutputPacket pkt;
    EXPECT(readNextOutputPacket(sec.data(), cursor, pkt));
    // Head = 74; oldest readable = 74 - 64 + 1 = 11.
    EXPECT_EQ(cursor, (uint32_t)11);
    EXPECT_EQ(pkt.data[0], (uint8_t)10);
    int drained = 1;
    while (readNextOutputPacket(sec.data(), cursor, pkt)) drained++;
    EXPECT_EQ(drained, (int)OUTPUT_RING_SLOTS);
    EXPECT_EQ(cursor, (uint32_t)(OUTPUT_RING_SLOTS + 10));
}

static void test_ring_reader_reserved_but_unpublished() {
    TEST("readNextOutputPacket — a reserved-but-unwritten slot reads as no data");
    std::vector<uint8_t> sec(OUTPUT_SECTION_SIZE, 0);
    uint32_t cursor = 0;
    // Producer bumped Head but hasn't published the slot SeqNo yet.
    storeU32(sec.data(), 1);
    OutputPacket pkt;
    EXPECT(!readNextOutputPacket(sec.data(), cursor, pkt));
    EXPECT_EQ(cursor, (uint32_t)0); // cursor unchanged; retried on next wake
}

static void test_ring_reader_size_clamp() {
    TEST("readNextOutputPacket — corrupt DataSize clamped to slot capacity");
    std::vector<uint8_t> sec(OUTPUT_SECTION_SIZE, 0);
    uint32_t cursor = 0;
    ringPublish(sec.data(), OUTPUT_SOURCE_HID_OUTPUT, 0x02, {});
    uint8_t* slot = sec.data() + OUTPUT_HEADER_SIZE;
    storeU16(slot + OUTPUT_SLOT_SIZE_OFFSET, 0xFFFF);
    OutputPacket pkt;
    EXPECT(readNextOutputPacket(sec.data(), cursor, pkt));
    EXPECT_EQ(pkt.size, (uint16_t)OUTPUT_SLOT_DATA_CAPACITY);
}

int main() {
    test_layout_offsets();
    test_legacy_frame_seqlock_and_payload();
    test_gip_slice();
    test_seqlock_advances_each_write();
    test_legacy_capacity_guard();
    test_extended_frame();
    test_extended_capacity_guard();
    test_mode_switch_clears_extended();
    test_zero_length_frame();
    test_ring_reader_basics();
    test_ring_reader_lap_skip();
    test_ring_reader_reserved_but_unpublished();
    test_ring_reader_size_clamp();

    std::cout << "hidmaestro_wire: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
