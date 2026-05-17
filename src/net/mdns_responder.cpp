// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * mdns_responder.cpp — Multicast-DNS responder thread.
 *
 * See mdns_responder.h. Socket lifecycle mirrors discovery.cpp; the
 * query/response wire work is delegated to mdns_protocol.h.
 */
#include "mdns_responder.h"
#include "config.h"

#include <cstring>
#include <string>
#include <vector>

namespace {

// Discover this host's primary LAN IPv4 by asking the routing table which
// source address it would use to reach a public host. The UDP `connect`
// sends nothing on the wire — it just fixes the socket's default peer so
// `getsockname` can report the chosen interface. Standard portable trick;
// works on Windows / macOS / Linux. `out` receives 4 bytes, network order.
bool getLocalIPv4(uint8_t out[4]) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;

    sockaddr_in remote{};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(53);
    inet_pton(AF_INET, "203.0.113.1", &remote.sin_addr); // TEST-NET-3, never routed

    bool ok = false;
    if (connect(s, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) == 0) {
        sockaddr_in local{};
        socklen_t len = sizeof(local);
        if (getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0) {
            std::memcpy(out, &local.sin_addr.s_addr, 4);
            ok = true;
        }
    }
    closesocket(s);
    return ok;
}

// Trim a trailing `.local` (with or without the final dot) off the host
// label so the SRV target / instance name don't end up doubly-suffixed.
std::string shortHostLabel() {
    char hostname[256] = {};
    if (!netGetHostname(hostname, sizeof(hostname)) || hostname[0] == '\0') { return "satellite"; }
    std::string h(hostname);
    // Some platforms return an FQDN; keep only the first label.
    auto dot = h.find('.');
    if (dot != std::string::npos) h = h.substr(0, dot);
    return h.empty() ? "satellite" : h;
}

// Populate the PTR/SRV/TXT(+A) inputs the encoders share. The instance and
// host labels can differ — probing (RFC 6762 §8.1) may disambiguate the
// instance label on a name clash while leaving the host label alone. `ipv4`
// (4 bytes, network order) is borrowed; it must outlive `out`.
void fillServiceInputs(mdns::ResponseInputs& out, const std::string& instanceLabel,
                       const std::string& hostLabel, const uint8_t* ipv4) {
    out.instanceName = instanceLabel;
    out.hostName = hostLabel;
    out.udpPort = static_cast<uint16_t>(g_config.udpPort);
    out.txtPairs = {
        {"udp", std::to_string(g_config.udpPort)},
        {"pair", std::to_string(g_config.pairPort)},
        {"http", std::to_string(g_config.webPort)},
    };
    if (ipv4 != nullptr) out.ipv4 = ipv4;
}

} // namespace

void mdnsResponderThread() {
    if (!netInit()) return;

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        netShutdown();
        return;
    }

    // Port 5353 is almost always already held by the OS responder
    // (mDNSResponder on macOS, avahi-daemon on Linux). SO_REUSEADDR +
    // SO_REUSEPORT let us co-bind so incoming multicast is delivered to
    // every listener — that's how multiple mDNS apps coexist.
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
               sizeof(reuse));
#ifdef SO_REUSEPORT
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char*>(&reuse),
               sizeof(reuse));
#endif

    sockaddr_in bindAddr{};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_addr.s_addr = INADDR_ANY;
    bindAddr.sin_port = htons(mdns::MULTICAST_PORT);
    if (bind(sock, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr)) == SOCKET_ERROR) {
        // Another responder holds the port without REUSEPORT — non-fatal,
        // the legacy broadcast beacon still advertises us.
        logMsg(LogLevel::WARN, "mdns",
               "Could not bind UDP 5353 — mDNS discovery disabled (broadcast still active)");
        closesocket(sock);
        netShutdown();
        return;
    }

    // Join the mDNS multicast group so the NIC delivers 224.0.0.251 traffic.
    ip_mreq mreq{};
    inet_pton(AF_INET, mdns::MULTICAST_GROUP_V4, &mreq.imr_multiaddr);
    mreq.imr_interface.s_addr = INADDR_ANY;
    if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&mreq),
                   sizeof(mreq)) == SOCKET_ERROR) {
        logMsg(LogLevel::WARN, "mdns", "Could not join 224.0.0.251 — mDNS discovery disabled");
        closesocket(sock);
        netShutdown();
        return;
    }

    // RFC 6762 §11: mDNS packets use IP TTL 255 so a misconfigured router
    // can't silently confine us; loop-back on so same-host clients see us.
    int ttl = 255;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl),
               sizeof(ttl));
    int loop = 1;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, reinterpret_cast<const char*>(&loop),
               sizeof(loop));

    // 200 ms recv timeout so the loop notices g_appRunning clearing promptly.
    netSetRecvTimeoutMs(sock, 200);
    netDisableUdpConnReset(sock);

    sockaddr_in groupAddr{};
    groupAddr.sin_family = AF_INET;
    groupAddr.sin_port = htons(mdns::MULTICAST_PORT);
    inet_pton(AF_INET, mdns::MULTICAST_GROUP_V4, &groupAddr.sin_addr);

    const std::string host = shortHostLabel();

    // Resolve the LAN IPv4 once up front — reused by the probe-conflict
    // check, the startup announcement, and steady-state query responses.
    // Best-effort: a host with no usable IPv4 still advertises PTR+SRV+TXT.
    uint8_t selfIp[4];
    const bool haveSelfIp = getLocalIPv4(selfIp);

    // ── Lightweight probing — RFC 6762 §8.1 (best effort) ───────────────────
    // Before announcing, multicast a single ANY probe for our intended
    // instance name and listen briefly. If another responder answers for the
    // same name with a record that isn't ours (different A address, or any
    // SRV/TXT when we have no IP to compare), we have a name clash: log a
    // WARNING and disambiguate by suffixing the instance label. This is a
    // deliberately minimal probe — one query, no §8.1 authority-section
    // tie-break, no three-probe sequence — enough to avoid the common
    // two-satellites-same-hostname collision without large risk.
    std::string instance = host;
    {
        uint8_t probeBuf[512];
        const size_t probeLen = mdns::encodeProbeQuery(probeBuf, sizeof(probeBuf), instance);
        if (probeLen > 0) {
            sendto(sock, reinterpret_cast<const char*>(probeBuf), static_cast<int>(probeLen), 0,
                   reinterpret_cast<const sockaddr*>(&groupAddr), sizeof(groupAddr));

            // Listen ~250 ms for a conflicting answer. recvfrom already has a
            // 200 ms timeout, so at most a couple of iterations.
            const std::string instanceFqdn =
                instance + "." + std::string(mdns::SERVICE_TYPE_DOMAIN);
            const std::string hostFqdn = host + ".local.";
            bool conflict = false;
            for (int probeIter = 0; probeIter < 2 && !conflict && g_appRunning; ++probeIter) {
                sockaddr_in pfrom{};
                socklen_t pfromLen = sizeof(pfrom);
                uint8_t pbuf[2048];
                const int pn = static_cast<int>(
                    recvfrom(sock, reinterpret_cast<char*>(pbuf), sizeof(pbuf), 0,
                             reinterpret_cast<sockaddr*>(&pfrom), &pfromLen));
                if (pn <= 0) continue;

                mdns::Header phdr;
                std::vector<mdns::Question> pqs;
                std::vector<mdns::Answer> pans;
                if (!mdns::parsePacket(pbuf, static_cast<size_t>(pn), phdr, pqs, pans)) continue;
                constexpr uint16_t QR = 0x8000;
                if ((phdr.flags & QR) == 0) continue; // only responses can conflict

                // Loopback delivers our own probe back, but a probe has no
                // answers — any answer for our name therefore came from a
                // peer. Match on the instance FQDN (SRV/TXT) or host FQDN (A).
                for (const auto& a : pans) {
                    auto eqCi = [](const std::string& x, const std::string& y) {
                        if (x.size() != y.size()) return false;
                        for (size_t i = 0; i < x.size(); ++i)
                            if (std::tolower(static_cast<unsigned char>(x[i])) !=
                                std::tolower(static_cast<unsigned char>(y[i])))
                                return false;
                        return true;
                    };
                    const bool nameClash = (eqCi(a.name, instanceFqdn) &&
                                            (a.type == mdns::TYPE_SRV || a.type == mdns::TYPE_TXT ||
                                             a.type == mdns::TYPE_ANY)) ||
                                           (eqCi(a.name, hostFqdn) && a.type == mdns::TYPE_A);
                    if (nameClash) {
                        conflict = true;
                        break;
                    }
                }
            }
            if (conflict) {
                // Disambiguate per RFC 6762 §9 convention: append " (2)".
                // Single shot — if even this clashes we proceed anyway rather
                // than loop a full re-probe; logging the warning is the point.
                const std::string original = instance;
                instance = instance + " (2)";
                logMsg(LogLevel::WARN, "mdns",
                       "mDNS name conflict — '" + original +
                           "._satellite._udp.local.' already claimed on this segment; "
                           "advertising as '" + instance + "' instead");
            }
        }
    }

    logMsg(LogLevel::INFO, "mdns",
           "mDNS responder up — advertising _satellite._udp.local. as '" + instance + "'");
    g_mdnsResponderActive.store(true, std::memory_order_relaxed);

    // ── Startup announcement — RFC 6762 §8.3 ────────────────────────────────
    // Multicast the full unsolicited answer set so senders already running
    // learn about us immediately instead of waiting to re-query. §8.3 calls
    // for two-to-eight announcements one second apart; we send three. The
    // record set / cache-flush bits are identical to a query response.
    {
        mdns::ResponseInputs ann;
        fillServiceInputs(ann, instance, host, haveSelfIp ? selfIp : nullptr);
        uint8_t annBuf[1024];
        const size_t annLen = mdns::encodeAnnouncement(annBuf, sizeof(annBuf), ann);
        if (annLen > 0) {
            constexpr int kAnnouncements = 3;
            for (int i = 0; i < kAnnouncements && g_appRunning; ++i) {
                sendto(sock, reinterpret_cast<const char*>(annBuf), static_cast<int>(annLen), 0,
                       reinterpret_cast<const sockaddr*>(&groupAddr), sizeof(groupAddr));
                // ~1 s between announcements (§8.3), in 100 ms slices so a
                // shutdown during startup is still noticed promptly. Skip the
                // wait after the final announcement.
                if (i + 1 < kAnnouncements) {
                    for (int s = 0; s < 10 && g_appRunning; ++s) netSleepMs(100);
                }
            }
        }
    }

    while (g_appRunning) {
        sockaddr_in from{};
        socklen_t fromLen = sizeof(from);
        uint8_t buf[2048];
        int n = static_cast<int>(recvfrom(sock, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                                          reinterpret_cast<sockaddr*>(&from), &fromLen));
        if (n <= 0) continue; // timeout or transient error

        mdns::Header header;
        std::vector<mdns::Question> questions;
        std::vector<mdns::Answer> knownAnswers;
        if (!mdns::parsePacket(buf, static_cast<size_t>(n), header, questions, knownAnswers))
            continue;

        // Ignore responses (QR bit set) — we only answer queries.
        constexpr uint16_t QR_BIT = 0x8000;
        if (header.flags & QR_BIT) continue;

        bool matched = false;
        bool wantUnicast = false;
        for (const auto& q : questions) {
            if (mdns::questionMatchesService(q)) {
                matched = true;
                wantUnicast = wantUnicast || q.unicastResponse;
            }
        }
        if (!matched) continue;

        // Build the response. The A record is best-effort — if we can't
        // resolve a LAN IP we still answer with PTR + SRV + TXT and a
        // proper mDNS client resolves `<host>.local.` itself.
        uint8_t ipv4[4];
        const bool haveIp = getLocalIPv4(ipv4);
        mdns::ResponseInputs in;
        fillServiceInputs(in, instance, host, haveIp ? ipv4 : nullptr);

        // Known-Answer Suppression — RFC 6762 §7.1. The querier may list
        // records it already holds in the packet's answer section; any of
        // ours it knows with a TTL ≥ ½ ours must be dropped from the reply.
        // The PTR ages at TTL_SERVICE; SRV/TXT/A age at TTL_HOST.
        const std::string serviceType = mdns::SERVICE_TYPE_DOMAIN;
        const std::string instanceFqdn = instance + "." + serviceType;
        const std::string hostFqdn = host + ".local.";
        in.suppressPtr = mdns::isKnownAnswerSuppressed(knownAnswers, serviceType, mdns::TYPE_PTR,
                                                       mdns::TTL_SERVICE);
        in.suppressSrv = mdns::isKnownAnswerSuppressed(knownAnswers, instanceFqdn, mdns::TYPE_SRV,
                                                       mdns::TTL_HOST);
        in.suppressTxt = mdns::isKnownAnswerSuppressed(knownAnswers, instanceFqdn, mdns::TYPE_TXT,
                                                       mdns::TTL_HOST);
        in.suppressA =
            mdns::isKnownAnswerSuppressed(knownAnswers, hostFqdn, mdns::TYPE_A, mdns::TTL_HOST);

        uint8_t out[1024];
        // encodeResponse returns 0 when every record was suppressed — that is
        // the §7.1 "send nothing" case, indistinguishable here from a buffer
        // failure; either way the correct action is to stay silent.
        size_t outLen = mdns::encodeResponse(out, sizeof(out), header.id, in);
        if (outLen == 0) continue;

        // Unicast back to the querier when it set the QU bit; otherwise
        // multicast to the group so every cache on the segment updates.
        const sockaddr_in& dest = wantUnicast ? from : groupAddr;
        sendto(sock, reinterpret_cast<const char*>(out), static_cast<int>(outLen), 0,
               reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
    }

    // RFC 6762 §10.1 — announce departure with a TTL-0 record set so caches
    // on the segment drop the service immediately instead of holding a dead
    // entry until the 120 s service TTL expires. Best-effort, single shot.
    g_mdnsResponderActive.store(false, std::memory_order_relaxed);
    {
        uint8_t byeIp[4];
        const bool haveByeIp = getLocalIPv4(byeIp);
        mdns::ResponseInputs bye;
        fillServiceInputs(bye, instance, host, haveByeIp ? byeIp : nullptr);
        bye.goodbye = true;
        uint8_t byeBuf[1024];
        const size_t byeLen = mdns::encodeResponse(byeBuf, sizeof(byeBuf), 0, bye);
        if (byeLen > 0) {
            sendto(sock, reinterpret_cast<const char*>(byeBuf), static_cast<int>(byeLen), 0,
                   reinterpret_cast<const sockaddr*>(&groupAddr), sizeof(groupAddr));
        }
    }

    setsockopt(sock, IPPROTO_IP, IP_DROP_MEMBERSHIP, reinterpret_cast<const char*>(&mreq),
               sizeof(mreq));
    closesocket(sock);
    netShutdown();
}
