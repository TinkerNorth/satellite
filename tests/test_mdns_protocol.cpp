// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * tests/test_mdns_protocol.cpp — unit tests for the pure mDNS encoders /
 * parsers in src/net/mdns_protocol.cpp.
 *
 * Coverage: DNS name encode/decode (incl. compression pointers + malformed
 * input), query packet parsing, response encoding (record shapes, cache-flush
 * bits, SRV port, TXT pairs, A record, RFC 6762 §10.1 goodbye / TTL-0), and
 * the questionMatchesService predicate.
 */
#include "../src/net/mdns_protocol.h"

#include <cstdint>
#include <cstring>
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

// ── Test helpers ────────────────────────────────────────────────────────────

static uint16_t readBE16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

static uint32_t readBE32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

// One decoded resource record from an encodeResponse() packet. `cls` keeps the
// cache-flush bit as-is so tests can assert on it directly.
struct Rr {
    std::string name;
    uint16_t type = 0;
    uint16_t cls = 0;
    uint32_t ttl = 0;
    size_t rdataOffset = 0;
    uint16_t rdlen = 0;
};

// Walk an encodeResponse() packet into its header + answer records. Skips the
// (always empty in our responses) question section. Returns false on a
// malformed / truncated packet. Uses the production readDnsName so the helper
// itself exercises the decoder.
static bool parseResponseRecords(const uint8_t* buf, size_t len, mdns::Header& hdr,
                                 std::vector<Rr>& out) {
    out.clear();
    if (len < 12) return false;
    hdr.id = readBE16(buf + 0);
    hdr.flags = readBE16(buf + 2);
    hdr.qdCount = readBE16(buf + 4);
    hdr.anCount = readBE16(buf + 6);
    hdr.nsCount = readBE16(buf + 8);
    hdr.arCount = readBE16(buf + 10);

    size_t pos = 12;
    for (uint16_t i = 0; i < hdr.qdCount; ++i) {
        std::string n;
        const size_t c = mdns::readDnsName(buf, len, pos, n);
        if (c == 0) return false;
        pos += c + 4; // type + class
        if (pos > len) return false;
    }
    for (uint16_t i = 0; i < hdr.anCount; ++i) {
        Rr rr;
        const size_t c = mdns::readDnsName(buf, len, pos, rr.name);
        if (c == 0) return false;
        pos += c;
        if (pos + 10 > len) return false;
        rr.type = readBE16(buf + pos);
        rr.cls = readBE16(buf + pos + 2);
        rr.ttl = readBE32(buf + pos + 4);
        rr.rdlen = readBE16(buf + pos + 8);
        rr.rdataOffset = pos + 10;
        pos += 10;
        if (pos + rr.rdlen > len) return false;
        pos += rr.rdlen;
        out.push_back(rr);
    }
    return true;
}

// Find the first record of `type`, or nullptr.
static const Rr* findRr(const std::vector<Rr>& rrs, uint16_t type) {
    for (const auto& rr : rrs) {
        if (rr.type == type) return &rr;
    }
    return nullptr;
}

// Build the canonical set of response inputs used across the encode tests.
static mdns::ResponseInputs sampleInputs() {
    mdns::ResponseInputs in;
    in.instanceName = "satellite-host";
    in.hostName = "satellite-host";
    in.udpPort = 9876;
    in.txtPairs = {{"udp", "9876"}, {"pair", "9878"}, {"http", "9877"}};
    return in;
}

// ── DNS name encoding ───────────────────────────────────────────────────────

static void test_writeDnsName_basic() {
    TEST("writeDnsName encodes 'foo.bar.local.' as labelled-length form");
    uint8_t buf[64] = {};
    const size_t n = mdns::writeDnsName(buf, sizeof(buf), "foo.bar.local.");
    // 1B len + 3B "foo" + 1B len + 3B "bar" + 1B len + 5B "local" + 1B 0 = 15
    EXPECT_EQ(n, 15u);
    EXPECT_EQ(static_cast<int>(buf[0]), 3);
    EXPECT_EQ(static_cast<int>(buf[4]), 3);
    EXPECT_EQ(static_cast<int>(buf[8]), 5);
    EXPECT_EQ(static_cast<int>(buf[14]), 0); // terminator
    EXPECT(std::memcmp(buf + 1, "foo", 3) == 0);
    EXPECT(std::memcmp(buf + 5, "bar", 3) == 0);
    EXPECT(std::memcmp(buf + 9, "local", 5) == 0);
}

static void test_writeDnsName_singleLabel() {
    TEST("writeDnsName encodes a single label 'local.'");
    uint8_t buf[16] = {};
    const size_t n = mdns::writeDnsName(buf, sizeof(buf), "local.");
    EXPECT_EQ(n, 7u); // 1 + 5 + 1
    EXPECT_EQ(static_cast<int>(buf[0]), 5);
    EXPECT_EQ(static_cast<int>(buf[6]), 0);
}

static void test_writeDnsName_emptyTerminatorOnly() {
    TEST("writeDnsName writes single terminator for empty name");
    uint8_t buf[8] = {0xFF};
    const size_t n = mdns::writeDnsName(buf, sizeof(buf), "");
    EXPECT_EQ(n, 1u);
    EXPECT_EQ(static_cast<int>(buf[0]), 0);
}

static void test_writeDnsName_rejectsOversizedLabel() {
    TEST("writeDnsName rejects labels longer than 63 bytes");
    uint8_t buf[128];
    std::string longLabel(64, 'a');
    EXPECT_EQ(mdns::writeDnsName(buf, sizeof(buf), longLabel + ".local."), 0u);
}

static void test_writeDnsName_acceptsMaxLabel() {
    TEST("writeDnsName accepts a label of exactly 63 bytes");
    uint8_t buf[128];
    std::string maxLabel(63, 'a');
    EXPECT_EQ(mdns::writeDnsName(buf, sizeof(buf), maxLabel + "."), 65u); // 1 + 63 + 1
}

static void test_writeDnsName_rejectsUndersizedBuffer() {
    TEST("writeDnsName rejects buffers too small for the encoded output");
    uint8_t buf[4];
    EXPECT_EQ(mdns::writeDnsName(buf, sizeof(buf), "foo.bar.local."), 0u);
}

static void test_writeDnsName_rejectsBufferTooSmallForTerminator() {
    TEST("writeDnsName rejects a buffer with no room for the terminator");
    uint8_t buf[3]; // exactly fits "ab" label header+data, no room for the 0
    EXPECT_EQ(mdns::writeDnsName(buf, sizeof(buf), "ab."), 0u);
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

static void test_readDnsName_appendsTrailingDot() {
    TEST("readDnsName always yields a trailing dot even when input had none");
    uint8_t buf[32];
    const size_t written = mdns::writeDnsName(buf, sizeof(buf), "a.b");
    EXPECT(written > 0);
    std::string out;
    const size_t consumed = mdns::readDnsName(buf, written, 0, out);
    EXPECT_EQ(consumed, written);
    EXPECT_EQ(out, std::string("a.b."));
}

static void test_readDnsName_atNonZeroOffset() {
    TEST("readDnsName decodes a name that starts partway into the buffer");
    uint8_t buf[64] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
    const size_t written = mdns::writeDnsName(buf + 5, sizeof(buf) - 5, "host.local.");
    EXPECT(written > 0);
    std::string out;
    const size_t consumed = mdns::readDnsName(buf, 5 + written, 5, out);
    EXPECT_EQ(consumed, written);
    EXPECT_EQ(out, std::string("host.local."));
}

static void test_readDnsName_followsCompressionPointer() {
    TEST("readDnsName follows a backwards compression pointer");
    // Layout:
    //   0: 0x03 'f' 'o' 'o' 0x03 'b' 'a' 'r' 0x00      (9 bytes, "foo.bar.")
    //   9: 0x03 'b' 'a' 'z' 0xC0 0x04                  (6 bytes, "baz." + ptr to byte 4)
    uint8_t packet[] = {
        3, 'f', 'o', 'o', 3, 'b', 'a', 'r', 0, 3, 'b', 'a', 'z', 0xC0, 0x04,
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

static void test_readDnsName_rejectsTruncatedLabel() {
    TEST("readDnsName rejects a label that runs past the packet end");
    uint8_t packet[] = {0x0A, 'a', 'b', 'c'}; // claims 10 bytes, only 3 present
    std::string out;
    EXPECT_EQ(mdns::readDnsName(packet, sizeof(packet), 0, out), 0u);
}

static void test_readDnsName_rejectsPointerPastEnd() {
    TEST("readDnsName rejects a compression pointer targeting past the packet");
    // 5: 0xC0 0x40 → pointer to offset 64, well past this 7-byte packet.
    uint8_t packet[] = {3, 'f', 'o', 'o', 0, 0xC0, 0x40};
    std::string out;
    EXPECT_EQ(mdns::readDnsName(packet, sizeof(packet), 5, out), 0u);
}

// ── Packet parsing ──────────────────────────────────────────────────────────

// Append a DNS question (name + qtype + qclass) to `buf` at `pos`, return new pos.
static size_t appendQuestion(uint8_t* buf, size_t cap, size_t pos, const std::string& name,
                             uint16_t qtype, uint16_t qclass) {
    pos += mdns::writeDnsName(buf + pos, cap - pos, name);
    buf[pos++] = static_cast<uint8_t>(qtype >> 8);
    buf[pos++] = static_cast<uint8_t>(qtype & 0xFF);
    buf[pos++] = static_cast<uint8_t>(qclass >> 8);
    buf[pos++] = static_cast<uint8_t>(qclass & 0xFF);
    return pos;
}

static void writeQueryHeader(uint8_t* buf, uint16_t id, uint16_t qd, uint16_t an) {
    buf[0] = static_cast<uint8_t>(id >> 8);
    buf[1] = static_cast<uint8_t>(id & 0xFF);
    buf[2] = 0x00;
    buf[3] = 0x00; // flags: query
    buf[4] = static_cast<uint8_t>(qd >> 8);
    buf[5] = static_cast<uint8_t>(qd & 0xFF);
    buf[6] = static_cast<uint8_t>(an >> 8);
    buf[7] = static_cast<uint8_t>(an & 0xFF);
    buf[8] = buf[9] = buf[10] = buf[11] = 0x00; // NS / AR counts
}

// Append a DNS answer record (name + type + class + ttl + rdlen + rdata) to
// `buf` at `pos`; returns the new pos. Used to build the Known-Answer list
// (RFC 6762 §7.1) carried in a query's answer section. `rdata` may be null
// for a zero-length record — the parser only keys on name/type/ttl, so the
// exact rdata bytes are immaterial to the suppression tests.
static size_t appendAnswer(uint8_t* buf, size_t cap, size_t pos, const std::string& name,
                           uint16_t rtype, uint16_t rclass, uint32_t ttl, const uint8_t* rdata,
                           uint16_t rdlen) {
    pos += mdns::writeDnsName(buf + pos, cap - pos, name);
    buf[pos++] = static_cast<uint8_t>(rtype >> 8);
    buf[pos++] = static_cast<uint8_t>(rtype & 0xFF);
    buf[pos++] = static_cast<uint8_t>(rclass >> 8);
    buf[pos++] = static_cast<uint8_t>(rclass & 0xFF);
    buf[pos++] = static_cast<uint8_t>((ttl >> 24) & 0xFF);
    buf[pos++] = static_cast<uint8_t>((ttl >> 16) & 0xFF);
    buf[pos++] = static_cast<uint8_t>((ttl >> 8) & 0xFF);
    buf[pos++] = static_cast<uint8_t>(ttl & 0xFF);
    buf[pos++] = static_cast<uint8_t>(rdlen >> 8);
    buf[pos++] = static_cast<uint8_t>(rdlen & 0xFF);
    for (uint16_t i = 0; i < rdlen; ++i) buf[pos++] = rdata != nullptr ? rdata[i] : 0;
    return pos;
}

static void test_parsePacket_singleQuestion() {
    TEST("parsePacket decodes a single PTR question for the service domain (QU set)");
    uint8_t buf[128];
    writeQueryHeader(buf, 0x1234, 1, 0);
    const size_t end =
        appendQuestion(buf, sizeof(buf), 12, "_satellite._udp.local.", mdns::TYPE_PTR, 0x8001);
    mdns::Header h{};
    std::vector<mdns::Question> qs;
    EXPECT(mdns::parsePacket(buf, end, h, qs));
    EXPECT_EQ(h.id, 0x1234);
    EXPECT_EQ(qs.size(), 1u);
    EXPECT_EQ(qs[0].name, std::string("_satellite._udp.local."));
    EXPECT_EQ(qs[0].type, mdns::TYPE_PTR);
    EXPECT_EQ(qs[0].cls, mdns::CLASS_IN);
    EXPECT(qs[0].unicastResponse);
}

static void test_parsePacket_nonQuClass() {
    TEST("parsePacket clears unicastResponse when the QU bit is not set");
    uint8_t buf[128];
    writeQueryHeader(buf, 1, 1, 0);
    const size_t end = appendQuestion(buf, sizeof(buf), 12, "_satellite._udp.local.",
                                      mdns::TYPE_PTR, mdns::CLASS_IN);
    mdns::Header h{};
    std::vector<mdns::Question> qs;
    EXPECT(mdns::parsePacket(buf, end, h, qs));
    EXPECT_EQ(qs.size(), 1u);
    EXPECT(!qs[0].unicastResponse);
    EXPECT_EQ(qs[0].cls, mdns::CLASS_IN);
}

static void test_parsePacket_multipleQuestions() {
    TEST("parsePacket decodes every question when QDCOUNT > 1");
    uint8_t buf[256];
    writeQueryHeader(buf, 7, 2, 0);
    size_t pos = appendQuestion(buf, sizeof(buf), 12, "_satellite._udp.local.", mdns::TYPE_PTR,
                                mdns::CLASS_IN);
    pos = appendQuestion(buf, sizeof(buf), pos, "_airplay._tcp.local.", mdns::TYPE_PTR,
                         mdns::CLASS_IN);
    mdns::Header h{};
    std::vector<mdns::Question> qs;
    EXPECT(mdns::parsePacket(buf, pos, h, qs));
    EXPECT_EQ(qs.size(), 2u);
    EXPECT_EQ(qs[0].name, std::string("_satellite._udp.local."));
    EXPECT_EQ(qs[1].name, std::string("_airplay._tcp.local."));
}

static void test_parsePacket_emptyQuestionSection() {
    TEST("parsePacket accepts a header-only packet with QDCOUNT 0");
    uint8_t buf[12];
    writeQueryHeader(buf, 0x55, 0, 0);
    mdns::Header h{};
    std::vector<mdns::Question> qs;
    EXPECT(mdns::parsePacket(buf, 12, h, qs));
    EXPECT_EQ(qs.size(), 0u);
}

static void test_parsePacket_toleratesStaleAnswerCount() {
    TEST("parsePacket reads QDCOUNT questions and ignores a non-zero ANCOUNT");
    // Known-answer suppression packets carry answers we deliberately skip.
    uint8_t buf[128];
    writeQueryHeader(buf, 1, 1, /*an=*/3);
    const size_t end = appendQuestion(buf, sizeof(buf), 12, "_satellite._udp.local.",
                                      mdns::TYPE_PTR, mdns::CLASS_IN);
    mdns::Header h{};
    std::vector<mdns::Question> qs;
    EXPECT(mdns::parsePacket(buf, end, h, qs));
    EXPECT_EQ(qs.size(), 1u);
    EXPECT_EQ(h.anCount, 3u);
}

static void test_parsePacket_rejectsTruncatedHeader() {
    TEST("parsePacket rejects packets shorter than the 12-byte header");
    uint8_t buf[8] = {};
    mdns::Header h{};
    std::vector<mdns::Question> qs;
    EXPECT(!mdns::parsePacket(buf, sizeof(buf), h, qs));
}

static void test_parsePacket_rejectsTruncatedQuestion() {
    TEST("parsePacket rejects a QDCOUNT that exceeds the bytes present");
    uint8_t buf[20];
    writeQueryHeader(buf, 1, /*qd=*/2, 0);
    // Only one (short) malformed question's worth of bytes follow.
    buf[12] = 0x09; // label length 9 with nothing after it
    mdns::Header h{};
    std::vector<mdns::Question> qs;
    EXPECT(!mdns::parsePacket(buf, 13, h, qs));
}

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
    EXPECT(mdns::questionMatchesService(makeQuestion("_satellite._udp.local.", mdns::TYPE_PTR)));
}

static void test_questionMatchesService_any() {
    TEST("questionMatchesService accepts an ANY query for the service domain");
    EXPECT(mdns::questionMatchesService(makeQuestion("_satellite._udp.local.", mdns::TYPE_ANY)));
}

static void test_questionMatchesService_caseInsensitive() {
    TEST("questionMatchesService folds case (DNS names are case-insensitive)");
    EXPECT(mdns::questionMatchesService(makeQuestion("_Satellite._UDP.Local.", mdns::TYPE_PTR)));
}

static void test_questionMatchesService_rejectsWrongType() {
    TEST("questionMatchesService rejects a non-PTR/ANY record type");
    EXPECT(!mdns::questionMatchesService(makeQuestion("_satellite._udp.local.", mdns::TYPE_SRV)));
    EXPECT(!mdns::questionMatchesService(makeQuestion("_satellite._udp.local.", mdns::TYPE_A)));
    EXPECT(!mdns::questionMatchesService(makeQuestion("_satellite._udp.local.", mdns::TYPE_TXT)));
}

static void test_questionMatchesService_rejectsWrongName() {
    TEST("questionMatchesService rejects a different service domain");
    EXPECT(!mdns::questionMatchesService(makeQuestion("_airplay._tcp.local.", mdns::TYPE_PTR)));
    EXPECT(!mdns::questionMatchesService(makeQuestion("_satellite._tcp.local.", mdns::TYPE_PTR)));
}

static void test_questionMatchesService_rejectsPrefixAndSuffix() {
    TEST("questionMatchesService requires an exact name match (no prefix/suffix)");
    EXPECT(!mdns::questionMatchesService(makeQuestion("_satellite._udp.local", mdns::TYPE_PTR)));
    EXPECT(
        !mdns::questionMatchesService(makeQuestion("box._satellite._udp.local.", mdns::TYPE_PTR)));
    EXPECT(!mdns::questionMatchesService(
        makeQuestion("_satellite._udp.local.extra.", mdns::TYPE_PTR)));
    EXPECT(!mdns::questionMatchesService(makeQuestion("", mdns::TYPE_PTR)));
}

// ── Response encoding ───────────────────────────────────────────────────────

static void test_encodeResponse_minimumPacketShape() {
    TEST("encodeResponse writes header + 3 answers (PTR/SRV/TXT) without an A record");
    uint8_t buf[512];
    mdns::ResponseInputs in = sampleInputs();
    const size_t n = mdns::encodeResponse(buf, sizeof(buf), /*txId=*/0xBEEF, in);
    EXPECT(n > 12u);
    mdns::Header h{};
    std::vector<Rr> rrs;
    EXPECT(parseResponseRecords(buf, n, h, rrs));
    EXPECT_EQ(h.id, 0xBEEFu);
    EXPECT_EQ((h.flags >> 15) & 1, 1); // QR
    EXPECT_EQ((h.flags >> 10) & 1, 1); // AA
    EXPECT_EQ(h.qdCount, 0u);
    EXPECT_EQ(h.anCount, 3u);
    EXPECT_EQ(rrs.size(), 3u);
    EXPECT(findRr(rrs, mdns::TYPE_PTR) != nullptr);
    EXPECT(findRr(rrs, mdns::TYPE_SRV) != nullptr);
    EXPECT(findRr(rrs, mdns::TYPE_TXT) != nullptr);
    EXPECT(findRr(rrs, mdns::TYPE_A) == nullptr);
}

static void test_encodeResponse_includesARecordWhenIpv4Provided() {
    TEST("encodeResponse emits 4 answers and a valid A record when ipv4 supplied");
    uint8_t buf[512];
    const uint8_t ip[4] = {192, 168, 1, 2};
    mdns::ResponseInputs in = sampleInputs();
    in.ipv4 = ip;
    const size_t n = mdns::encodeResponse(buf, sizeof(buf), 1, in);
    mdns::Header h{};
    std::vector<Rr> rrs;
    EXPECT(parseResponseRecords(buf, n, h, rrs));
    EXPECT_EQ(h.anCount, 4u);
    const Rr* a = findRr(rrs, mdns::TYPE_A);
    EXPECT(a != nullptr);
    if (a != nullptr) {
        EXPECT_EQ(a->rdlen, 4u);
        EXPECT(std::memcmp(buf + a->rdataOffset, ip, 4) == 0);
        EXPECT_EQ(a->name, std::string("satellite-host.local."));
    }
}

static void test_encodeResponse_ptrTargetsInstanceFqdn() {
    TEST("encodeResponse PTR record points the service type at the instance FQDN");
    uint8_t buf[512];
    mdns::ResponseInputs in = sampleInputs();
    const size_t n = mdns::encodeResponse(buf, sizeof(buf), 1, in);
    mdns::Header h{};
    std::vector<Rr> rrs;
    EXPECT(parseResponseRecords(buf, n, h, rrs));
    const Rr* ptr = findRr(rrs, mdns::TYPE_PTR);
    EXPECT(ptr != nullptr);
    if (ptr != nullptr) {
        EXPECT_EQ(ptr->name, std::string("_satellite._udp.local."));
        std::string target;
        EXPECT(mdns::readDnsName(buf, n, ptr->rdataOffset, target) > 0);
        EXPECT_EQ(target, std::string("satellite-host._satellite._udp.local."));
    }
}

static void test_encodeResponse_srvCarriesPortAndTarget() {
    TEST("encodeResponse SRV record carries the UDP port and host target");
    uint8_t buf[512];
    mdns::ResponseInputs in = sampleInputs();
    in.udpPort = 40001;
    const size_t n = mdns::encodeResponse(buf, sizeof(buf), 1, in);
    mdns::Header h{};
    std::vector<Rr> rrs;
    EXPECT(parseResponseRecords(buf, n, h, rrs));
    const Rr* srv = findRr(rrs, mdns::TYPE_SRV);
    EXPECT(srv != nullptr);
    if (srv != nullptr) {
        // rdata: priority(2) weight(2) port(2) target(name)
        EXPECT_EQ(readBE16(buf + srv->rdataOffset + 0), 0u); // priority
        EXPECT_EQ(readBE16(buf + srv->rdataOffset + 2), 0u); // weight
        EXPECT_EQ(readBE16(buf + srv->rdataOffset + 4), 40001u);
        std::string target;
        EXPECT(mdns::readDnsName(buf, n, srv->rdataOffset + 6, target) > 0);
        EXPECT_EQ(target, std::string("satellite-host.local."));
    }
}

static void test_encodeResponse_cacheFlushBits() {
    TEST("encodeResponse sets the cache-flush bit on unique RRs but not the shared PTR");
    uint8_t buf[512];
    const uint8_t ip[4] = {10, 0, 0, 9};
    mdns::ResponseInputs in = sampleInputs();
    in.ipv4 = ip;
    const size_t n = mdns::encodeResponse(buf, sizeof(buf), 1, in);
    mdns::Header h{};
    std::vector<Rr> rrs;
    EXPECT(parseResponseRecords(buf, n, h, rrs));
    const Rr* ptr = findRr(rrs, mdns::TYPE_PTR);
    const Rr* srv = findRr(rrs, mdns::TYPE_SRV);
    const Rr* txt = findRr(rrs, mdns::TYPE_TXT);
    const Rr* a = findRr(rrs, mdns::TYPE_A);
    EXPECT(ptr != nullptr && srv != nullptr && txt != nullptr && a != nullptr);
    if (ptr != nullptr) EXPECT_EQ(ptr->cls & mdns::CACHE_FLUSH_BIT, 0); // shared → no flush
    if (srv != nullptr) EXPECT(srv->cls & mdns::CACHE_FLUSH_BIT);       // unique → flush
    if (txt != nullptr) EXPECT(txt->cls & mdns::CACHE_FLUSH_BIT);
    if (a != nullptr) EXPECT(a->cls & mdns::CACHE_FLUSH_BIT);
}

static void test_encodeResponse_writesTxtPairs() {
    TEST("encodeResponse TXT rdata encodes each key=value as a length-prefixed string");
    uint8_t buf[512];
    mdns::ResponseInputs in = sampleInputs();
    const size_t n = mdns::encodeResponse(buf, sizeof(buf), 1, in);
    mdns::Header h{};
    std::vector<Rr> rrs;
    EXPECT(parseResponseRecords(buf, n, h, rrs));
    const Rr* txt = findRr(rrs, mdns::TYPE_TXT);
    EXPECT(txt != nullptr);
    if (txt != nullptr) {
        // Walk the length-prefixed strings inside the TXT rdata.
        std::vector<std::string> entries;
        size_t t = txt->rdataOffset;
        const size_t end = txt->rdataOffset + txt->rdlen;
        while (t < end) {
            const uint8_t slen = buf[t];
            if (t + 1 + slen > end) break;
            entries.emplace_back(reinterpret_cast<const char*>(buf + t + 1), slen);
            t += 1 + slen;
        }
        EXPECT_EQ(entries.size(), 3u);
        EXPECT(!entries.empty() && entries[0] == "udp=9876");
        EXPECT(entries.size() > 1 && entries[1] == "pair=9878");
        EXPECT(entries.size() > 2 && entries[2] == "http=9877");
    }
}

static void test_encodeResponse_emptyTxtIsZeroLengthString() {
    TEST("encodeResponse with no TXT pairs emits a single zero-length string (DNS-SD §6.1)");
    uint8_t buf[512];
    mdns::ResponseInputs in = sampleInputs();
    in.txtPairs.clear();
    const size_t n = mdns::encodeResponse(buf, sizeof(buf), 1, in);
    mdns::Header h{};
    std::vector<Rr> rrs;
    EXPECT(parseResponseRecords(buf, n, h, rrs));
    const Rr* txt = findRr(rrs, mdns::TYPE_TXT);
    EXPECT(txt != nullptr);
    if (txt != nullptr) {
        EXPECT_EQ(txt->rdlen, 1u);
        EXPECT_EQ(static_cast<int>(buf[txt->rdataOffset]), 0);
    }
}

static void test_encodeResponse_normalTtls() {
    TEST("encodeResponse uses the service / host TTLs for a non-goodbye response");
    uint8_t buf[512];
    const uint8_t ip[4] = {10, 0, 0, 1};
    mdns::ResponseInputs in = sampleInputs();
    in.ipv4 = ip;
    const size_t n = mdns::encodeResponse(buf, sizeof(buf), 1, in);
    mdns::Header h{};
    std::vector<Rr> rrs;
    EXPECT(parseResponseRecords(buf, n, h, rrs));
    const Rr* ptr = findRr(rrs, mdns::TYPE_PTR);
    const Rr* srv = findRr(rrs, mdns::TYPE_SRV);
    EXPECT(ptr != nullptr && srv != nullptr);
    if (ptr != nullptr) EXPECT_EQ(ptr->ttl, mdns::TTL_SERVICE);
    if (srv != nullptr) EXPECT_EQ(srv->ttl, mdns::TTL_HOST);
}

static void test_encodeResponse_goodbyeUsesTtlZero() {
    TEST("encodeResponse goodbye announcement sets TTL 0 on every record");
    uint8_t buf[512];
    const uint8_t ip[4] = {10, 0, 0, 1};
    mdns::ResponseInputs in = sampleInputs();
    in.ipv4 = ip;
    in.goodbye = true;
    const size_t n = mdns::encodeResponse(buf, sizeof(buf), 0, in);
    mdns::Header h{};
    std::vector<Rr> rrs;
    EXPECT(parseResponseRecords(buf, n, h, rrs));
    EXPECT_EQ(rrs.size(), 4u); // goodbye still emits the full record set
    bool allZero = !rrs.empty();
    for (const auto& rr : rrs) {
        if (rr.ttl != 0) allZero = false;
    }
    EXPECT(allZero);
}

static void test_encodeResponse_rejectsUndersizedBuffer() {
    TEST("encodeResponse returns 0 on an undersized buffer");
    uint8_t buf[4];
    mdns::ResponseInputs in = sampleInputs();
    EXPECT_EQ(mdns::encodeResponse(buf, sizeof(buf), 1, in), 0u);
}

static void test_encodeResponse_rejectsBufferBetweenHeaderAndBody() {
    TEST("encodeResponse returns 0 when the buffer holds the header but not the records");
    uint8_t buf[20]; // > 12 (header) but far too small for PTR+SRV+TXT
    mdns::ResponseInputs in = sampleInputs();
    EXPECT_EQ(mdns::encodeResponse(buf, sizeof(buf), 1, in), 0u);
}

// ── Startup announcement (RFC 6762 §8.3) ────────────────────────────────────

static void test_encodeAnnouncement_recordSetAndShape() {
    TEST("encodeAnnouncement emits PTR/SRV/TXT/A with txId 0 and QR+AA set");
    uint8_t buf[512];
    const uint8_t ip[4] = {192, 168, 4, 7};
    mdns::ResponseInputs in = sampleInputs();
    in.ipv4 = ip;
    const size_t n = mdns::encodeAnnouncement(buf, sizeof(buf), in);
    EXPECT(n > 12u);
    mdns::Header h{};
    std::vector<Rr> rrs;
    EXPECT(parseResponseRecords(buf, n, h, rrs));
    // §8.3: an announcement answers no query → transaction ID is 0.
    EXPECT_EQ(h.id, 0u);
    EXPECT_EQ((h.flags >> 15) & 1, 1); // QR
    EXPECT_EQ((h.flags >> 10) & 1, 1); // AA
    EXPECT_EQ(h.qdCount, 0u);
    EXPECT_EQ(h.anCount, 4u);
    EXPECT(findRr(rrs, mdns::TYPE_PTR) != nullptr);
    EXPECT(findRr(rrs, mdns::TYPE_SRV) != nullptr);
    EXPECT(findRr(rrs, mdns::TYPE_TXT) != nullptr);
    EXPECT(findRr(rrs, mdns::TYPE_A) != nullptr);
}

static void test_encodeAnnouncement_cacheFlushBits() {
    TEST("encodeAnnouncement sets cache-flush on unique RRs but not the shared PTR");
    uint8_t buf[512];
    const uint8_t ip[4] = {10, 1, 2, 3};
    mdns::ResponseInputs in = sampleInputs();
    in.ipv4 = ip;
    const size_t n = mdns::encodeAnnouncement(buf, sizeof(buf), in);
    mdns::Header h{};
    std::vector<Rr> rrs;
    EXPECT(parseResponseRecords(buf, n, h, rrs));
    const Rr* ptr = findRr(rrs, mdns::TYPE_PTR);
    const Rr* srv = findRr(rrs, mdns::TYPE_SRV);
    const Rr* txt = findRr(rrs, mdns::TYPE_TXT);
    const Rr* a = findRr(rrs, mdns::TYPE_A);
    EXPECT(ptr != nullptr && srv != nullptr && txt != nullptr && a != nullptr);
    if (ptr != nullptr) EXPECT_EQ(ptr->cls & mdns::CACHE_FLUSH_BIT, 0); // shared → no flush
    if (srv != nullptr) EXPECT(srv->cls & mdns::CACHE_FLUSH_BIT);       // unique → flush
    if (txt != nullptr) EXPECT(txt->cls & mdns::CACHE_FLUSH_BIT);
    if (a != nullptr) EXPECT(a->cls & mdns::CACHE_FLUSH_BIT);
}

static void test_encodeAnnouncement_recordContentMatchesResponse() {
    TEST("encodeAnnouncement carries the same PTR target / SRV port / TXT pairs as a query reply");
    uint8_t buf[512];
    const uint8_t ip[4] = {172, 16, 0, 5};
    mdns::ResponseInputs in = sampleInputs();
    in.udpPort = 41234;
    in.ipv4 = ip;
    const size_t n = mdns::encodeAnnouncement(buf, sizeof(buf), in);
    mdns::Header h{};
    std::vector<Rr> rrs;
    EXPECT(parseResponseRecords(buf, n, h, rrs));
    const Rr* ptr = findRr(rrs, mdns::TYPE_PTR);
    const Rr* srv = findRr(rrs, mdns::TYPE_SRV);
    const Rr* txt = findRr(rrs, mdns::TYPE_TXT);
    const Rr* a = findRr(rrs, mdns::TYPE_A);
    EXPECT(ptr != nullptr && srv != nullptr && txt != nullptr && a != nullptr);
    if (ptr != nullptr) {
        EXPECT_EQ(ptr->name, std::string("_satellite._udp.local."));
        std::string target;
        EXPECT(mdns::readDnsName(buf, n, ptr->rdataOffset, target) > 0);
        EXPECT_EQ(target, std::string("satellite-host._satellite._udp.local."));
        EXPECT_EQ(ptr->ttl, mdns::TTL_SERVICE);
    }
    if (srv != nullptr) {
        EXPECT_EQ(readBE16(buf + srv->rdataOffset + 4), 41234u); // port
        std::string target;
        EXPECT(mdns::readDnsName(buf, n, srv->rdataOffset + 6, target) > 0);
        EXPECT_EQ(target, std::string("satellite-host.local."));
        EXPECT_EQ(srv->ttl, mdns::TTL_HOST);
    }
    if (txt != nullptr) {
        std::vector<std::string> entries;
        size_t t = txt->rdataOffset;
        const size_t end = txt->rdataOffset + txt->rdlen;
        while (t < end) {
            const uint8_t slen = buf[t];
            if (t + 1 + slen > end) break;
            entries.emplace_back(reinterpret_cast<const char*>(buf + t + 1), slen);
            t += 1 + slen;
        }
        EXPECT_EQ(entries.size(), 3u);
        EXPECT(!entries.empty() && entries[0] == "udp=9876");
    }
    if (a != nullptr) {
        EXPECT_EQ(a->name, std::string("satellite-host.local."));
        EXPECT_EQ(a->rdlen, 4u);
        EXPECT(std::memcmp(buf + a->rdataOffset, ip, 4) == 0);
    }
}

static void test_encodeAnnouncement_withoutIpv4OmitsARecord() {
    TEST("encodeAnnouncement emits only PTR/SRV/TXT when no IPv4 is supplied");
    uint8_t buf[512];
    mdns::ResponseInputs in = sampleInputs(); // no ipv4
    const size_t n = mdns::encodeAnnouncement(buf, sizeof(buf), in);
    mdns::Header h{};
    std::vector<Rr> rrs;
    EXPECT(parseResponseRecords(buf, n, h, rrs));
    EXPECT_EQ(h.anCount, 3u);
    EXPECT(findRr(rrs, mdns::TYPE_A) == nullptr);
}

static void test_encodeAnnouncement_normalTtlsNeverZero() {
    TEST("encodeAnnouncement uses live TTLs even if the caller set goodbye");
    uint8_t buf[512];
    const uint8_t ip[4] = {10, 0, 0, 8};
    mdns::ResponseInputs in = sampleInputs();
    in.ipv4 = ip;
    in.goodbye = true; // must be ignored — an announcement is not a retraction
    const size_t n = mdns::encodeAnnouncement(buf, sizeof(buf), in);
    mdns::Header h{};
    std::vector<Rr> rrs;
    EXPECT(parseResponseRecords(buf, n, h, rrs));
    EXPECT_EQ(rrs.size(), 4u);
    bool allNonZero = !rrs.empty();
    for (const auto& rr : rrs) {
        if (rr.ttl == 0) allNonZero = false;
    }
    EXPECT(allNonZero);
}

// ── Probe query (RFC 6762 §8.1) ─────────────────────────────────────────────

static void test_encodeProbeQuery_shape() {
    TEST("encodeProbeQuery emits one ANY question for the instance FQDN, QU bit clear");
    uint8_t buf[256];
    const size_t n = mdns::encodeProbeQuery(buf, sizeof(buf), "satellite-host");
    EXPECT(n > 12u);
    mdns::Header h{};
    std::vector<mdns::Question> qs;
    std::vector<mdns::Answer> ans;
    EXPECT(mdns::parsePacket(buf, n, h, qs, ans));
    EXPECT_EQ(h.id, 0u);                  // mDNS query → txn id 0
    EXPECT_EQ((h.flags >> 15) & 1, 0);    // QR clear → it is a query
    EXPECT_EQ(h.qdCount, 1u);
    EXPECT_EQ(h.anCount, 0u);
    EXPECT_EQ(qs.size(), 1u);
    if (!qs.empty()) {
        EXPECT_EQ(qs[0].name, std::string("satellite-host._satellite._udp.local."));
        EXPECT_EQ(qs[0].type, mdns::TYPE_ANY);
        EXPECT(!qs[0].unicastResponse); // probes are multicast
        EXPECT_EQ(qs[0].cls, mdns::CLASS_IN);
    }
}

static void test_encodeProbeQuery_rejectsUndersizedBuffer() {
    TEST("encodeProbeQuery returns 0 on a buffer too small for the question");
    uint8_t buf[14]; // header fits, the name does not
    EXPECT_EQ(mdns::encodeProbeQuery(buf, sizeof(buf), "satellite-host"), 0u);
}

// ── Known-Answer parsing (RFC 6762 §7.1) ────────────────────────────────────

static void test_parsePacket_surfacesKnownAnswer() {
    TEST("parsePacket(5-arg) surfaces a query's answer-section record");
    uint8_t buf[256];
    writeQueryHeader(buf, 0x2222, /*qd=*/1, /*an=*/1);
    size_t pos = appendQuestion(buf, sizeof(buf), 12, "_satellite._udp.local.", mdns::TYPE_PTR,
                                mdns::CLASS_IN);
    // Known answer: a PTR for the service type pointing at an instance.
    uint8_t rdata[64];
    const size_t rdlen =
        mdns::writeDnsName(rdata, sizeof(rdata), "other._satellite._udp.local.");
    pos = appendAnswer(buf, sizeof(buf), pos, "_satellite._udp.local.", mdns::TYPE_PTR,
                       mdns::CLASS_IN, /*ttl=*/4000, rdata, static_cast<uint16_t>(rdlen));
    mdns::Header h{};
    std::vector<mdns::Question> qs;
    std::vector<mdns::Answer> ans;
    EXPECT(mdns::parsePacket(buf, pos, h, qs, ans));
    EXPECT_EQ(qs.size(), 1u);
    EXPECT_EQ(ans.size(), 1u);
    if (!ans.empty()) {
        EXPECT_EQ(ans[0].name, std::string("_satellite._udp.local."));
        EXPECT_EQ(ans[0].type, mdns::TYPE_PTR);
        EXPECT_EQ(ans[0].ttl, 4000u);
        EXPECT_EQ(ans[0].cls, mdns::CLASS_IN);
    }
}

static void test_parsePacket_surfacesMultipleKnownAnswers() {
    TEST("parsePacket(5-arg) surfaces every answer when ANCOUNT > 1");
    uint8_t buf[512];
    writeQueryHeader(buf, 9, /*qd=*/1, /*an=*/2);
    size_t pos = appendQuestion(buf, sizeof(buf), 12, "_satellite._udp.local.", mdns::TYPE_ANY,
                                mdns::CLASS_IN);
    const uint8_t a4[4] = {192, 168, 1, 50};
    pos = appendAnswer(buf, sizeof(buf), pos, "satellite-host.local.", mdns::TYPE_A,
                       mdns::CLASS_IN, /*ttl=*/4500, a4, 4);
    uint8_t srvrd[32];
    size_t s = 0;
    srvrd[s++] = 0; srvrd[s++] = 0;             // priority
    srvrd[s++] = 0; srvrd[s++] = 0;             // weight
    srvrd[s++] = 0x26; srvrd[s++] = 0x94;       // port 9876
    s += mdns::writeDnsName(srvrd + s, sizeof(srvrd) - s, "satellite-host.local.");
    pos = appendAnswer(buf, sizeof(buf), pos, "satellite-host._satellite._udp.local.",
                       mdns::TYPE_SRV, mdns::CLASS_IN, /*ttl=*/4500, srvrd,
                       static_cast<uint16_t>(s));
    mdns::Header h{};
    std::vector<mdns::Question> qs;
    std::vector<mdns::Answer> ans;
    EXPECT(mdns::parsePacket(buf, pos, h, qs, ans));
    EXPECT_EQ(ans.size(), 2u);
    if (ans.size() == 2) {
        EXPECT_EQ(ans[0].type, mdns::TYPE_A);
        EXPECT_EQ(ans[1].type, mdns::TYPE_SRV);
        EXPECT_EQ(ans[1].name, std::string("satellite-host._satellite._udp.local."));
    }
}

static void test_parsePacket_cacheFlushMaskedOffKnownAnswer() {
    TEST("parsePacket(5-arg) masks the cache-flush bit out of a known answer's class");
    uint8_t buf[256];
    writeQueryHeader(buf, 1, /*qd=*/1, /*an=*/1);
    size_t pos = appendQuestion(buf, sizeof(buf), 12, "_satellite._udp.local.", mdns::TYPE_PTR,
                                mdns::CLASS_IN);
    const uint8_t a4[4] = {10, 0, 0, 1};
    // Class carries the cache-flush bit set — the parser must strip it.
    pos = appendAnswer(buf, sizeof(buf), pos, "satellite-host.local.", mdns::TYPE_A,
                       mdns::CLASS_IN | mdns::CACHE_FLUSH_BIT, 4500, a4, 4);
    mdns::Header h{};
    std::vector<mdns::Question> qs;
    std::vector<mdns::Answer> ans;
    EXPECT(mdns::parsePacket(buf, pos, h, qs, ans));
    EXPECT_EQ(ans.size(), 1u);
    if (!ans.empty()) EXPECT_EQ(ans[0].cls, mdns::CLASS_IN);
}

static void test_parsePacket_fourArgStillSkipsAnswers() {
    TEST("parsePacket(4-arg) still parses questions and ignores the answer section");
    uint8_t buf[256];
    writeQueryHeader(buf, 3, /*qd=*/1, /*an=*/1);
    size_t pos = appendQuestion(buf, sizeof(buf), 12, "_satellite._udp.local.", mdns::TYPE_PTR,
                                mdns::CLASS_IN);
    const uint8_t a4[4] = {10, 0, 0, 1};
    pos = appendAnswer(buf, sizeof(buf), pos, "satellite-host.local.", mdns::TYPE_A,
                       mdns::CLASS_IN, 4500, a4, 4);
    mdns::Header h{};
    std::vector<mdns::Question> qs;
    EXPECT(mdns::parsePacket(buf, pos, h, qs)); // 4-arg overload
    EXPECT_EQ(qs.size(), 1u);
    EXPECT_EQ(h.anCount, 1u);
}

static void test_parsePacket_rejectsTruncatedAnswerHeader() {
    TEST("parsePacket(5-arg) rejects an answer record truncated before its fixed fields");
    uint8_t buf[256];
    writeQueryHeader(buf, 1, /*qd=*/1, /*an=*/1);
    size_t pos = appendQuestion(buf, sizeof(buf), 12, "_satellite._udp.local.", mdns::TYPE_PTR,
                                mdns::CLASS_IN);
    // Write a valid answer name, then truncate before type/class/ttl/rdlen.
    pos += mdns::writeDnsName(buf + pos, sizeof(buf) - pos, "satellite-host.local.");
    buf[pos++] = 0x00; // a couple of stray bytes — far short of the 10-byte fixed block
    buf[pos++] = 0x01;
    mdns::Header h{};
    std::vector<mdns::Question> qs;
    std::vector<mdns::Answer> ans;
    EXPECT(!mdns::parsePacket(buf, pos, h, qs, ans));
}

static void test_parsePacket_rejectsAnswerRdlenOverrun() {
    TEST("parsePacket(5-arg) rejects an answer whose RDLENGTH overruns the packet");
    uint8_t buf[256];
    writeQueryHeader(buf, 1, /*qd=*/1, /*an=*/1);
    size_t pos = appendQuestion(buf, sizeof(buf), 12, "_satellite._udp.local.", mdns::TYPE_PTR,
                                mdns::CLASS_IN);
    pos += mdns::writeDnsName(buf + pos, sizeof(buf) - pos, "satellite-host.local.");
    buf[pos++] = 0x00; buf[pos++] = 0x01;       // type A
    buf[pos++] = 0x00; buf[pos++] = 0x01;       // class IN
    buf[pos++] = 0x00; buf[pos++] = 0x00;
    buf[pos++] = 0x11; buf[pos++] = 0x94;       // ttl 4500
    buf[pos++] = 0xFF; buf[pos++] = 0xFF;       // rdlen 65535 — wildly past the packet
    buf[pos++] = 0x0A; buf[pos++] = 0x00;       // only 2 rdata bytes actually present
    mdns::Header h{};
    std::vector<mdns::Question> qs;
    std::vector<mdns::Answer> ans;
    EXPECT(!mdns::parsePacket(buf, pos, h, qs, ans));
}

static void test_parsePacket_rejectsAnswerCountOverrun() {
    TEST("parsePacket(5-arg) rejects an ANCOUNT larger than the records present");
    uint8_t buf[256];
    writeQueryHeader(buf, 1, /*qd=*/1, /*an=*/3); // claims 3 answers
    size_t pos = appendQuestion(buf, sizeof(buf), 12, "_satellite._udp.local.", mdns::TYPE_PTR,
                                mdns::CLASS_IN);
    const uint8_t a4[4] = {10, 0, 0, 1};
    pos = appendAnswer(buf, sizeof(buf), pos, "satellite-host.local.", mdns::TYPE_A,
                       mdns::CLASS_IN, 4500, a4, 4); // only one supplied
    mdns::Header h{};
    std::vector<mdns::Question> qs;
    std::vector<mdns::Answer> ans;
    EXPECT(!mdns::parsePacket(buf, pos, h, qs, ans));
}

static void test_parsePacket_rejectsAnswerCompressionLoop() {
    TEST("parsePacket(5-arg) rejects an answer name with a self-referential compression pointer");
    uint8_t buf[256];
    writeQueryHeader(buf, 1, /*qd=*/1, /*an=*/1);
    size_t pos = appendQuestion(buf, sizeof(buf), 12, "_satellite._udp.local.", mdns::TYPE_PTR,
                                mdns::CLASS_IN);
    // Answer name is a pointer to itself — readDnsName must reject it.
    buf[pos] = 0xC0;
    buf[pos + 1] = static_cast<uint8_t>(pos & 0xFF);
    pos += 2;
    mdns::Header h{};
    std::vector<mdns::Question> qs;
    std::vector<mdns::Answer> ans;
    EXPECT(!mdns::parsePacket(buf, pos, h, qs, ans));
}

// ── isKnownAnswerSuppressed (RFC 6762 §7.1) ─────────────────────────────────

static mdns::Answer makeAnswer(const std::string& name, uint16_t type, uint32_t ttl) {
    mdns::Answer a;
    a.name = name;
    a.type = type;
    a.cls = mdns::CLASS_IN;
    a.ttl = ttl;
    return a;
}

static void test_isKnownAnswerSuppressed_freshAnswerSuppresses() {
    TEST("isKnownAnswerSuppressed suppresses when the querier's TTL is >= half ours");
    std::vector<mdns::Answer> known = {
        makeAnswer("_satellite._udp.local.", mdns::TYPE_PTR, mdns::TTL_SERVICE),
    };
    // Exactly half is the boundary and must still suppress.
    EXPECT(mdns::isKnownAnswerSuppressed(known, "_satellite._udp.local.", mdns::TYPE_PTR,
                                         mdns::TTL_SERVICE));
    std::vector<mdns::Answer> halfExactly = {
        makeAnswer("_satellite._udp.local.", mdns::TYPE_PTR, mdns::TTL_SERVICE / 2),
    };
    EXPECT(mdns::isKnownAnswerSuppressed(halfExactly, "_satellite._udp.local.", mdns::TYPE_PTR,
                                         mdns::TTL_SERVICE));
}

static void test_isKnownAnswerSuppressed_lowTtlDoesNotSuppress() {
    TEST("isKnownAnswerSuppressed does not suppress when the querier's TTL is below half ours");
    std::vector<mdns::Answer> known = {
        makeAnswer("_satellite._udp.local.", mdns::TYPE_PTR, (mdns::TTL_SERVICE / 2) - 1),
    };
    EXPECT(!mdns::isKnownAnswerSuppressed(known, "_satellite._udp.local.", mdns::TYPE_PTR,
                                          mdns::TTL_SERVICE));
}

static void test_isKnownAnswerSuppressed_typeAndNameMustMatch() {
    TEST("isKnownAnswerSuppressed requires both name and type to match");
    std::vector<mdns::Answer> known = {
        makeAnswer("_satellite._udp.local.", mdns::TYPE_PTR, mdns::TTL_SERVICE),
    };
    // Right name, wrong type.
    EXPECT(!mdns::isKnownAnswerSuppressed(known, "_satellite._udp.local.", mdns::TYPE_SRV,
                                          mdns::TTL_HOST));
    // Right type, wrong name.
    EXPECT(!mdns::isKnownAnswerSuppressed(known, "_other._udp.local.", mdns::TYPE_PTR,
                                          mdns::TTL_SERVICE));
    // Empty known-answer list never suppresses.
    EXPECT(!mdns::isKnownAnswerSuppressed({}, "_satellite._udp.local.", mdns::TYPE_PTR,
                                          mdns::TTL_SERVICE));
}

static void test_isKnownAnswerSuppressed_caseInsensitiveName() {
    TEST("isKnownAnswerSuppressed folds case on the record name");
    std::vector<mdns::Answer> known = {
        makeAnswer("_SATELLITE._UDP.LOCAL.", mdns::TYPE_PTR, mdns::TTL_SERVICE),
    };
    EXPECT(mdns::isKnownAnswerSuppressed(known, "_satellite._udp.local.", mdns::TYPE_PTR,
                                         mdns::TTL_SERVICE));
}

// ── Known-Answer suppression in encodeResponse (RFC 6762 §7.1) ──────────────

static void test_encodeResponse_suppressPtrOmitsPtr() {
    TEST("encodeResponse with suppressPtr drops the PTR and adjusts ANCOUNT");
    uint8_t buf[512];
    const uint8_t ip[4] = {10, 0, 0, 1};
    mdns::ResponseInputs in = sampleInputs();
    in.ipv4 = ip;
    in.suppressPtr = true;
    const size_t n = mdns::encodeResponse(buf, sizeof(buf), 1, in);
    EXPECT(n > 12u);
    mdns::Header h{};
    std::vector<Rr> rrs;
    EXPECT(parseResponseRecords(buf, n, h, rrs));
    EXPECT_EQ(h.anCount, 3u); // SRV + TXT + A
    EXPECT(findRr(rrs, mdns::TYPE_PTR) == nullptr);
    EXPECT(findRr(rrs, mdns::TYPE_SRV) != nullptr);
    EXPECT(findRr(rrs, mdns::TYPE_TXT) != nullptr);
    EXPECT(findRr(rrs, mdns::TYPE_A) != nullptr);
}

static void test_encodeResponse_suppressSrvAndTxt() {
    TEST("encodeResponse can suppress SRV and TXT independently");
    uint8_t buf[512];
    mdns::ResponseInputs in = sampleInputs(); // no A record
    in.suppressSrv = true;
    in.suppressTxt = true;
    const size_t n = mdns::encodeResponse(buf, sizeof(buf), 1, in);
    EXPECT(n > 12u);
    mdns::Header h{};
    std::vector<Rr> rrs;
    EXPECT(parseResponseRecords(buf, n, h, rrs));
    EXPECT_EQ(h.anCount, 1u); // only PTR survives
    EXPECT(findRr(rrs, mdns::TYPE_PTR) != nullptr);
    EXPECT(findRr(rrs, mdns::TYPE_SRV) == nullptr);
    EXPECT(findRr(rrs, mdns::TYPE_TXT) == nullptr);
}

static void test_encodeResponse_suppressAllReturnsZero() {
    TEST("encodeResponse returns 0 when every record is suppressed (send nothing)");
    uint8_t buf[512];
    const uint8_t ip[4] = {10, 0, 0, 1};
    mdns::ResponseInputs in = sampleInputs();
    in.ipv4 = ip;
    in.suppressPtr = true;
    in.suppressSrv = true;
    in.suppressTxt = true;
    in.suppressA = true;
    EXPECT_EQ(mdns::encodeResponse(buf, sizeof(buf), 1, in), 0u);
}

static void test_encodeResponse_suppressAOnlyDropsA() {
    TEST("encodeResponse with suppressA keeps PTR/SRV/TXT and drops only the A record");
    uint8_t buf[512];
    const uint8_t ip[4] = {10, 0, 0, 1};
    mdns::ResponseInputs in = sampleInputs();
    in.ipv4 = ip;
    in.suppressA = true;
    const size_t n = mdns::encodeResponse(buf, sizeof(buf), 1, in);
    mdns::Header h{};
    std::vector<Rr> rrs;
    EXPECT(parseResponseRecords(buf, n, h, rrs));
    EXPECT_EQ(h.anCount, 3u);
    EXPECT(findRr(rrs, mdns::TYPE_A) == nullptr);
}

// End-to-end: parse a query carrying a known answer, then verify the response
// omits exactly that record — and that a stale known answer is not suppressed.
static void test_knownAnswer_endToEnd_suppressesKnownPtr() {
    TEST("a query carrying a fresh known PTR yields a response with the PTR omitted");
    // Build the query: PTR question + a known PTR answer with a fresh TTL.
    uint8_t qbuf[256];
    writeQueryHeader(qbuf, 0x44, /*qd=*/1, /*an=*/1);
    size_t qpos = appendQuestion(qbuf, sizeof(qbuf), 12, "_satellite._udp.local.", mdns::TYPE_PTR,
                                 mdns::CLASS_IN);
    uint8_t rdata[64];
    const size_t rdlen =
        mdns::writeDnsName(rdata, sizeof(rdata), "satellite-host._satellite._udp.local.");
    qpos = appendAnswer(qbuf, sizeof(qbuf), qpos, "_satellite._udp.local.", mdns::TYPE_PTR,
                        mdns::CLASS_IN, mdns::TTL_SERVICE, rdata, static_cast<uint16_t>(rdlen));
    mdns::Header qh{};
    std::vector<mdns::Question> qqs;
    std::vector<mdns::Answer> qans;
    EXPECT(mdns::parsePacket(qbuf, qpos, qh, qqs, qans));
    EXPECT_EQ(qans.size(), 1u);

    // Responder logic: decide suppression from the known-answer list.
    mdns::ResponseInputs in = sampleInputs();
    in.suppressPtr = mdns::isKnownAnswerSuppressed(qans, "_satellite._udp.local.", mdns::TYPE_PTR,
                                                   mdns::TTL_SERVICE);
    EXPECT(in.suppressPtr);
    uint8_t rbuf[512];
    const size_t rn = mdns::encodeResponse(rbuf, sizeof(rbuf), qh.id, in);
    mdns::Header rh{};
    std::vector<Rr> rrs;
    EXPECT(parseResponseRecords(rbuf, rn, rh, rrs));
    EXPECT(findRr(rrs, mdns::TYPE_PTR) == nullptr); // suppressed
    EXPECT(findRr(rrs, mdns::TYPE_SRV) != nullptr); // still sent
}

static void test_knownAnswer_endToEnd_staleTtlStillSent() {
    TEST("a query carrying a known PTR with a too-low TTL still gets the PTR in the response");
    uint8_t qbuf[256];
    writeQueryHeader(qbuf, 0x45, /*qd=*/1, /*an=*/1);
    size_t qpos = appendQuestion(qbuf, sizeof(qbuf), 12, "_satellite._udp.local.", mdns::TYPE_PTR,
                                 mdns::CLASS_IN);
    uint8_t rdata[64];
    const size_t rdlen =
        mdns::writeDnsName(rdata, sizeof(rdata), "satellite-host._satellite._udp.local.");
    // TTL below half of TTL_SERVICE — the querier's copy is stale.
    qpos = appendAnswer(qbuf, sizeof(qbuf), qpos, "_satellite._udp.local.", mdns::TYPE_PTR,
                        mdns::CLASS_IN, (mdns::TTL_SERVICE / 2) - 1, rdata,
                        static_cast<uint16_t>(rdlen));
    mdns::Header qh{};
    std::vector<mdns::Question> qqs;
    std::vector<mdns::Answer> qans;
    EXPECT(mdns::parsePacket(qbuf, qpos, qh, qqs, qans));

    mdns::ResponseInputs in = sampleInputs();
    in.suppressPtr = mdns::isKnownAnswerSuppressed(qans, "_satellite._udp.local.", mdns::TYPE_PTR,
                                                   mdns::TTL_SERVICE);
    EXPECT(!in.suppressPtr); // stale → still send
    uint8_t rbuf[512];
    const size_t rn = mdns::encodeResponse(rbuf, sizeof(rbuf), qh.id, in);
    mdns::Header rh{};
    std::vector<Rr> rrs;
    EXPECT(parseResponseRecords(rbuf, rn, rh, rrs));
    EXPECT(findRr(rrs, mdns::TYPE_PTR) != nullptr); // sent — querier's copy was stale
}

// ── Driver ──────────────────────────────────────────────────────────────────

int main() {
    std::cout << "Running mDNS protocol tests...\n\n";

    test_writeDnsName_basic();
    test_writeDnsName_singleLabel();
    test_writeDnsName_emptyTerminatorOnly();
    test_writeDnsName_rejectsOversizedLabel();
    test_writeDnsName_acceptsMaxLabel();
    test_writeDnsName_rejectsUndersizedBuffer();
    test_writeDnsName_rejectsBufferTooSmallForTerminator();

    test_readDnsName_roundTrip();
    test_readDnsName_appendsTrailingDot();
    test_readDnsName_atNonZeroOffset();
    test_readDnsName_followsCompressionPointer();
    test_readDnsName_rejectsForwardJump();
    test_readDnsName_rejectsTruncatedLabel();
    test_readDnsName_rejectsPointerPastEnd();

    test_parsePacket_singleQuestion();
    test_parsePacket_nonQuClass();
    test_parsePacket_multipleQuestions();
    test_parsePacket_emptyQuestionSection();
    test_parsePacket_toleratesStaleAnswerCount();
    test_parsePacket_rejectsTruncatedHeader();
    test_parsePacket_rejectsTruncatedQuestion();

    test_parsePacket_surfacesKnownAnswer();
    test_parsePacket_surfacesMultipleKnownAnswers();
    test_parsePacket_cacheFlushMaskedOffKnownAnswer();
    test_parsePacket_fourArgStillSkipsAnswers();
    test_parsePacket_rejectsTruncatedAnswerHeader();
    test_parsePacket_rejectsAnswerRdlenOverrun();
    test_parsePacket_rejectsAnswerCountOverrun();
    test_parsePacket_rejectsAnswerCompressionLoop();

    test_questionMatchesService_ptr();
    test_questionMatchesService_any();
    test_questionMatchesService_caseInsensitive();
    test_questionMatchesService_rejectsWrongType();
    test_questionMatchesService_rejectsWrongName();
    test_questionMatchesService_rejectsPrefixAndSuffix();

    test_encodeResponse_minimumPacketShape();
    test_encodeResponse_includesARecordWhenIpv4Provided();
    test_encodeResponse_ptrTargetsInstanceFqdn();
    test_encodeResponse_srvCarriesPortAndTarget();
    test_encodeResponse_cacheFlushBits();
    test_encodeResponse_writesTxtPairs();
    test_encodeResponse_emptyTxtIsZeroLengthString();
    test_encodeResponse_normalTtls();
    test_encodeResponse_goodbyeUsesTtlZero();
    test_encodeResponse_rejectsUndersizedBuffer();
    test_encodeResponse_rejectsBufferBetweenHeaderAndBody();

    test_encodeAnnouncement_recordSetAndShape();
    test_encodeAnnouncement_cacheFlushBits();
    test_encodeAnnouncement_recordContentMatchesResponse();
    test_encodeAnnouncement_withoutIpv4OmitsARecord();
    test_encodeAnnouncement_normalTtlsNeverZero();

    test_encodeProbeQuery_shape();
    test_encodeProbeQuery_rejectsUndersizedBuffer();

    test_isKnownAnswerSuppressed_freshAnswerSuppresses();
    test_isKnownAnswerSuppressed_lowTtlDoesNotSuppress();
    test_isKnownAnswerSuppressed_typeAndNameMustMatch();
    test_isKnownAnswerSuppressed_caseInsensitiveName();

    test_encodeResponse_suppressPtrOmitsPtr();
    test_encodeResponse_suppressSrvAndTxt();
    test_encodeResponse_suppressAllReturnsZero();
    test_encodeResponse_suppressAOnlyDropsA();

    test_knownAnswer_endToEnd_suppressesKnownPtr();
    test_knownAnswer_endToEnd_staleTtlStillSent();

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
