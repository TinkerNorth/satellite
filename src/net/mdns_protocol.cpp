// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

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

// ── DNS name encoding / decoding ────────────────────────────────────────────

size_t writeDnsName(uint8_t* out, size_t outCap, const std::string& dottedName) {
    if (out == nullptr) return 0;
    size_t pos = 0;
    size_t i = 0;
    while (i < dottedName.size()) {
        // Find next dot (or end of string).
        size_t j = dottedName.find('.', i);
        if (j == std::string::npos) j = dottedName.size();
        const size_t labelLen = j - i;
        if (labelLen > 63) return 0; // RFC 1035 hard limit
        if (labelLen == 0) break;    // trailing dot — write terminator and stop
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
    size_t consumedBeforeJump = 0; // bytes consumed at the call site (before any compression jump)
    bool jumped = false;
    int safety = 64; // labels per name — hard cap so a malformed compression loop can't hang us

    while (safety-- > 0) {
        if (pos >= packetLen) return 0;
        const uint8_t b = packet[pos];
        if (b == 0) {
            // Terminator — finalise consumption: if we never jumped, we
            // consumed (pos - offset + 1); after a jump only the bytes up
            // to the first jump count. The trailing '.' is already in
            // outName from the previous label's append (or empty for a
            // zero-label name).
            if (!jumped)
                consumedBeforeJump = pos - offset + 1;
            else if (consumedBeforeJump == 0)
                return 0;
            return consumedBeforeJump;
        }
        if ((b & 0xC0) == 0xC0) {
            // Compression pointer — 2 bytes total, target is the low 14 bits.
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
        // Normal length-prefixed label.
        const size_t labelLen = b;
        if (pos + 1 + labelLen > packetLen) return 0;
        outName.append(reinterpret_cast<const char*>(packet + pos + 1), labelLen);
        outName.push_back('.');
        pos += 1 + labelLen;
    }
    return 0;
}

// ── Packet parsing ──────────────────────────────────────────────────────────

namespace {

// Parse the 12-byte header into `header` and the QDCOUNT questions into
// `questions`, advancing `pos` to the first byte after the question section.
// Returns true on success. Shared by both parsePacket overloads.
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
    // 4-arg overload: header + questions only. The answer / authority /
    // additional sections are deliberately not walked — callers that need
    // the querier's Known-Answer list use the 5-arg overload. A non-zero
    // ANCOUNT with no records actually present is therefore tolerated here.
    size_t pos = 0;
    return parseHeaderAndQuestions(data, len, header, questions, pos);
}

bool parsePacket(const uint8_t* data, size_t len, Header& header,
                 std::vector<Question>& questions, std::vector<Answer>& knownAnswers) {
    size_t pos = 0;
    knownAnswers.clear();
    if (!parseHeaderAndQuestions(data, len, header, questions, pos)) return false;

    // Answer section — the querier's Known-Answer list (RFC 6762 §7.1). Each
    // record is name + type(2) + class(2) + ttl(4) + rdlength(2) + rdata.
    // We retain name/type/class/ttl and skip the rdata bytes wholesale. A
    // malformed record (name decode failure, or an rdlength that overruns
    // the packet) fails the parse so a bad packet can't be acted on.
    for (uint16_t i = 0; i < header.anCount; ++i) {
        Answer a;
        const size_t consumed = readDnsName(data, len, pos, a.name);
        if (consumed == 0) return false;
        pos += consumed;
        if (pos + 10 > len) return false;
        a.type = readBE16(data + pos);
        a.cls = readBE16(data + pos + 2) & ~CACHE_FLUSH_BIT;
        a.ttl = (static_cast<uint32_t>(data[pos + 4]) << 24) |
                (static_cast<uint32_t>(data[pos + 5]) << 16) |
                (static_cast<uint32_t>(data[pos + 6]) << 8) |
                static_cast<uint32_t>(data[pos + 7]);
        const uint16_t rdlen = readBE16(data + pos + 8);
        pos += 10;
        if (pos + rdlen > len) return false; // rdata overruns the packet
        pos += rdlen;                        // skip rdata — identity is name+type
        knownAnswers.push_back(std::move(a));
    }
    return true;
}

bool isKnownAnswerSuppressed(const std::vector<Answer>& knownAnswers, const std::string& recordName,
                             uint16_t recordType, uint32_t ourTtl) {
    // Case-insensitive name compare — DNS labels are case-insensitive
    // (RFC 1035 §2.3.3); Bonjour/Avahi may capitalise differently.
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
        // RFC 6762 §7.1: suppress only if the querier's copy is at least
        // half as fresh as ours. A querier reporting a near-expired TTL
        // still wants the record refreshed. Compare without overflow.
        if (a.ttl >= ourTtl / 2) return true;
    }
    return false;
}

bool questionMatchesService(const Question& question) {
    if (question.type != TYPE_PTR && question.type != TYPE_ANY) return false;
    // Case-insensitive compare against SERVICE_TYPE_DOMAIN — DNS labels are
    // case-insensitive, and Bonjour/Avahi sometimes capitalise differently.
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

// ── Response encoding ───────────────────────────────────────────────────────

namespace {

// Write one RR header (name + type + class + ttl + rdlength placeholder)
// and return the offset of the rdlength field. The caller fills in
// rdlength after writing the rdata.
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

    // Which records actually go on the wire. The PTR/SRV/TXT are normally
    // all present; any may be dropped by RFC 6762 §7.1 Known-Answer
    // suppression. The A record additionally requires an IPv4 address.
    const bool emitPtr = !inputs.suppressPtr;
    const bool emitSrv = !inputs.suppressSrv;
    const bool emitTxt = !inputs.suppressTxt;
    const bool emitA = (inputs.ipv4 != nullptr) && !inputs.suppressA;
    const uint16_t answerCount = static_cast<uint16_t>((emitPtr ? 1 : 0) + (emitSrv ? 1 : 0) +
                                                       (emitTxt ? 1 : 0) + (emitA ? 1 : 0));
    // Every record suppressed → there is nothing to say; signal "send
    // nothing" so the responder stays silent (RFC 6762 §7.1).
    if (answerCount == 0) return 0;

    // Header.
    size_t pos = 0;
    putBE16(out + pos, txId);
    pos += 2;
    // mDNS responses always carry QR=1 + AA=1 (RFC 6762 §18.2 / §18.4); no
    // recursion bits. Unicast vs multicast changes only the destination —
    // the responder's concern — never these bytes.
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

    // PTR record: serviceType → instanceFqdn.
    if (emitPtr) {
        auto c = writeRrHeader(out, outCap, pos, serviceType, TYPE_PTR, CLASS_IN, ttlService);
        if (!c.ok) return 0;
        const size_t n = writeDnsName(out + pos, outCap - pos, instanceFqdn);
        if (n == 0) return 0;
        pos += n;
        finalizeRr(out, c, pos);
    }
    // SRV record: instanceFqdn → priority/weight/port/hostFqdn.
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
    // TXT record: one length-prefixed "key=value" per entry. Empty TXT is
    // encoded as a single zero-length string per DNS-SD §6.1.
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
    // Optional A record for the host.
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
    // RFC 6762 §8.3 — an announcement is wire-identical to a query response:
    // QR=1, AA=1, the full PTR+SRV+TXT(+A) answer set with the cache-flush
    // bit set on the unique SRV/TXT/A and clear on the shared PTR. It simply
    // answers no specific query, so the transaction ID is 0 and no record is
    // suppressed. Reuse encodeResponse so the two paths can never diverge.
    ResponseInputs ann = inputs;
    ann.goodbye = false;
    ann.suppressPtr = false;
    ann.suppressSrv = false;
    ann.suppressTxt = false;
    ann.suppressA = false;
    return encodeResponse(out, outCap, /*txId=*/0, ann);
}

size_t encodeProbeQuery(uint8_t* out, size_t outCap, const std::string& instanceName) {
    // RFC 6762 §8.1 — a probe is an ordinary query carrying a single ANY
    // question for the name we intend to claim. It is multicast, so the QU
    // (unicast-response) bit stays clear. We send the minimal form: no
    // proposed records in the authority section (the §8.1 simultaneous-probe
    // tie-break is deliberately out of scope — see mdns_responder.cpp).
    if (out == nullptr || outCap < 12) return 0;
    const std::string instanceFqdn = instanceName + "." + std::string(SERVICE_TYPE_DOMAIN);

    size_t pos = 0;
    putBE16(out + pos, 0); // transaction ID — 0 for mDNS
    pos += 2;
    putBE16(out + pos, 0); // flags — a plain query (QR=0)
    pos += 2;
    putBE16(out + pos, 1); // QDCOUNT
    pos += 2;
    putBE16(out + pos, 0); // ANCOUNT
    pos += 2;
    putBE16(out + pos, 0); // NSCOUNT
    pos += 2;
    putBE16(out + pos, 0); // ARCOUNT
    pos += 2;

    const size_t n = writeDnsName(out + pos, outCap - pos, instanceFqdn);
    if (n == 0) return 0;
    pos += n;
    if (pos + 4 > outCap) return 0;
    putBE16(out + pos, TYPE_ANY);     // ask for everything held under this name
    putBE16(out + pos + 2, CLASS_IN); // QU bit clear → multicast response
    pos += 4;
    return pos;
}

} // namespace mdns
