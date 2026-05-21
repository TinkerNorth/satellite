// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * net/mdns_protocol.h — Pure mDNS query/response encoders for the
 * `_satellite._udp.local.` service type (Tier 1 Task 1.6 — replace UDP
 * broadcast with mDNS / Bonjour).
 *
 * mDNS is "regular DNS over UDP multicast (224.0.0.251:5353), with a few
 * extra conventions" — most notably the per-record `cache flush` bit and
 * the use of unicast-response (QU) queries. This module provides the wire
 * encoders/parsers the responder needs to:
 *
 *   - parse incoming DNS questions and pick out the ones asking for
 *     `_satellite._udp.local.` (record type PTR)
 *   - emit a response packet containing PTR + SRV + TXT (+ A when we have
 *     a usable host address)
 *   - encode a full RFC 6762 §8.1 probe query (ANY question + the proposed
 *     unique records in the authority section) and parse an inbound probe's
 *     authority section, so the responder can run the §8.2 simultaneous-
 *     probe tiebreak before claiming its name
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
    uint16_t cls = 0; // cache-flush bit masked off, mirroring Question::cls
    uint32_t ttl = 0; // remaining TTL the querier reports for this record
};

// One resource record lifted out of a packet's *authority* section, used by
// the RFC 6762 §8.2 simultaneous-probe tiebreak. Unlike `Answer`, the rdata
// IS retained: the tiebreak (§8.2.1) compares rdata byte-for-byte. The bytes
// are stored *canonical* — any DNS-name fields inside the rdata have been
// decompressed (RFC 6762 §8.2 requires names be uncompressed before
// comparison); see canonicalizeAuthorityRdata in the .cpp. `cls` keeps the
// cache-flush bit masked off, matching `Answer`; the comparator (§8.2)
// compares class "excluding the cache-flush bit".
struct ProbeRecord {
    std::string name; // dot-separated, trailing dot included
    uint16_t type = 0;
    uint16_t cls = 0;           // cache-flush bit masked off
    uint32_t ttl = 0;           // informational; not used by the tiebreak
    std::vector<uint8_t> rdata; // canonical (names uncompressed) raw rdata
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
bool parsePacket(const uint8_t* data, size_t len, Header& header, std::vector<Question>& questions,
                 std::vector<Answer>& knownAnswers);

// As the five-argument overload, but ALSO surfaces the authority-section
// resource records into `authority`. An mDNS probe query (RFC 6762 §8.1)
// carries the prober's proposed unique records in the authority section; a
// responder that is itself probing reads them to run the §8.2 simultaneous-
// probe tiebreak. Unlike the answer/Known-Answer records, authority RDATA IS
// retained — the §8.2.1 comparison is byte-for-byte over the rdata — and is
// stored in canonical form (DNS names embedded in SRV/PTR rdata are
// decompressed, per §8.2). Additional-section records are still skipped.
// Returns true and fills all four out-params on success; false on a
// malformed packet (a bad name, or an rdlength that overruns the buffer).
bool parsePacket(const uint8_t* data, size_t len, Header& header, std::vector<Question>& questions,
                 std::vector<Answer>& knownAnswers, std::vector<ProbeRecord>& authority);

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

// Encode a full RFC 6762 §8.1 probe query for the satellite instance name
// `<instanceName>._satellite._udp.local.`, asked three times before
// announcing so a name clash on the segment is detected.
//
// Wire form (RFC 6762 §8.1 / §8.2):
//   - QDCOUNT 1: a single question of type ANY (255) for the instance FQDN,
//     "to elicit answers for all types of records with that name". §8.1 says
//     probe questions SHOULD set the QU (unicast-response) bit — because of
//     the multicast rate-limiting rules — so the question class carries
//     CACHE_FLUSH_BIT (the QU bit) set.
//   - NSCOUNT > 0: the authority section is populated with "the record or
//     records with the rdata that [the host] would be proposing to use" —
//     here the SRV and TXT for the instance name, plus the A record for the
//     host when an IPv4 address is supplied. This is what lets a peer that
//     is simultaneously probing run the §8.2 tiebreak. The records carry
//     class IN with the cache-flush bit *clear* in the authority section
//     (the bit is a response-side cache directive; a probe is a query).
//   - ANCOUNT / ARCOUNT 0; transaction ID 0.
//
// `inputs` supplies the instance/host labels, SRV port and TXT pairs exactly
// as for encodeResponse; `inputs.ipv4`, when non-null, adds the proposed A
// record to the authority section. The `goodbye` / `suppress*` flags are
// ignored. Returns bytes written, or 0 if `outCap` is too small.
size_t encodeProbeQuery(uint8_t* out, size_t outCap, const ResponseInputs& inputs);

// Build the set of unique resource records this host proposes to claim for
// `instanceName` — exactly the records encodeProbeQuery puts in a probe's
// authority section: the SRV and TXT for `<instanceName>._satellite._udp.local.`
// and, when `inputs.ipv4` is non-null, the A record for `<hostName>.local.`.
// The PTR is shared, not unique, so it is deliberately excluded (RFC 6762
// §8.1 probes only the records "that [the host] desires to be unique").
//
// Each record's rdata is built in canonical form (DNS names uncompressed) so
// the result can be fed straight into compareRecordSets for the §8.2
// tiebreak. Used both to encode our own probe and to represent "our side"
// when comparing against an inbound peer probe.
std::vector<ProbeRecord> buildProposedRecords(const ResponseInputs& inputs);

// RFC 6762 §8.2 / §8.2.1 simultaneous-probe tiebreak comparator. Returns:
//   <0  — `ours` sorts lexicographically EARLIER (we lose: per §8.2 the
//         caller must defer — wait one second, then re-probe)
//    0  — the two sets are byte-for-byte identical (§8.2.1: "two devices
//         are advertising identical sets of records … there is, in fact,
//         no conflict")
//   >0  — `ours` sorts lexicographically LATER (we win: ignore the peer
//         probe and continue our sequence)
//
// Both vectors are sorted internally (the function does not require the
// caller to pre-sort, and does not mutate the caller's vectors). The §8.2.1
// ordering compares record class first (cache-flush bit already masked off
// by the parser / builder), then record type, then the raw rdata bytes as
// unsigned values; on a length-mismatched rdata prefix the record that
// "still has remaining data" sorts later. Sets are then compared pairwise;
// if one list runs out first, the list with records remaining wins.
int compareRecordSets(std::vector<ProbeRecord> ours, std::vector<ProbeRecord> theirs);

// True iff `authority` (the records from an inbound probe) contains at least
// one record whose name case-insensitively equals `name` — i.e. the inbound
// probe is proposing a record for a name we are also probing, so the §8.2
// tiebreak applies rather than an outright conflict. `name` is the full FQDN.
bool authorityHasRecordFor(const std::vector<ProbeRecord>& authority, const std::string& name);

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
