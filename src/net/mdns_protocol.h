// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * net/mdns_protocol.h — Pure mDNS query/response encoders for the
 * `_satellite._udp.local.` service type (Tier 1 Task 1.6 — replace UDP
 * broadcast with mDNS / Bonjour).
 *
 * mDNS is "regular DNS over UDP multicast (224.0.0.251:5353), with a few
 * extra conventions" — most notably the per-record `cache flush` bit and
 * the use of unicast-response (QU) queries. For our minimal need
 * (responding to PTR queries for our service type with PTR + SRV + TXT
 * records) we don't have to be a full Bonjour responder; we just need to:
 *
 *   - parse incoming DNS questions and pick out the ones asking for
 *     `_satellite._udp.local.` (record type PTR)
 *   - emit a response packet containing PTR + SRV + TXT (+ A when we have
 *     a usable host address)
 *
 * This header is platform-free. Socket / multicast-group plumbing lives in
 * mdns_responder.h. The encoders here are exercised by unit tests with the
 * canonical packet captures from Bonjour / Avahi.
 *
 * Wire references: RFC 1035 (DNS), RFC 6762 (Multicast DNS), RFC 6763
 * (DNS-Based Service Discovery).
 */
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace mdns {

// IPv4 multicast group + port the spec mandates.
inline constexpr uint16_t MULTICAST_PORT = 5353;
inline constexpr const char* MULTICAST_GROUP_V4 = "224.0.0.251";

// Service type the satellite advertises. The fully-qualified instance name
// is `<host-label>._satellite._udp.local.` — host label is the responder's
// own machine name.
inline constexpr const char* SERVICE_TYPE_DOMAIN = "_satellite._udp.local.";

// DNS record types we touch.
inline constexpr uint16_t TYPE_A = 0x0001;
inline constexpr uint16_t TYPE_PTR = 0x000C;
inline constexpr uint16_t TYPE_TXT = 0x0010;
inline constexpr uint16_t TYPE_SRV = 0x0021;
inline constexpr uint16_t TYPE_ANY = 0x00FF;

// IN class + cache-flush bit (RFC 6762 §10.2).
inline constexpr uint16_t CLASS_IN = 0x0001;
inline constexpr uint16_t CACHE_FLUSH_BIT = 0x8000;

// Default TTLs (seconds). 120 s matches the Bonjour default for service
// records; 4500 s matches the spec recommendation for instance / host
// records that don't need to age out quickly.
inline constexpr uint32_t TTL_SERVICE = 120;
inline constexpr uint32_t TTL_HOST = 4500;

// ── Header / question parsing ───────────────────────────────────────────────

struct Header {
    uint16_t id = 0;
    uint16_t flags = 0;
    uint16_t qdCount = 0;
    uint16_t anCount = 0;
    uint16_t nsCount = 0;
    uint16_t arCount = 0;
};

struct Question {
    std::string name; // dot-separated, trailing dot included (e.g. "_satellite._udp.local.")
    uint16_t type = 0;
    uint16_t cls = 0;
    bool unicastResponse = false; // QU bit (high bit of class)
};

// One resource record lifted out of a packet's answer section. mDNS queries
// may carry answers the querier already holds — the "Known-Answer" list of
// RFC 6762 §7.1. We only need the identity (name + type) plus the remaining
// TTL the querier reported, so a responder can decide whether to suppress a
// record from its reply. rdata is intentionally not surfaced — Known-Answer
// suppression keys on name+type+TTL only.
struct Answer {
    std::string name; // dot-separated, trailing dot included
    uint16_t type = 0;
    uint16_t cls = 0;   // cache-flush bit masked off, mirroring Question::cls
    uint32_t ttl = 0;   // remaining TTL the querier reports for this record
};

// Parse an mDNS packet's header + question section. Returns true and fills
// `header` / `questions` on success. The answer / authority / additional
// sections are NOT walked: `header.anCount` is reported verbatim but the
// records themselves are ignored, so a packet whose ANCOUNT overstates the
// records actually present still parses. Use this overload when Known-Answer
// suppression (RFC 6762 §7.1) is not needed; otherwise use the five-argument
// overload below, which validates and surfaces the answer section.
bool parsePacket(const uint8_t* data, size_t len, Header& header, std::vector<Question>& questions);

// As above, but also surfaces the answer-section resource records into
// `knownAnswers` so the responder can perform Known-Answer suppression
// (RFC 6762 §7.1). Authority / additional records are still skipped — only
// the answer section carries a querier's Known-Answer list. RDATA bytes are
// walked over (using the record's RDLENGTH) but not retained. Returns true
// and fills all three out-params on success; false on a malformed packet.
bool parsePacket(const uint8_t* data, size_t len, Header& header,
                 std::vector<Question>& questions, std::vector<Answer>& knownAnswers);

// Decide, for a single record we would emit, whether RFC 6762 §7.1
// Known-Answer suppression applies: true iff `knownAnswers` contains a record
// with the same `recordName` (case-insensitive) and `recordType` whose
// reported TTL is at least half of `ourTtl`. A querier that already holds a
// sufficiently-fresh copy does not need us to repeat it.
bool isKnownAnswerSuppressed(const std::vector<Answer>& knownAnswers, const std::string& recordName,
                             uint16_t recordType, uint32_t ourTtl);

// True iff `question` targets the satellite service: a PTR or ANY record
// whose name matches `_satellite._udp.local.`. DNS names are case-insensitive
// (RFC 1035 §2.3.3), so the comparison folds case. Pure — exercised directly
// by the protocol unit tests; the responder calls it to decide what to answer.
bool questionMatchesService(const Question& question);

// ── Response encoding ───────────────────────────────────────────────────────

// Build a response packet containing PTR + SRV + TXT records for our
// `_satellite._udp.local.` service plus an optional A record when an IPv4
// address is supplied. The instance label is `<instanceName>._satellite._udp.local.`;
// the SRV record points at `<hostName>.local.`.
//
// `txtPairs` is rendered as the canonical "key=value" DNS-SD TXT format,
// one length-prefixed string per pair. Keys MUST be ASCII; values are
// passed through verbatim.
//
// `ipv4` is the four-byte network-order representation of the responder's
// LAN address (e.g. derived from `inet_pton`). Pass `nullptr` to skip the
// A record (useful in tests where we only care about the service-level
// fields).
//
// `txId` should mirror the request's transaction ID. mDNS responses always
// carry QR=1 + AA=1 regardless of the query's QU hint, so the response *bytes*
// never depend on unicast vs multicast — only the destination does, and that
// is the responder's concern (mdns_responder.cpp), not this encoder.
//
// Set `inputs.goodbye` to emit every record with TTL 0 — the RFC 6762 §10.1
// "goodbye" announcement that retracts the service when the responder shuts
// down, so caches on the segment flush promptly instead of holding a dead
// entry until the normal TTL expires.
//
// Set the `suppress*` flags to omit individual records from the response.
// This is how the responder applies RFC 6762 §7.1 Known-Answer suppression:
// a record the querier already holds (with a fresh-enough TTL) is dropped
// from the reply. ANCOUNT is adjusted to match. If every record is
// suppressed, encodeResponse returns 0 and the caller sends nothing.
//
// Returns the number of bytes written, or 0 if `outCap` is too small (or
// every record was suppressed).
struct ResponseInputs {
    std::string instanceName; // e.g. "satellite-laptop"
    std::string hostName;     // e.g. "laptop" (the .local. suffix is appended internally)
    uint16_t udpPort = 0;     // SRV target port (the dish UDP gamepad port)
    uint16_t weight = 0;      // SRV weight (we always use 0 — single replica)
    uint16_t priority = 0;    // SRV priority (we always use 0)
    std::vector<std::pair<std::string, std::string>> txtPairs;
    const uint8_t* ipv4 = nullptr; // 4 bytes, network order; or nullptr
    bool goodbye = false;          // true → every RR carries TTL 0 (service retraction)
    // RFC 6762 §7.1 Known-Answer suppression — drop a record the querier
    // already knows. The A record is also implicitly suppressed when `ipv4`
    // is null; `suppressA` lets the responder drop it even when an address
    // is available.
    bool suppressPtr = false;
    bool suppressSrv = false;
    bool suppressTxt = false;
    bool suppressA = false;
};

size_t encodeResponse(uint8_t* out, size_t outCap, uint16_t txId, const ResponseInputs& inputs);

// Encode an unsolicited announcement (RFC 6762 §8.3): the full PTR + SRV +
// TXT (+ A) answer set the responder multicasts when it first comes up, so
// that senders already running learn about the service without having to
// re-query. The wire form is identical to a query response — QR=1, AA=1,
// cache-flush set on the unique SRV/TXT/A records and clear on the shared
// PTR — the only conceptual difference is that it answers no specific query,
// so the transaction ID is fixed at 0. `inputs.goodbye` and the `suppress*`
// flags must not be set for an announcement; they are ignored here.
//
// Returns the number of bytes written, or 0 if `outCap` is too small.
size_t encodeAnnouncement(uint8_t* out, size_t outCap, const ResponseInputs& inputs);

// Encode a probe query (RFC 6762 §8.1): a single question of type ANY for
// our instance FQDN `<instanceName>._satellite._udp.local.`, asked before
// announcing so a name clash on the segment can be detected. The question is
// sent with the QU (unicast-response) bit clear — probes are multicast — and
// QDCOUNT is 1 with all other counts 0. (We send a minimal probe: the §8.1
// "proposed records in the authority section" tie-break is intentionally not
// implemented; see mdns_responder.cpp.)
//
// Returns the number of bytes written, or 0 if `outCap` is too small.
size_t encodeProbeQuery(uint8_t* out, size_t outCap, const std::string& instanceName);

// ── Helpers exposed for unit tests ──────────────────────────────────────────

// Write a DNS name as length-prefixed labels followed by a terminating 0.
// e.g. "foo.bar.local." → 0x03 'f' 'o' 'o' 0x03 'b' 'a' 'r' 0x05 'l' 'o' 'c'
// 'a' 'l' 0x00. Returns bytes written or 0 if outCap is too small.
//
// This implementation does NOT do DNS message compression; mDNS clients
// tolerate uncompressed names just fine for short service domains. Skipping
// compression keeps the encoder + test surface trivial.
size_t writeDnsName(uint8_t* out, size_t outCap, const std::string& dottedName);

// Reverse of writeDnsName. Handles compression pointers (0xC0xx). Returns
// the number of bytes consumed in the source on success (0 on malformed
// input).
size_t readDnsName(const uint8_t* packet, size_t packetLen, size_t offset, std::string& outName);

} // namespace mdns
