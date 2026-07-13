// SPDX-License-Identifier: LGPL-3.0-or-later

#include "mdns_protocol.h"

#include <algorithm>
#include <cctype>

namespace mdns {

namespace {

inline void putBE16(uint8_t* dst, uint16_t v) {
    dst[0] = static_cast<uint8_t>(v >> 8);
    dst[1] = static_cast<uint8_t>(v);
}

inline void putBE32(uint8_t* dst, uint32_t v) {
    dst[0] = static_cast<uint8_t>(v >> 24);
    dst[1] = static_cast<uint8_t>(v >> 16);
    dst[2] = static_cast<uint8_t>(v >> 8);
    dst[3] = static_cast<uint8_t>(v);
}

inline uint16_t readBE16(const uint8_t* src) {
    return static_cast<uint16_t>(src[0]) << 8 | static_cast<uint16_t>(src[1]);
}

} // namespace

size_t writeDnsName(uint8_t* out, size_t outCap, const std::string& dottedName) {
    if (out == nullptr) return 0;
    size_t pos = 0;
    size_t i = 0;
    while (i < dottedName.size()) {
        size_t j = dottedName.find('.', i);
        if (j == std::string::npos) j = dottedName.size();
        const size_t labelLen = j - i;
        if (labelLen > 63) return 0; // RFC 1035 hard limit
        if (labelLen == 0) break;    // trailing dot: write terminator and stop
        if (pos + 1 + labelLen > outCap) return 0;
        out[pos++] = static_cast<uint8_t>(labelLen);
        std::memcpy(out + pos, dottedName.data() + i, labelLen);
        pos += labelLen;
        i = j + 1;
    }
    if (pos + 1 > outCap) return 0;
    out[pos++] = 0; // terminator
    return pos;
}

size_t readDnsName(const uint8_t* packet, size_t packetLen, size_t offset, std::string& outName) {
    outName.clear();
    if (packet == nullptr) return 0;
    size_t pos = offset;
    size_t consumedBeforeJump = 0; // bytes consumed before any compression jump
    bool jumped = false;
    int safety = 64; // hard cap so a malformed compression loop can't hang us

    while (safety-- > 0) {
        if (pos >= packetLen) return 0;
        const uint8_t b = packet[pos];
        if (b == 0) {
            // Terminator. After a compression jump, only bytes up to the first
            // jump count toward consumption.
            if (!jumped)
                consumedBeforeJump = pos - offset + 1;
            else if (consumedBeforeJump == 0)
                return 0;
            return consumedBeforeJump;
        }
        if ((b & 0xC0) == 0xC0) {
            // Compression pointer: 2 bytes total, target is the low 14 bits.
            if (pos + 1 >= packetLen) return 0;
            const uint16_t target =
                (static_cast<uint16_t>(b & 0x3F) << 8) | static_cast<uint16_t>(packet[pos + 1]);
            if (!jumped) consumedBeforeJump = pos - offset + 2;
            jumped = true;
            if (target >= packetLen || target >= pos) return 0; // forbid forward jumps to here
            pos = target;
            continue;
        }
        if ((b & 0xC0) != 0) return 0; // reserved bits set
        const size_t labelLen = b;
        if (pos + 1 + labelLen > packetLen) return 0;
        outName.append(reinterpret_cast<const char*>(packet + pos + 1), labelLen);
        outName.push_back('.');
        pos += 1 + labelLen;
    }
    return 0;
}

namespace {

// Parse the 12-byte header + QDCOUNT questions, advancing `pos` past the
// question section. Shared by the parsePacket overloads.
bool parseHeaderAndQuestions(const uint8_t* data, size_t len, Header& header,
                             std::vector<Question>& questions, size_t& pos) {
    if (data == nullptr || len < 12) return false;
    header.id = readBE16(data + 0);
    header.flags = readBE16(data + 2);
    header.qdCount = readBE16(data + 4);
    header.anCount = readBE16(data + 6);
    header.nsCount = readBE16(data + 8);
    header.arCount = readBE16(data + 10);

    pos = 12;
    questions.clear();
    for (uint16_t i = 0; i < header.qdCount; ++i) {
        Question q;
        const size_t consumed = readDnsName(data, len, pos, q.name);
        if (consumed == 0) return false;
        pos += consumed;
        if (pos + 4 > len) return false;
        q.type = readBE16(data + pos);
        const uint16_t rawClass = readBE16(data + pos + 2);
        q.unicastResponse = (rawClass & CACHE_FLUSH_BIT) != 0;
        q.cls = rawClass & ~CACHE_FLUSH_BIT;
        pos += 4;
        questions.push_back(std::move(q));
    }
    return true;
}

} // namespace

bool parsePacket(const uint8_t* data, size_t len, Header& header,
                 std::vector<Question>& questions) {
    // Header + questions only; answer/authority/additional not walked, so a
    // non-zero ANCOUNT with no records present is tolerated.
    size_t pos = 0;
    return parseHeaderAndQuestions(data, len, header, questions, pos);
}

namespace {

// Parse the answer section (RFC 6762 §7.1 Known-Answer list) starting at
// `pos`, appending each record's identity (name/type/class/ttl) to
// `knownAnswers` and skipping the rdata. Advances `pos` past the section.
// Returns false on a malformed record. Shared by the 5- and 6-arg overloads.
bool parseAnswerSection(const uint8_t* data, size_t len, uint16_t count,
                        std::vector<Answer>& knownAnswers, size_t& pos) {
    for (uint16_t i = 0; i < count; ++i) {
        Answer a;
        const size_t consumed = readDnsName(data, len, pos, a.name);
        if (consumed == 0) return false;
        pos += consumed;
        if (pos + 10 > len) return false;
        a.type = readBE16(data + pos);
        a.cls = readBE16(data + pos + 2) & ~CACHE_FLUSH_BIT;
        a.ttl = (static_cast<uint32_t>(data[pos + 4]) << 24) |
                (static_cast<uint32_t>(data[pos + 5]) << 16) |
                (static_cast<uint32_t>(data[pos + 6]) << 8) | static_cast<uint32_t>(data[pos + 7]);
        const uint16_t rdlen = readBE16(data + pos + 8);
        pos += 10;
        if (pos + rdlen > len) return false; // rdata overruns the packet
        pos += rdlen;                        // skip rdata; identity is name+type
        knownAnswers.push_back(std::move(a));
    }
    return true;
}

// Rewrite raw on-wire rdata into the canonical form RFC 6762 §8.2 tiebreak
// requires: embedded DNS names decompressed. SRV (name after
// priority/weight/port) and PTR (a bare name) are expanded against the whole
// packet; A/TXT and others are copied verbatim. False if a name fails to decode.
bool canonicalizeAuthorityRdata(const uint8_t* data, size_t len, uint16_t type, size_t rdataStart,
                                uint16_t rdlen, std::vector<uint8_t>& out) {
    out.clear();
    if (type == TYPE_SRV) {
        // priority(2) + weight(2) + port(2) then a domain name.
        if (rdlen < 6) return false;
        out.insert(out.end(), data + rdataStart, data + rdataStart + 6);
        std::string target;
        const size_t consumed = readDnsName(data, len, rdataStart + 6, target);
        if (consumed == 0) return false;
        uint8_t nameBuf[256];
        const size_t n = writeDnsName(nameBuf, sizeof(nameBuf), target);
        if (n == 0) return false;
        out.insert(out.end(), nameBuf, nameBuf + n);
        return true;
    }
    if (type == TYPE_PTR) {
        std::string target;
        const size_t consumed = readDnsName(data, len, rdataStart, target);
        if (consumed == 0) return false;
        uint8_t nameBuf[256];
        const size_t n = writeDnsName(nameBuf, sizeof(nameBuf), target);
        if (n == 0) return false;
        out.insert(out.end(), nameBuf, nameBuf + n);
        return true;
    }
    // A, TXT, and anything else: rdata is already name-free, copy verbatim.
    out.insert(out.end(), data + rdataStart, data + rdataStart + rdlen);
    return true;
}

} // namespace

bool parsePacket(const uint8_t* data, size_t len, Header& header, std::vector<Question>& questions,
                 std::vector<Answer>& knownAnswers) {
    size_t pos = 0;
    knownAnswers.clear();
    if (!parseHeaderAndQuestions(data, len, header, questions, pos)) return false;
    // Answer section = the querier's Known-Answer list (RFC 6762 §7.1).
    return parseAnswerSection(data, len, header.anCount, knownAnswers, pos);
}

bool parsePacket(const uint8_t* data, size_t len, Header& header, std::vector<Question>& questions,
                 std::vector<Answer>& knownAnswers, std::vector<ProbeRecord>& authority) {
    size_t pos = 0;
    knownAnswers.clear();
    authority.clear();
    if (!parseHeaderAndQuestions(data, len, header, questions, pos)) return false;
    // Answer section precedes authority on the wire; walk it to reach authority.
    if (!parseAnswerSection(data, len, header.anCount, knownAnswers, pos)) return false;

    // Authority section: a probe's proposed unique records (RFC 6762 §8.1):
    // name + type(2) + class(2) + ttl(4) + rdlength(2) + rdata. rdata IS
    // retained, canonicalised (names uncompressed) for the §8.2 tiebreak.
    for (uint16_t i = 0; i < header.nsCount; ++i) {
        ProbeRecord r;
        const size_t consumed = readDnsName(data, len, pos, r.name);
        if (consumed == 0) return false;
        pos += consumed;
        if (pos + 10 > len) return false;
        r.type = readBE16(data + pos);
        r.cls = readBE16(data + pos + 2) & ~CACHE_FLUSH_BIT;
        r.ttl = (static_cast<uint32_t>(data[pos + 4]) << 24) |
                (static_cast<uint32_t>(data[pos + 5]) << 16) |
                (static_cast<uint32_t>(data[pos + 6]) << 8) | static_cast<uint32_t>(data[pos + 7]);
        const uint16_t rdlen = readBE16(data + pos + 8);
        pos += 10;
        if (pos + rdlen > len) return false; // rdata overruns the packet
        if (!canonicalizeAuthorityRdata(data, len, r.type, pos, rdlen, r.rdata)) return false;
        pos += rdlen;
        authority.push_back(std::move(r));
    }
    return true;
}

bool isKnownAnswerSuppressed(const std::vector<Answer>& knownAnswers, const std::string& recordName,
                             uint16_t recordType, uint32_t ourTtl) {
    // DNS labels are case-insensitive (RFC 1035 §2.3.3); Bonjour/Avahi may
    // capitalise differently.
    auto nameEqual = [](const std::string& x, const std::string& y) {
        if (x.size() != y.size()) return false;
        for (size_t i = 0; i < x.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(x[i])) !=
                std::tolower(static_cast<unsigned char>(y[i])))
                return false;
        }
        return true;
    };
    for (const auto& a : knownAnswers) {
        if (a.type != recordType) continue;
        if (!nameEqual(a.name, recordName)) continue;
        // RFC 6762 §7.1: suppress only if the querier's copy is at least half as
        // fresh as ours. A near-expired TTL still wants the record refreshed.
        if (a.ttl >= ourTtl / 2) return true;
    }
    return false;
}

bool questionMatchesService(const Question& question) {
    if (question.type != TYPE_PTR && question.type != TYPE_ANY) return false;
    // DNS labels are case-insensitive, and Bonjour/Avahi sometimes capitalise
    // differently.
    const std::string& a = question.name;
    const char* b = SERVICE_TYPE_DOMAIN;
    size_t i = 0;
    for (; i < a.size() && b[i] != '\0'; ++i) {
        const auto la = static_cast<unsigned char>(a[i]);
        const auto lb = static_cast<unsigned char>(b[i]);
        if (std::tolower(la) != std::tolower(lb)) return false;
    }
    return i == a.size() && b[i] == '\0';
}

namespace {

// Write one RR header (name + type + class + ttl + rdlength placeholder),
// returning the rdlength field offset for the caller to backfill.
struct RrCursor {
    size_t rdlenOffset = 0;
    bool ok = true;
};

RrCursor writeRrHeader(uint8_t* out, size_t outCap, size_t& pos, const std::string& name,
                       uint16_t type, uint16_t cls, uint32_t ttl) {
    RrCursor c{};
    const size_t n = writeDnsName(out + pos, outCap - pos, name);
    if (n == 0) {
        c.ok = false;
        return c;
    }
    pos += n;
    if (pos + 10 > outCap) {
        c.ok = false;
        return c;
    }
    putBE16(out + pos, type);
    putBE16(out + pos + 2, cls);
    putBE32(out + pos + 4, ttl);
    c.rdlenOffset = pos + 8;
    putBE16(out + pos + 8, 0); // placeholder; finalised later
    pos += 10;
    return c;
}

void finalizeRr(uint8_t* out, RrCursor& c, size_t pos) {
    if (!c.ok) return;
    const size_t rdata = pos - (c.rdlenOffset + 2);
    putBE16(out + c.rdlenOffset, static_cast<uint16_t>(rdata));
}

} // namespace

size_t encodeResponse(uint8_t* out, size_t outCap, uint16_t txId, const ResponseInputs& inputs) {
    if (out == nullptr || outCap < 12) return 0;

    const std::string serviceType = SERVICE_TYPE_DOMAIN;
    const std::string instanceFqdn = inputs.instanceName + "." + serviceType;
    const std::string hostFqdn = inputs.hostName + ".local.";

    // Goodbye announcements (RFC 6762 §10.1) re-send the records with TTL 0
    // so resolver caches drop the service immediately on shutdown.
    const uint32_t ttlService = inputs.goodbye ? 0u : TTL_SERVICE;
    const uint32_t ttlHost = inputs.goodbye ? 0u : TTL_HOST;

    // PTR/SRV/TXT normally all present; any may be dropped by §7.1 suppression.
    // A additionally requires an IPv4 address.
    const bool emitPtr = !inputs.suppressPtr;
    const bool emitSrv = !inputs.suppressSrv;
    const bool emitTxt = !inputs.suppressTxt;
    const bool emitA = (inputs.ipv4 != nullptr) && !inputs.suppressA;
    const uint16_t answerCount = static_cast<uint16_t>((emitPtr ? 1 : 0) + (emitSrv ? 1 : 0) +
                                                       (emitTxt ? 1 : 0) + (emitA ? 1 : 0));
    // Every record suppressed → "send nothing" (RFC 6762 §7.1).
    if (answerCount == 0) return 0;

    size_t pos = 0;
    putBE16(out + pos, txId);
    pos += 2;
    // mDNS responses always carry QR=1 + AA=1 (RFC 6762 §18.2/§18.4); the
    // bytes never depend on unicast vs multicast, only the destination does.
    const uint16_t flags = 0x8400;
    putBE16(out + pos, flags);
    pos += 2;
    putBE16(out + pos, 0); // QDCOUNT
    pos += 2;
    putBE16(out + pos, answerCount);
    pos += 2;
    putBE16(out + pos, 0); // NSCOUNT
    pos += 2;
    putBE16(out + pos, 0); // ARCOUNT
    pos += 2;

    if (emitPtr) {
        auto c = writeRrHeader(out, outCap, pos, serviceType, TYPE_PTR, CLASS_IN, ttlService);
        if (!c.ok) return 0;
        const size_t n = writeDnsName(out + pos, outCap - pos, instanceFqdn);
        if (n == 0) return 0;
        pos += n;
        finalizeRr(out, c, pos);
    }
    if (emitSrv) {
        auto c = writeRrHeader(out, outCap, pos, instanceFqdn, TYPE_SRV, CLASS_IN | CACHE_FLUSH_BIT,
                               ttlHost);
        if (!c.ok) return 0;
        if (pos + 6 > outCap) return 0;
        putBE16(out + pos, inputs.priority);
        putBE16(out + pos + 2, inputs.weight);
        putBE16(out + pos + 4, inputs.udpPort);
        pos += 6;
        const size_t n = writeDnsName(out + pos, outCap - pos, hostFqdn);
        if (n == 0) return 0;
        pos += n;
        finalizeRr(out, c, pos);
    }
    // TXT: one length-prefixed "key=value" per entry; empty TXT = a single
    // zero-length string (DNS-SD §6.1).
    if (emitTxt) {
        auto c = writeRrHeader(out, outCap, pos, instanceFqdn, TYPE_TXT, CLASS_IN | CACHE_FLUSH_BIT,
                               ttlHost);
        if (!c.ok) return 0;
        if (inputs.txtPairs.empty()) {
            if (pos + 1 > outCap) return 0;
            out[pos++] = 0;
        } else {
            for (const auto& [k, v] : inputs.txtPairs) {
                std::string entry = k + "=" + v;
                if (entry.size() > 255) return 0;
                if (pos + 1 + entry.size() > outCap) return 0;
                out[pos++] = static_cast<uint8_t>(entry.size());
                std::memcpy(out + pos, entry.data(), entry.size());
                pos += entry.size();
            }
        }
        finalizeRr(out, c, pos);
    }
    if (emitA) {
        auto c =
            writeRrHeader(out, outCap, pos, hostFqdn, TYPE_A, CLASS_IN | CACHE_FLUSH_BIT, ttlHost);
        if (!c.ok) return 0;
        if (pos + 4 > outCap) return 0;
        std::memcpy(out + pos, inputs.ipv4, 4);
        pos += 4;
        finalizeRr(out, c, pos);
    }
    return pos;
}

size_t encodeAnnouncement(uint8_t* out, size_t outCap, const ResponseInputs& inputs) {
    // RFC 6762 §8.3: wire-identical to a query response with txId 0 and no
    // suppression. Reuse encodeResponse so the paths can't diverge.
    ResponseInputs ann = inputs;
    ann.goodbye = false;
    ann.suppressPtr = false;
    ann.suppressSrv = false;
    ann.suppressTxt = false;
    ann.suppressA = false;
    return encodeResponse(out, outCap, /*txId=*/0, ann);
}

// Write one authority-section RR (RFC 6762 §8.1 probe): header + rdata, then
// finalise rdlength. `cls` is verbatim: a probe carries IN, cache-flush clear.
namespace {
bool writeAuthorityRecord(uint8_t* out, size_t outCap, size_t& pos, const std::string& name,
                          uint16_t type, uint16_t cls, uint32_t ttl, const uint8_t* rdata,
                          size_t rdlen) {
    auto c = writeRrHeader(out, outCap, pos, name, type, cls, ttl);
    if (!c.ok) return false;
    if (pos + rdlen > outCap) return false;
    if (rdlen > 0) std::memcpy(out + pos, rdata, rdlen);
    pos += rdlen;
    finalizeRr(out, c, pos);
    return true;
}
} // namespace

size_t encodeProbeQuery(uint8_t* out, size_t outCap, const ResponseInputs& inputs) {
    // RFC 6762 §8.1: a probe is a query with one ANY question for the claimed
    // name plus the proposed unique records in the authority section (for the
    // §8.2 tiebreak). Proposed set: SRV + TXT, +A when an IPv4 is known.
    if (out == nullptr || outCap < 12) return 0;
    const std::string serviceType = SERVICE_TYPE_DOMAIN;
    const std::string instanceFqdn = inputs.instanceName + "." + serviceType;
    const std::string hostFqdn = inputs.hostName + ".local.";

    const std::vector<ProbeRecord> proposed = buildProposedRecords(inputs);
    if (proposed.empty()) return 0; // SRV + TXT are always present; defensive

    size_t pos = 0;
    putBE16(out + pos, 0); // transaction ID: 0 for mDNS
    pos += 2;
    putBE16(out + pos, 0); // flags: a plain query (QR=0)
    pos += 2;
    putBE16(out + pos, 1); // QDCOUNT: the single ANY question
    pos += 2;
    putBE16(out + pos, 0); // ANCOUNT
    pos += 2;
    putBE16(out + pos, static_cast<uint16_t>(proposed.size())); // NSCOUNT: proposed records
    pos += 2;
    putBE16(out + pos, 0); // ARCOUNT
    pos += 2;

    // Question: instance FQDN, type ANY, QU bit set (CACHE_FLUSH_BIT in a
    // question's class); §8.1 probe questions SHOULD be QU.
    const size_t qn = writeDnsName(out + pos, outCap - pos, instanceFqdn);
    if (qn == 0) return 0;
    pos += qn;
    if (pos + 4 > outCap) return 0;
    putBE16(out + pos, TYPE_ANY);                       // everything held under this name
    putBE16(out + pos + 2, CLASS_IN | CACHE_FLUSH_BIT); // QU bit set: unicast response
    pos += 4;

    // Authority: proposed records, class IN with cache-flush bit CLEAR; that bit
    // is a response-side directive, meaningless in a query's authority.
    for (const auto& r : proposed) {
        if (!writeAuthorityRecord(out, outCap, pos, r.name, r.type, CLASS_IN, r.ttl, r.rdata.data(),
                                  r.rdata.size()))
            return 0;
    }
    return pos;
}

std::vector<ProbeRecord> buildProposedRecords(const ResponseInputs& inputs) {
    // Unique records claimed for the instance: SRV + TXT for the instance FQDN,
    // +A when an IPv4 is known. Shared PTR excluded (§8.1 probes only records
    // desired unique). rdata canonical (names uncompressed) for compareRecordSets.
    const std::string serviceType = SERVICE_TYPE_DOMAIN;
    const std::string instanceFqdn = inputs.instanceName + "." + serviceType;
    const std::string hostFqdn = inputs.hostName + ".local.";

    std::vector<ProbeRecord> recs;

    // SRV: priority(2) weight(2) port(2) + uncompressed target name.
    {
        ProbeRecord srv;
        srv.name = instanceFqdn;
        srv.type = TYPE_SRV;
        srv.cls = CLASS_IN;
        srv.ttl = TTL_HOST;
        uint8_t fixed[6];
        putBE16(fixed + 0, inputs.priority);
        putBE16(fixed + 2, inputs.weight);
        putBE16(fixed + 4, inputs.udpPort);
        srv.rdata.insert(srv.rdata.end(), fixed, fixed + 6);
        uint8_t nameBuf[256];
        const size_t n = writeDnsName(nameBuf, sizeof(nameBuf), hostFqdn);
        if (n > 0) srv.rdata.insert(srv.rdata.end(), nameBuf, nameBuf + n);
        recs.push_back(std::move(srv));
    }
    // TXT: identical rdata to encodeResponse so probe and announcement propose
    // the same bytes (empty TXT → one 0 byte).
    {
        ProbeRecord txt;
        txt.name = instanceFqdn;
        txt.type = TYPE_TXT;
        txt.cls = CLASS_IN;
        txt.ttl = TTL_HOST;
        if (inputs.txtPairs.empty()) {
            txt.rdata.push_back(0);
        } else {
            for (const auto& [k, v] : inputs.txtPairs) {
                std::string entry = k + "=" + v;
                if (entry.size() > 255) continue;
                txt.rdata.push_back(static_cast<uint8_t>(entry.size()));
                txt.rdata.insert(txt.rdata.end(), entry.begin(), entry.end());
            }
        }
        recs.push_back(std::move(txt));
    }
    // A: 4 raw address bytes, only when the host owns a usable IPv4.
    if (inputs.ipv4 != nullptr) {
        ProbeRecord a;
        a.name = hostFqdn;
        a.type = TYPE_A;
        a.cls = CLASS_IN;
        a.ttl = TTL_HOST;
        a.rdata.insert(a.rdata.end(), inputs.ipv4, inputs.ipv4 + 4);
        recs.push_back(std::move(a));
    }
    return recs;
}

namespace {

// RFC 6762 §8.2.1 single-record lexicographic compare (<0/0/>0). Class first
// (cache-flush bit assumed masked off), then type, then rdata bytes unsigned;
// on a length-mismatched prefix the record with data remaining sorts later.
int compareOneRecord(const ProbeRecord& a, const ProbeRecord& b) {
    if (a.cls != b.cls) return a.cls < b.cls ? -1 : 1;
    if (a.type != b.type) return a.type < b.type ? -1 : 1;
    const size_t common = std::min(a.rdata.size(), b.rdata.size());
    for (size_t i = 0; i < common; ++i) {
        if (a.rdata[i] != b.rdata[i]) return a.rdata[i] < b.rdata[i] ? -1 : 1;
    }
    if (a.rdata.size() != b.rdata.size()) return a.rdata.size() < b.rdata.size() ? -1 : 1;
    return 0;
}

} // namespace

int compareRecordSets(std::vector<ProbeRecord> ours, std::vector<ProbeRecord> theirs) {
    // RFC 6762 §8.2.1: sort both sets into the same order, compare pairwise.
    auto less = [](const ProbeRecord& x, const ProbeRecord& y) {
        return compareOneRecord(x, y) < 0;
    };
    std::sort(ours.begin(), ours.end(), less);
    std::sort(theirs.begin(), theirs.end(), less);

    const size_t n = std::min(ours.size(), theirs.size());
    for (size_t i = 0; i < n; ++i) {
        const int c = compareOneRecord(ours[i], theirs[i]);
        if (c != 0) return c;
    }
    // §8.2.1: if a list runs out first, the side with records remaining wins;
    // if both run out, no conflict.
    if (ours.size() != theirs.size()) return ours.size() < theirs.size() ? -1 : 1;
    return 0;
}

bool authorityHasRecordFor(const std::vector<ProbeRecord>& authority, const std::string& name) {
    auto nameEqual = [](const std::string& x, const std::string& y) {
        if (x.size() != y.size()) return false;
        for (size_t i = 0; i < x.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(x[i])) !=
                std::tolower(static_cast<unsigned char>(y[i])))
                return false;
        }
        return true;
    };
    for (const auto& r : authority) {
        if (nameEqual(r.name, name)) return true;
    }
    return false;
}

} // namespace mdns
