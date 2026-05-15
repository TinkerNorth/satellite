// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * tests/test_mdns_protocol.cpp — unit tests for the pure mDNS encoders /
 * parsers in src/net/mdns_protocol.cpp.
 */
#include "../src/net/mdns_protocol.h"

#include <cstring>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

static int g_pass = 0;
static int g_fail = 0;
static std::string g_currentTest;

#define TEST(name)                                                                                 \
    do { g_currentTest = (name); } while (0)

#define EXPECT(cond)                                                                               \
    do {                                                                                           \
        if (cond) { g_pass++; } else {                                                             \
            g_fail++;                                                                              \
            std::cerr << "  FAIL [" << g_currentTest << "] " << __FILE__ << ":" << __LINE__        \
                      << "  " << #cond << "\n";                                                    \
        }                                                                                          \
    } while (0)

#define EXPECT_EQ(a, b)                                                                            \
    do {                                                                                           \
        auto _a = (a);                                                                             \
        auto _b = (b);                                                                             \
        if (_a == _b) { g_pass++; } else {                                                         \
            g_fail++;                                                                              \
            std::cerr << "  FAIL [" << g_currentTest << "] " << __FILE__ << ":" << __LINE__        \
                      << "  " << #a << " == " << #b << "  (got " << _a << " vs " << _b << ")\n";   \
        }                                                                                          \
    } while (0)

// ── DNS name encoding ───────────────────────────────────────────────────────

static void test_writeDnsName_basic() {
    TEST("writeDnsName encodes 'foo.bar.local.' as labelled-length form");
    uint8_t buf[64] = {};
    const size_t n = mdns::writeDnsName(buf, sizeof(buf), "foo.bar.local.");
    // 1B len + 3B "foo" + 1B len + 3B "bar" + 1B len + 5B "local" + 1B 0 = 15
    EXPECT_EQ(n, 15u);
    EXPECT_EQ(buf[0], 3);
    EXPECT_EQ(buf[4], 3);
    EXPECT_EQ(buf[8], 5);
    EXPECT_EQ(buf[14], 0); // terminator
    EXPECT(std::memcmp(buf + 1, "foo", 3) == 0);
    EXPECT(std::memcmp(buf + 5, "bar", 3) == 0);
    EXPECT(std::memcmp(buf + 9, "local", 5) == 0);
}

static void test_writeDnsName_emptyTerminatorOnly() {
    TEST("writeDnsName writes single terminator for empty name");
    uint8_t buf[8] = {0xFF};
    const size_t n = mdns::writeDnsName(buf, sizeof(buf), "");
    EXPECT_EQ(n, 1u);
    EXPECT_EQ(buf[0], 0);
}

static void test_writeDnsName_rejectsOversizedLabel() {
    TEST("writeDnsName rejects labels longer than 63 bytes");
    uint8_t buf[128];
    std::string longLabel(64, 'a');
    EXPECT_EQ(mdns::writeDnsName(buf, sizeof(buf), longLabel + ".local."), 0u);
}

static void test_writeDnsName_rejectsUndersizedBuffer() {
    TEST("writeDnsName rejects buffers too small for the encoded output");
    uint8_t buf[4];
    EXPECT_EQ(mdns::writeDnsName(buf, sizeof(buf), "foo.bar.local."), 0u);
}

// ── DNS name decoding ───────────────────────────────────────────────────────

static void test_readDnsName_roundTrip() {
    TEST("writeDnsName + readDnsName round-trip basic name");
    uint8_t buf[64];
    const size_t written = mdns::writeDnsName(buf, sizeof(buf), "_satellite._udp.local.");
    EXPECT(written > 0);
    std::string out;
    const size_t consumed = mdns::readDnsName(buf, written, 0, out);
    EXPECT_EQ(consumed, written);
    EXPECT_EQ(out, std::string("_satellite._udp.local."));
}

static void test_readDnsName_followsCompressionPointer() {
    TEST("readDnsName follows a backwards compression pointer");
    // Build a small packet: at offset 0 we have "foo.bar.", and at offset 9
    // we have a name "baz." followed by a compression pointer back to "bar."
    // which is at offset 4 (the second label in "foo.bar.").
    //
    // Layout:
    //   0: 0x03 'f' 'o' 'o' 0x03 'b' 'a' 'r' 0x00      (9 bytes, "foo.bar.")
    //   9: 0x03 'b' 'a' 'z' 0xC0 0x04                  (6 bytes, "baz." + ptr to byte 4)
    uint8_t packet[] = {
        3, 'f', 'o', 'o',
        3, 'b', 'a', 'r',
        0,
        3, 'b', 'a', 'z',
        0xC0, 0x04,
    };
    std::string out;
    const size_t consumed = mdns::readDnsName(packet, sizeof(packet), 9, out);
    EXPECT_EQ(consumed, 6u);
    EXPECT_EQ(out, std::string("baz.bar."));
}

static void test_readDnsName_rejectsForwardJump() {
    TEST("readDnsName rejects a forward / equal compression pointer (loop guard)");
    uint8_t packet[] = {0xC0, 0x00}; // pointer to itself
    std::string out;
    EXPECT_EQ(mdns::readDnsName(packet, sizeof(packet), 0, out), 0u);
}

// ── Packet parsing ──────────────────────────────────────────────────────────

static void test_parsePacket_singleQuestion() {
    TEST("parsePacket decodes a single PTR question for the service domain");
    // Hand-build a minimal mDNS query for _satellite._udp.local. PTR IN
    // (with QU = unicast-response bit).
    uint8_t buf[64];
    size_t pos = 0;
    // Header
    buf[pos++] = 0x12; buf[pos++] = 0x34;        // id
    buf[pos++] = 0x00; buf[pos++] = 0x00;        // flags (query)
    buf[pos++] = 0x00; buf[pos++] = 0x01;        // QDCOUNT
    buf[pos++] = 0x00; buf[pos++] = 0x00;        // ANCOUNT
    buf[pos++] = 0x00; buf[pos++] = 0x00;        // NSCOUNT
    buf[pos++] = 0x00; buf[pos++] = 0x00;        // ARCOUNT
    pos += mdns::writeDnsName(buf + pos, sizeof(buf) - pos, "_satellite._udp.local.");
    // QTYPE = PTR, QCLASS = IN | QU
    buf[pos++] = 0x00; buf[pos++] = 0x0C;
    buf[pos++] = 0x80; buf[pos++] = 0x01;

    mdns::Header h{};
    std::vector<mdns::Question> qs;
    EXPECT(mdns::parsePacket(buf, pos, h, qs));
    EXPECT_EQ(h.id, 0x1234);
    EXPECT_EQ(qs.size(), 1u);
    EXPECT_EQ(qs[0].name, std::string("_satellite._udp.local."));
    EXPECT_EQ(qs[0].type, mdns::TYPE_PTR);
    EXPECT_EQ(qs[0].cls, mdns::CLASS_IN);
    EXPECT(qs[0].unicastResponse);
}

static void test_parsePacket_rejectsTruncated() {
    TEST("parsePacket rejects packets shorter than 12-byte header");
    uint8_t buf[8] = {};
    mdns::Header h{};
    std::vector<mdns::Question> qs;
    EXPECT(!mdns::parsePacket(buf, sizeof(buf), h, qs));
}

// ── Response encoding ───────────────────────────────────────────────────────

static void test_encodeResponse_minimumPacketShape() {
    TEST("encodeResponse writes header + 3 answers for a TXT-less service");
    uint8_t buf[512];
    mdns::ResponseInputs in;
    in.instanceName = "host";
    in.hostName = "host";
    in.udpPort = 9876;
    const size_t n = mdns::encodeResponse(buf, sizeof(buf), /*txId=*/0xBEEF,
                                          /*unicast=*/false, in);
    EXPECT(n > 12u);
    // Header: id, flags (QR=1), QD=0, AN=3.
    EXPECT_EQ(buf[0], 0xBE);
    EXPECT_EQ(buf[1], 0xEF);
    EXPECT_EQ((buf[2] & 0x80) >> 7, 1);     // QR
    EXPECT_EQ(buf[5], 0u);                  // QDCOUNT low
    EXPECT_EQ(buf[7], 3u);                  // ANCOUNT low (no A record)
}

static void test_encodeResponse_includesARecordWhenIpv4Provided() {
    TEST("encodeResponse emits 4 answers when ipv4 supplied");
    uint8_t buf[512];
    const uint8_t ip[4] = {192, 168, 1, 2};
    mdns::ResponseInputs in;
    in.instanceName = "host";
    in.hostName = "host";
    in.udpPort = 9876;
    in.ipv4 = ip;
    const size_t n = mdns::encodeResponse(buf, sizeof(buf), 1, false, in);
    EXPECT(n > 0u);
    EXPECT_EQ(buf[7], 4u); // ANCOUNT low
}

static void test_encodeResponse_writesTxtPairs() {
    TEST("encodeResponse TXT rdata encodes each key=value as length-prefixed");
    uint8_t buf[512];
    mdns::ResponseInputs in;
    in.instanceName = "host";
    in.hostName = "host";
    in.udpPort = 9876;
    in.txtPairs = {{"udp", "9876"}, {"pair", "9878"}, {"http", "9877"}};
    const size_t n = mdns::encodeResponse(buf, sizeof(buf), 1, false, in);
    EXPECT(n > 0u);
    // Walk the buffer for "udp=9876" / "pair=9878" / "http=9877" — easiest
    // sanity check without re-implementing the parser here.
    auto bufStr = std::string(reinterpret_cast<const char*>(buf), n);
    EXPECT(bufStr.find("udp=9876") != std::string::npos);
    EXPECT(bufStr.find("pair=9878") != std::string::npos);
    EXPECT(bufStr.find("http=9877") != std::string::npos);
}

static void test_encodeResponse_emptyBufferReturnsZero() {
    TEST("encodeResponse returns 0 on undersized buffer");
    uint8_t buf[4];
    mdns::ResponseInputs in;
    in.instanceName = "host";
    in.hostName = "host";
    EXPECT_EQ(mdns::encodeResponse(buf, sizeof(buf), 1, false, in), 0u);
}

// ── Driver ──────────────────────────────────────────────────────────────────

// ── questionMatchesService ──────────────────────────────────────────────────

static mdns::Question makeQuestion(const std::string& name, uint16_t type) {
    mdns::Question q;
    q.name = name;
    q.type = type;
    q.cls = mdns::CLASS_IN;
    return q;
}

static void test_questionMatchesService_ptr() {
    TEST("questionMatchesService accepts a PTR query for the service domain");
    EXPECT(mdns::questionMatchesService(
        makeQuestion("_satellite._udp.local.", mdns::TYPE_PTR)));
}

static void test_questionMatchesService_any() {
    TEST("questionMatchesService accepts an ANY query for the service domain");
    EXPECT(mdns::questionMatchesService(
        makeQuestion("_satellite._udp.local.", mdns::TYPE_ANY)));
}

static void test_questionMatchesService_caseInsensitive() {
    TEST("questionMatchesService folds case (DNS names are case-insensitive)");
    EXPECT(mdns::questionMatchesService(
        makeQuestion("_Satellite._UDP.Local.", mdns::TYPE_PTR)));
}

static void test_questionMatchesService_rejectsWrongType() {
    TEST("questionMatchesService rejects a non-PTR/ANY record type");
    EXPECT(!mdns::questionMatchesService(
        makeQuestion("_satellite._udp.local.", mdns::TYPE_SRV)));
    EXPECT(!mdns::questionMatchesService(
        makeQuestion("_satellite._udp.local.", mdns::TYPE_A)));
    EXPECT(!mdns::questionMatchesService(
        makeQuestion("_satellite._udp.local.", mdns::TYPE_TXT)));
}

static void test_questionMatchesService_rejectsWrongName() {
    TEST("questionMatchesService rejects a different service domain");
    EXPECT(!mdns::questionMatchesService(
        makeQuestion("_airplay._tcp.local.", mdns::TYPE_PTR)));
    EXPECT(!mdns::questionMatchesService(
        makeQuestion("_satellite._tcp.local.", mdns::TYPE_PTR)));
}

static void test_questionMatchesService_rejectsPrefixAndSuffix() {
    TEST("questionMatchesService requires an exact name match (no prefix/suffix)");
    // Missing trailing dot.
    EXPECT(!mdns::questionMatchesService(
        makeQuestion("_satellite._udp.local", mdns::TYPE_PTR)));
    // An instance name *under* the service type is not the service type.
    EXPECT(!mdns::questionMatchesService(
        makeQuestion("box._satellite._udp.local.", mdns::TYPE_PTR)));
    // Trailing junk.
    EXPECT(!mdns::questionMatchesService(
        makeQuestion("_satellite._udp.local.extra.", mdns::TYPE_PTR)));
    // Empty.
    EXPECT(!mdns::questionMatchesService(makeQuestion("", mdns::TYPE_PTR)));
}

int main() {
    std::cout << "Running mDNS protocol tests...\n\n";

    test_writeDnsName_basic();
    test_writeDnsName_emptyTerminatorOnly();
    test_writeDnsName_rejectsOversizedLabel();
    test_writeDnsName_rejectsUndersizedBuffer();

    test_readDnsName_roundTrip();
    test_readDnsName_followsCompressionPointer();
    test_readDnsName_rejectsForwardJump();

    test_parsePacket_singleQuestion();
    test_parsePacket_rejectsTruncated();

    test_encodeResponse_minimumPacketShape();
    test_encodeResponse_includesARecordWhenIpv4Provided();
    test_encodeResponse_writesTxtPairs();
    test_encodeResponse_emptyBufferReturnsZero();

    test_questionMatchesService_ptr();
    test_questionMatchesService_any();
    test_questionMatchesService_caseInsensitive();
    test_questionMatchesService_rejectsWrongType();
    test_questionMatchesService_rejectsWrongName();
    test_questionMatchesService_rejectsPrefixAndSuffix();

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
