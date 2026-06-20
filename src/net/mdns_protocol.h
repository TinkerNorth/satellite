// SPDX-License-Identifier: LGPL-3.0-or-later

// Pure (platform-free) mDNS query/response encoders for the
// `_satellite._udp.local.` service type. Socket plumbing lives in
// mdns_responder.h. Wire refs: RFC 1035 (DNS), RFC 6762 (mDNS), RFC 6763 (DNS-SD).
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

// Service type the satellite advertises. Instance name is
// `<host-label>._satellite._udp.local.`, host label = the responder's machine name.
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

// Default TTLs (seconds): 120 s = Bonjour default for service records;
// 4500 s = spec recommendation for instance/host records.
inline constexpr uint32_t TTL_SERVICE = 120;
inline constexpr uint32_t TTL_HOST = 4500;

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

// A record from a packet's answer section: the querier's Known-Answer list
// (RFC 6762 §7.1). rdata is not surfaced: suppression keys on name+type+TTL.
struct Answer {
    std::string name; // dot-separated, trailing dot included
    uint16_t type = 0;
    uint16_t cls = 0; // cache-flush bit masked off, mirroring Question::cls
    uint32_t ttl = 0; // remaining TTL the querier reports for this record
};

// A record from a packet's authority section, for the RFC 6762 §8.2
// simultaneous-probe tiebreak. Unlike `Answer`, rdata IS retained (§8.2.1
// compares it byte-for-byte) and stored canonical (embedded DNS names
// decompressed). cls has the cache-flush bit masked off.
struct ProbeRecord {
    std::string name; // dot-separated, trailing dot included
    uint16_t type = 0;
    uint16_t cls = 0;           // cache-flush bit masked off
    uint32_t ttl = 0;           // informational; not used by the tiebreak
    std::vector<uint8_t> rdata; // canonical (names uncompressed) raw rdata
};

// Parse header + question section only. Answer/authority/additional are NOT
// walked, so a packet whose ANCOUNT overstates the records present still parses.
bool parsePacket(const uint8_t* data, size_t len, Header& header, std::vector<Question>& questions);

// Also surfaces the answer section into `knownAnswers` for Known-Answer
// suppression (RFC 6762 §7.1). RDATA is walked but not retained. Authority/
// additional skipped.
bool parsePacket(const uint8_t* data, size_t len, Header& header, std::vector<Question>& questions,
                 std::vector<Answer>& knownAnswers);

// Also surfaces the authority section into `authority`: a probe's proposed
// unique records (RFC 6762 §8.1), read to run the §8.2 tiebreak. Authority
// RDATA IS retained, canonical (names decompressed). False on a malformed
// packet (bad name, or rdlength overrunning the buffer).
bool parsePacket(const uint8_t* data, size_t len, Header& header, std::vector<Question>& questions,
                 std::vector<Answer>& knownAnswers, std::vector<ProbeRecord>& authority);

// RFC 6762 §7.1: true iff `knownAnswers` holds a record matching `recordName`
// (case-insensitive) + `recordType` with TTL >= half of `ourTtl`, a copy fresh
// enough that we need not repeat it.
bool isKnownAnswerSuppressed(const std::vector<Answer>& knownAnswers, const std::string& recordName,
                             uint16_t recordType, uint32_t ourTtl);

// True iff `question` is a PTR or ANY for `_satellite._udp.local.`. Case-folded.
bool questionMatchesService(const Question& question);

// Build a PTR + SRV + TXT (+ optional A) response for our service. Instance
// label is `<instanceName>._satellite._udp.local.`; SRV target `<hostName>.local.`.
//
// `txtPairs`: canonical DNS-SD "key=value", one length-prefixed string each;
// keys MUST be ASCII. `ipv4`: 4 bytes network order, or nullptr to skip the A
// record. `txId` mirrors the request's transaction ID.
//
// `goodbye`: emit every record with TTL 0 (RFC 6762 §10.1 retraction) so caches
// flush promptly on shutdown. `suppress*`: drop individual records for §7.1
// Known-Answer suppression; ANCOUNT is adjusted. Returns bytes written, or 0 if
// `outCap` too small or every record suppressed.
struct ResponseInputs {
    std::string instanceName; // e.g. "satellite-laptop"
    std::string hostName;     // e.g. "laptop" (.local. appended internally)
    uint16_t udpPort = 0;     // SRV target port (the dish UDP gamepad port)
    uint16_t weight = 0;      // SRV weight (always 0, single replica)
    uint16_t priority = 0;    // SRV priority (always 0)
    std::vector<std::pair<std::string, std::string>> txtPairs;
    const uint8_t* ipv4 = nullptr; // 4 bytes, network order; or nullptr
    bool goodbye = false;          // true: every RR carries TTL 0 (service retraction)
    // RFC 6762 §7.1 suppression. A is also implicitly suppressed when `ipv4` is
    // null; `suppressA` drops it even when an address is available.
    bool suppressPtr = false;
    bool suppressSrv = false;
    bool suppressTxt = false;
    bool suppressA = false;
};

size_t encodeResponse(uint8_t* out, size_t outCap, uint16_t txId, const ResponseInputs& inputs);

// Unsolicited announcement (RFC 6762 §8.3): wire-identical to a query response
// but answers no query, so txId is 0. `goodbye`/`suppress*` are ignored.
size_t encodeAnnouncement(uint8_t* out, size_t outCap, const ResponseInputs& inputs);

// Full RFC 6762 §8.1 probe query for `<instanceName>._satellite._udp.local.`:
//   QDCOUNT 1: one ANY question for the instance FQDN; class carries
//     CACHE_FLUSH_BIT (the QU bit) set (§8.1 probe questions SHOULD be QU).
//   NSCOUNT > 0: authority holds the proposed records (SRV+TXT, +A when ipv4
//     given) for the §8.2 tiebreak. Class IN with cache-flush bit CLEAR (it's a
//     response-side directive; a probe is a query).
//   ANCOUNT/ARCOUNT 0; txId 0.
// `goodbye`/`suppress*` ignored. Returns bytes written, or 0 if outCap too small.
size_t encodeProbeQuery(uint8_t* out, size_t outCap, const ResponseInputs& inputs);

// The unique records this host proposes for `instanceName`: what encodeProbeQuery
// puts in the authority section (SRV+TXT, +A when ipv4 given). Shared PTR
// excluded. rdata canonical (names uncompressed) for compareRecordSets.
std::vector<ProbeRecord> buildProposedRecords(const ResponseInputs& inputs);

// RFC 6762 §8.2.1 simultaneous-probe tiebreak comparator. <0: `ours` sorts
// earlier (we lose, defer 1 s, re-probe). 0: identical sets. >0: we win. Both
// vectors sorted internally (not mutated). Order: class, then type, then rdata
// bytes unsigned; the record with data remaining on a length-mismatched prefix
// sorts later; if a list runs out first, the longer list wins.
int compareRecordSets(std::vector<ProbeRecord> ours, std::vector<ProbeRecord> theirs);

// True iff `authority` holds a record whose name case-insensitively equals
// `name`: the §8.2 tiebreak applies rather than an outright conflict.
bool authorityHasRecordFor(const std::vector<ProbeRecord>& authority, const std::string& name);

// Write a DNS name as length-prefixed labels + terminating 0. Returns bytes
// written or 0 if outCap too small. No DNS compression: mDNS clients tolerate
// uncompressed names, and it keeps the encoder trivial.
size_t writeDnsName(uint8_t* out, size_t outCap, const std::string& dottedName);

// Reverse of writeDnsName. Handles compression pointers (0xC0xx). Returns bytes
// consumed in the source, or 0 on malformed input.
size_t readDnsName(const uint8_t* packet, size_t packetLen, size_t offset, std::string& outName);

} // namespace mdns
