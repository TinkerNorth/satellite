// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * tests/test_dsu_protocol.cpp — Unit tests for the pure Cemuhook DSU
 * protocol encoders/decoders (src/net/dsu_protocol.cpp).
 *
 * Self-contained: no external test framework required (matches the existing
 * test_session_service.cpp / test_*_platform.cpp shape so the CI runner can
 * just exec ./test_dsu_protocol).
 */
#include "../src/net/dsu_protocol.h"

#include <cassert>
#include <cstring>
#include <cstdio>
#include <iostream>
#include <sstream>

// ── Counters & state for assertions ─────────────────────────────────────────
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

static uint32_t readLE32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

static uint16_t readLE16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

static float readFloatLE(const uint8_t* p) {
    uint32_t bits = readLE32(p);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

// ── CRC32 ───────────────────────────────────────────────────────────────────

static void test_crc32_empty() {
    TEST("crc32 of empty buffer is 0");
    EXPECT_EQ(dsu::crc32(nullptr, 0), 0u);
}

static void test_crc32_known_vector() {
    TEST("crc32 matches IEEE 802.3 test vector");
    // CRC32 of ASCII "123456789" = 0xCBF43926. This is the standard
    // vector every CRC32 impl is checked against; if it passes here we
    // know the polynomial + init/final XOR are correct.
    const char* s = "123456789";
    EXPECT_EQ(dsu::crc32(reinterpret_cast<const uint8_t*>(s), 9), 0xCBF43926u);
}

static void test_crc32_two_byte_change() {
    TEST("crc32 differs when input differs");
    uint8_t a[] = {0x11, 0x22, 0x33};
    uint8_t b[] = {0x11, 0x22, 0x34};
    EXPECT(dsu::crc32(a, 3) != dsu::crc32(b, 3));
}

// ── Header ──────────────────────────────────────────────────────────────────

static void test_encodeServerHeader_magic_and_version() {
    TEST("encodeServerHeader writes magic + version + length");
    uint8_t buf[64] = {};
    dsu::encodeServerHeader(buf, /*totalLen=*/40, /*serverId=*/0xDEADBEEF);
    EXPECT(std::memcmp(buf, "DSUS", 4) == 0);
    EXPECT_EQ(readLE16(buf + 4), dsu::PROTOCOL_VERSION);
    // Length field = totalLen - 8, per spec.
    EXPECT_EQ(readLE16(buf + 6), 32u);
    // CRC stays zero until finaliseCrc runs.
    EXPECT_EQ(readLE32(buf + 8), 0u);
    EXPECT_EQ(readLE32(buf + 12), 0xDEADBEEFu);
}

static void test_finaliseCrc_stamps_and_zeros() {
    TEST("finaliseCrc stamps CRC over zeroed CRC field");
    uint8_t buf[20] = {};
    dsu::encodeServerHeader(buf, 20, 0x12345678);
    // Pollute CRC area with non-zero to verify finaliseCrc clears it first.
    buf[8] = 0xFF;
    buf[10] = 0xFF;
    dsu::finaliseCrc(buf, 20);
    const uint32_t stamped = readLE32(buf + 8);
    EXPECT(stamped != 0);
    // Recompute with CRC zeroed should yield the same value.
    uint8_t scratch[20];
    std::memcpy(scratch, buf, 20);
    scratch[8] = scratch[9] = scratch[10] = scratch[11] = 0;
    EXPECT_EQ(dsu::crc32(scratch, 20), stamped);
}

// ── Version response ────────────────────────────────────────────────────────

static void test_encodeVersionResponse_packetSize() {
    TEST("encodeVersionResponse writes 24 bytes total");
    uint8_t buf[64];
    const size_t n = dsu::encodeVersionResponse(buf, sizeof(buf), 0xAABBCCDD);
    EXPECT_EQ(n, 24u);
}

static void test_encodeVersionResponse_eventAndVersion() {
    TEST("encodeVersionResponse encodes event type + version field");
    uint8_t buf[64];
    dsu::encodeVersionResponse(buf, sizeof(buf), 0x12345678);
    EXPECT_EQ(readLE32(buf + 16), dsu::EVENT_VERSION);
    EXPECT_EQ(readLE16(buf + 20), dsu::PROTOCOL_VERSION);
}

static void test_encodeVersionResponse_bufferTooSmall() {
    TEST("encodeVersionResponse rejects undersized buffers");
    uint8_t small[10];
    EXPECT_EQ(dsu::encodeVersionResponse(small, sizeof(small), 0), 0u);
}

// ── Information response ────────────────────────────────────────────────────

static void test_encodeInformationResponse_connected() {
    TEST("encodeInformationResponse — connected pad reports correct fields");
    uint8_t buf[64];
    std::array<uint8_t, 6> mac{0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01};
    const size_t n = dsu::encodeInformationResponse(buf, sizeof(buf), 1, /*slot=*/2,
                                                    /*connected=*/true, mac);
    EXPECT_EQ(n, 32u);
    EXPECT_EQ(readLE32(buf + 16), dsu::EVENT_INFORMATION);
    EXPECT_EQ(buf[20], 2);                                  // slot index
    EXPECT_EQ(buf[21], dsu::SLOT_STATE_CONNECTED);
    EXPECT_EQ(buf[22], dsu::SLOT_MODEL_FULL);
    EXPECT_EQ(buf[23], dsu::SLOT_CONNECTION_BLUETOOTH);
    EXPECT(std::memcmp(buf + 24, mac.data(), 6) == 0);
    EXPECT_EQ(buf[30], dsu::SLOT_BATTERY_FULL);
    EXPECT_EQ(buf[31], 0);                                  // terminator
}

static void test_encodeInformationResponse_disconnected() {
    TEST("encodeInformationResponse — disconnected pad zeros the fields");
    uint8_t buf[64];
    std::array<uint8_t, 6> mac{};
    dsu::encodeInformationResponse(buf, sizeof(buf), 0, /*slot=*/3, /*connected=*/false, mac);
    EXPECT_EQ(buf[20], 3);
    EXPECT_EQ(buf[21], dsu::SLOT_STATE_DISCONNECTED);
    EXPECT_EQ(buf[22], dsu::SLOT_MODEL_NONE);
    EXPECT_EQ(buf[23], dsu::SLOT_CONNECTION_NONE);
    EXPECT_EQ(buf[30], dsu::SLOT_BATTERY_NONE);
}

// ── Pad Data response ───────────────────────────────────────────────────────

static void test_encodePadDataResponse_packetSize() {
    TEST("encodePadDataResponse writes 100 bytes total");
    uint8_t buf[128];
    dsu::PadDataInputs inputs;
    inputs.slotIndex = 0;
    inputs.connected = true;
    const size_t n = dsu::encodePadDataResponse(buf, sizeof(buf), 0xFFFFFFFF, inputs);
    EXPECT_EQ(n, dsu::PAD_DATA_PACKET_SIZE);
    EXPECT_EQ(n, 100u);
}

static void test_encodePadDataResponse_event_and_slot() {
    TEST("encodePadDataResponse encodes event type + slot bookkeeping");
    uint8_t buf[128];
    dsu::PadDataInputs inputs;
    inputs.slotIndex = 1;
    inputs.connected = true;
    inputs.packetNumber = 42;
    inputs.battery = dsu::SLOT_BATTERY_HIGH;
    dsu::encodePadDataResponse(buf, sizeof(buf), 0, inputs);
    EXPECT_EQ(readLE32(buf + 16), dsu::EVENT_PAD_DATA);
    EXPECT_EQ(buf[20], 1);                                  // slot
    EXPECT_EQ(buf[21], dsu::SLOT_STATE_CONNECTED);
    EXPECT_EQ(buf[30], dsu::SLOT_BATTERY_HIGH);
    EXPECT_EQ(buf[31], 1);                                  // connected
    EXPECT_EQ(readLE32(buf + 32), 42u);                     // packet number
}

static void test_encodePadDataResponse_motion_passes_through() {
    TEST("encodePadDataResponse encodes accel + gyro as float LE");
    uint8_t buf[128];
    dsu::PadDataInputs inputs;
    inputs.connected = true;
    inputs.accelGX = 1.0f;
    inputs.accelGY = -0.5f;
    inputs.accelGZ = 0.25f;
    inputs.gyroPitch = 90.0f;
    inputs.gyroYaw = -45.0f;
    inputs.gyroRoll = 22.5f;
    dsu::encodePadDataResponse(buf, sizeof(buf), 0, inputs);
    // Accel @ 76..87 (after the +20 event-type offset → 56..67).
    EXPECT_EQ(readFloatLE(buf + 76), 1.0f);
    EXPECT_EQ(readFloatLE(buf + 80), -0.5f);
    EXPECT_EQ(readFloatLE(buf + 84), 0.25f);
    // Gyro @ 88..99.
    EXPECT_EQ(readFloatLE(buf + 88), 90.0f);
    EXPECT_EQ(readFloatLE(buf + 92), -45.0f);
    EXPECT_EQ(readFloatLE(buf + 96), 22.5f);
}

static void test_applyMotionReport_converts_scale() {
    TEST("applyMotionReport converts MotionReport int16 → DSU float units");
    dsu::PadDataInputs inputs;
    MotionReport r{};
    // Full positive int16 should land at the documented scale max:
    // gyro_max_deg_s = 32767 * (2000/32767) = 2000.0
    // accel_max_g    = 32767 * (4/32767)    = 4.0
    r.gyroX = 32767;
    r.gyroY = 0;
    r.gyroZ = -32767;
    r.accelX = 32767;
    r.accelY = 0;
    r.accelZ = -32767;
    dsu::applyMotionReport(inputs, r);
    EXPECT(inputs.gyroPitch > 1999.0f && inputs.gyroPitch <= 2000.5f);
    EXPECT(inputs.gyroYaw == 0.0f);
    EXPECT(inputs.gyroRoll < -1999.0f && inputs.gyroRoll >= -2000.5f);
    EXPECT(inputs.accelGX > 3.99f && inputs.accelGX <= 4.01f);
    EXPECT(inputs.accelGY == 0.0f);
    EXPECT(inputs.accelGZ < -3.99f && inputs.accelGZ >= -4.01f);
}

// ── Client request parsing ──────────────────────────────────────────────────

// Build a minimal client packet for parseClientHeader testing.
static size_t buildClientPacket(uint8_t* out, uint32_t eventType, uint32_t clientId,
                                 const uint8_t* tail, size_t tailLen) {
    const size_t total = dsu::HEADER_SIZE + 4 + tailLen;
    std::memcpy(out, "DSUC", 4);
    out[4] = static_cast<uint8_t>(dsu::PROTOCOL_VERSION);
    out[5] = static_cast<uint8_t>(dsu::PROTOCOL_VERSION >> 8);
    const uint16_t advertised = static_cast<uint16_t>(total - 8);
    out[6] = static_cast<uint8_t>(advertised);
    out[7] = static_cast<uint8_t>(advertised >> 8);
    out[8] = out[9] = out[10] = out[11] = 0;
    out[12] = static_cast<uint8_t>(clientId);
    out[13] = static_cast<uint8_t>(clientId >> 8);
    out[14] = static_cast<uint8_t>(clientId >> 16);
    out[15] = static_cast<uint8_t>(clientId >> 24);
    out[16] = static_cast<uint8_t>(eventType);
    out[17] = static_cast<uint8_t>(eventType >> 8);
    out[18] = static_cast<uint8_t>(eventType >> 16);
    out[19] = static_cast<uint8_t>(eventType >> 24);
    if (tailLen > 0) std::memcpy(out + 20, tail, tailLen);
    // Stamp CRC last.
    const uint32_t c = dsu::crc32(out, total);
    out[8] = static_cast<uint8_t>(c);
    out[9] = static_cast<uint8_t>(c >> 8);
    out[10] = static_cast<uint8_t>(c >> 16);
    out[11] = static_cast<uint8_t>(c >> 24);
    return total;
}

static void test_parseClientHeader_returnsEventType() {
    TEST("parseClientHeader returns the event type on valid input");
    uint8_t pkt[32];
    const size_t n = buildClientPacket(pkt, dsu::EVENT_VERSION, 0xCAFEBABE, nullptr, 0);
    EXPECT_EQ(dsu::parseClientHeader(pkt, n), dsu::EVENT_VERSION);
}

static void test_parseClientHeader_rejectsBadMagic() {
    TEST("parseClientHeader rejects packets without the DSUC magic");
    uint8_t pkt[32];
    buildClientPacket(pkt, dsu::EVENT_VERSION, 0, nullptr, 0);
    pkt[0] = 'X'; // mangle magic
    EXPECT_EQ(dsu::parseClientHeader(pkt, 20), 0u);
}

static void test_parseClientHeader_rejectsTruncated() {
    TEST("parseClientHeader rejects packets that don't reach the header size");
    uint8_t pkt[10] = {};
    EXPECT_EQ(dsu::parseClientHeader(pkt, sizeof(pkt)), 0u);
}

static void test_parseClientHeader_rejectsBadCrc() {
    TEST("parseClientHeader rejects packets with the wrong CRC");
    uint8_t pkt[32];
    const size_t n = buildClientPacket(pkt, dsu::EVENT_VERSION, 0, nullptr, 0);
    pkt[8] ^= 0x01; // flip a bit in the CRC
    EXPECT_EQ(dsu::parseClientHeader(pkt, n), 0u);
}

static void test_parseSubscriptionRequest_wantAll() {
    TEST("parseSubscriptionRequest with flags=0 means subscribe-all");
    uint8_t pkt[32];
    uint8_t tail[11] = {}; // all zeros = match all
    const size_t n = buildClientPacket(pkt, dsu::EVENT_PAD_DATA, 0, tail, sizeof(tail));
    dsu::SubscriptionRequest req;
    EXPECT(dsu::parseSubscriptionRequest(pkt, n, req));
    EXPECT(req.wantAllSlots);
}

static void test_parseSubscriptionRequest_specificSlot() {
    TEST("parseSubscriptionRequest decodes slot index from tail");
    uint8_t pkt[32];
    uint8_t tail[11] = {0x02, 0, 0, 0, /*slot=*/3, 0, 0, 0, 0, 0, 0};
    const size_t n = buildClientPacket(pkt, dsu::EVENT_PAD_DATA, 0, tail, sizeof(tail));
    dsu::SubscriptionRequest req;
    EXPECT(dsu::parseSubscriptionRequest(pkt, n, req));
    EXPECT(!req.wantAllSlots);
    EXPECT_EQ(req.slotIndex, 3);
}

// ── Driver ──────────────────────────────────────────────────────────────────

int main() {
    std::cout << "Running DSU protocol tests...\n\n";

    test_crc32_empty();
    test_crc32_known_vector();
    test_crc32_two_byte_change();

    test_encodeServerHeader_magic_and_version();
    test_finaliseCrc_stamps_and_zeros();

    test_encodeVersionResponse_packetSize();
    test_encodeVersionResponse_eventAndVersion();
    test_encodeVersionResponse_bufferTooSmall();

    test_encodeInformationResponse_connected();
    test_encodeInformationResponse_disconnected();

    test_encodePadDataResponse_packetSize();
    test_encodePadDataResponse_event_and_slot();
    test_encodePadDataResponse_motion_passes_through();
    test_applyMotionReport_converts_scale();

    test_parseClientHeader_returnsEventType();
    test_parseClientHeader_rejectsBadMagic();
    test_parseClientHeader_rejectsTruncated();
    test_parseClientHeader_rejectsBadCrc();
    test_parseSubscriptionRequest_wantAll();
    test_parseSubscriptionRequest_specificSlot();

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
