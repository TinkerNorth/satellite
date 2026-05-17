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

#include <chrono>
#include <cctype>
#include <cstring>
#include <deque>
#include <random>
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

// Sleep `ms` milliseconds in <=100 ms slices, bailing the moment g_appRunning
// clears so a shutdown during the (deliberately long) probe sequence is
// noticed promptly. Returns false if g_appRunning cleared while waiting.
bool interruptibleSleep(unsigned ms) {
    unsigned remaining = ms;
    while (remaining > 0 && g_appRunning) {
        const unsigned slice = remaining < 100 ? remaining : 100;
        netSleepMs(slice);
        remaining -= slice;
    }
    return g_appRunning.load();
}

// Case-insensitive DNS-name compare (RFC 1035 §2.3.3) — Bonjour/Avahi may
// capitalise labels differently from us.
bool nameEqCi(const std::string& x, const std::string& y) {
    if (x.size() != y.size()) return false;
    for (size_t i = 0; i < x.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(x[i])) !=
            std::tolower(static_cast<unsigned char>(y[i])))
            return false;
    }
    return true;
}

// RFC 6762 §9 name-conflict disambiguation. Given the current instance label,
// return the next candidate by appending " (2)" or incrementing an existing
// " (N)" suffix: "kitchen" → "kitchen (2)" → "kitchen (3)" → …
std::string nextInstanceLabel(const std::string& label) {
    // Detect a trailing " (N)" where N is one or more digits.
    if (!label.empty() && label.back() == ')') {
        const size_t open = label.rfind(" (");
        if (open != std::string::npos && open + 2 < label.size() - 1) {
            const size_t digitsStart = open + 2;
            const size_t digitsEnd = label.size() - 1; // index of ')'
            bool allDigits = true;
            for (size_t i = digitsStart; i < digitsEnd; ++i) {
                if (!std::isdigit(static_cast<unsigned char>(label[i]))) {
                    allDigits = false;
                    break;
                }
            }
            if (allDigits && digitsEnd > digitsStart) {
                long n = std::strtol(label.substr(digitsStart, digitsEnd - digitsStart).c_str(),
                                     nullptr, 10);
                return label.substr(0, open) + " (" + std::to_string(n + 1) + ")";
            }
        }
    }
    return label + " (2)";
}

// Outcome of one inbound packet examined during our probing window.
enum class ProbeEvent {
    None,        // packet irrelevant to our probe (or no packet)
    Conflict,    // a peer RESPONSE answered our claimed name → §9 conflict
    LostTiebreak // a peer PROBE for our name and §8.2 says the peer wins
};

// Examine one received mDNS packet during probing for our instance/host
// names. `instanceFqdn` / `hostFqdn` are the names we are currently claiming;
// `ours` is our proposed record set for the §8.2 tiebreak. Implements:
//   - RFC 6762 §9: a RESPONSE (QR=1) carrying an Answer record for one of our
//     claimed names is an outright conflict.
//   - RFC 6762 §8.2: a PROBE (QR=0 query with our name in the Question
//     section AND records in the Authority section) is *not* an outright
//     conflict — run the §8.2.1 tiebreak. We win → ignore (None); we lose →
//     LostTiebreak; identical record sets → no conflict (None).
ProbeEvent classifyProbePacket(const uint8_t* buf, size_t n, const std::string& instanceFqdn,
                               const std::string& hostFqdn,
                               const std::vector<mdns::ProbeRecord>& ours) {
    mdns::Header hdr;
    std::vector<mdns::Question> qs;
    std::vector<mdns::Answer> ans;
    std::vector<mdns::ProbeRecord> authority;
    if (!mdns::parsePacket(buf, n, hdr, qs, ans, authority)) return ProbeEvent::None;

    constexpr uint16_t QR = 0x8000;
    const bool isResponse = (hdr.flags & QR) != 0;

    if (isResponse) {
        // §9 conflict: any Answer record for a name we are claiming. Our own
        // probe loops back via IP_MULTICAST_LOOP, but a probe is a query with
        // no answers, so any answer here came from a peer.
        for (const auto& a : ans) {
            const bool hitInstance = nameEqCi(a.name, instanceFqdn) &&
                                     (a.type == mdns::TYPE_SRV || a.type == mdns::TYPE_TXT ||
                                      a.type == mdns::TYPE_ANY);
            const bool hitHost = nameEqCi(a.name, hostFqdn) && a.type == mdns::TYPE_A;
            if (hitInstance || hitHost) return ProbeEvent::Conflict;
        }
        return ProbeEvent::None;
    }

    // A query. It is a *probe* for our name iff it questions our name AND
    // carries authority records — RFC 6762 §8.2's "another host issue a query
    // for the same record [with] the Authority Section [populated]". A bare
    // query with no authority section is just an ordinary lookup; it cannot
    // conflict with a name we do not yet own, so we ignore it while probing.
    bool questionsOurName = false;
    for (const auto& q : qs) {
        if (nameEqCi(q.name, instanceFqdn) || nameEqCi(q.name, hostFqdn)) {
            questionsOurName = true;
            break;
        }
    }
    if (!questionsOurName || authority.empty()) return ProbeEvent::None;

    // The peer is simultaneously probing one of our names. RFC 6762 §8.2
    // compares "the data of [the peer's] resource record(s) … with its own
    // tentative data" for the *same* record(s). So restrict BOTH sides to a
    // single contested name and compare like-for-like — otherwise the §8.2.1
    // "list runs out" rule would wrongly let one host win merely because the
    // peer probed fewer of our names than we propose. The instance name
    // (SRV+TXT) and the host name (A) are independent records, so a peer
    // probe touching either contests that name's set on its own.
    auto sideFor = [](const std::vector<mdns::ProbeRecord>& recs, const std::string& fqdn) {
        std::vector<mdns::ProbeRecord> sub;
        for (const auto& r : recs)
            if (nameEqCi(r.name, fqdn)) sub.push_back(r);
        return sub;
    };
    for (const std::string& contested : {instanceFqdn, hostFqdn}) {
        const std::vector<mdns::ProbeRecord> theirs = sideFor(authority, contested);
        if (theirs.empty()) continue; // peer is not probing this name
        const std::vector<mdns::ProbeRecord> oursForName = sideFor(ours, contested);
        const int cmp = mdns::compareRecordSets(oursForName, theirs);
        // cmp > 0 → our data is lexicographically later → we win this name.
        // cmp == 0 → identical record sets → §8.2.1 says no conflict.
        // cmp < 0 → our data is earlier → we lose → defer (§8.2). Losing any
        //           contested name forces a deferral; report it immediately.
        if (cmp < 0) return ProbeEvent::LostTiebreak;
    }
    return ProbeEvent::None;
}

// Result of the RFC 6762 §8.1 probe sequence.
struct ProbeResult {
    std::string instance; // final (possibly renamed) instance label to advertise
    bool shutdown = false; // g_appRunning cleared mid-probe — caller must not announce
    bool gaveUp = false;   // §9 rename cap hit — advertising unprobed, conflicts possible
};

// Run the full RFC 6762 §8.1 + §8.2 probe sequence for `<host>` on `sock`,
// returning the instance label the caller should advertise. Blocks for the
// sequence duration (~750 ms minimum, longer on conflict / rate limiting),
// in short slices so a shutdown is noticed promptly.
ProbeResult runProbeSequence(SOCKET sock, const sockaddr_in& groupAddr, const std::string& host,
                             const uint8_t* selfIp, bool haveSelfIp) {
    ProbeResult result;
    result.instance = host;

    constexpr int kProbeCount = 3;         // §8.1: three probe queries
    constexpr unsigned kProbeGapMs = 250;  // §8.1: 250 ms between probes
    constexpr int kMaxRenameAttempts = 10; // sanity cap on §9 renames
    // §8.1 rate limiting: 15 conflicts within any 10 s window → wait >=5 s
    // before each subsequent probe attempt.
    constexpr size_t kRateLimitConflicts = 15;
    constexpr auto kRateLimitWindow = std::chrono::seconds(10);
    constexpr unsigned kRateLimitDelayMs = 5000;

    // Timestamps of conflicts (a §9 conflict or a lost §8.2 tiebreak) for the
    // rate-limit window.
    std::deque<std::chrono::steady_clock::time_point> conflictTimes;

    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<unsigned> startupDelayMs(0, 250);

    // §8.1: "wait for a short random delay … uniformly distributed in the
    // range 0-250 ms" — desynchronises hosts that power up together.
    if (!interruptibleSleep(startupDelayMs(rng))) {
        result.shutdown = true;
        return result;
    }

    for (int attempt = 0; attempt < kMaxRenameAttempts; ++attempt) {
        if (!g_appRunning) {
            result.shutdown = true;
            return result;
        }
        const std::string instanceFqdn =
            result.instance + "." + std::string(mdns::SERVICE_TYPE_DOMAIN);
        const std::string hostFqdn = host + ".local.";

        // Our proposed record set for this name — carried in every probe and
        // used as "our side" of the §8.2 tiebreak.
        mdns::ResponseInputs probeIn;
        fillServiceInputs(probeIn, result.instance, host, haveSelfIp ? selfIp : nullptr);
        const std::vector<mdns::ProbeRecord> ours = mdns::buildProposedRecords(probeIn);

        uint8_t probeBuf[768];
        const size_t probeLen = mdns::encodeProbeQuery(probeBuf, sizeof(probeBuf), probeIn);
        if (probeLen == 0) {
            // Encoder failure (e.g. an absurd hostname) — skip probing rather
            // than spin; the announcement still advertises us.
            logMsg(LogLevel::WARN, "mdns",
                   "mDNS probe encoding failed — skipping probe, announcing unprobed");
            return result;
        }

        bool conflict = false;     // §9 conflict seen this attempt
        bool lostTiebreak = false; // §8.2 tiebreak lost this attempt

        for (int probe = 0; probe < kProbeCount && g_appRunning && !conflict && !lostTiebreak;
             ++probe) {
            // §8.1 rate limit: once 15 conflicts have piled up inside the
            // trailing 10 s window, wait >=5 s before *each* probe send.
            while (!conflictTimes.empty() &&
                   std::chrono::steady_clock::now() - conflictTimes.front() > kRateLimitWindow)
                conflictTimes.pop_front();
            if (conflictTimes.size() >= kRateLimitConflicts) {
                logMsg(LogLevel::WARN, "mdns",
                       "mDNS probing rate-limited — 15+ conflicts in 10 s; "
                       "waiting 5 s before the next probe");
                if (!interruptibleSleep(kRateLimitDelayMs)) {
                    result.shutdown = true;
                    return result;
                }
            }

            sendto(sock, reinterpret_cast<const char*>(probeBuf), static_cast<int>(probeLen), 0,
                   reinterpret_cast<const sockaddr*>(&groupAddr), sizeof(groupAddr));

            // Listen 250 ms for a conflicting response or a peer probe.
            // recvfrom has a 200 ms timeout; loop on a steady-clock deadline
            // so every packet that arrives in-window is drained.
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(kProbeGapMs);
            while (g_appRunning && std::chrono::steady_clock::now() < deadline) {
                sockaddr_in pfrom{};
                socklen_t pfromLen = sizeof(pfrom);
                uint8_t pbuf[2048];
                const int pn = static_cast<int>(
                    recvfrom(sock, reinterpret_cast<char*>(pbuf), sizeof(pbuf), 0,
                             reinterpret_cast<sockaddr*>(&pfrom), &pfromLen));
                if (pn <= 0) continue; // timeout slice — re-check the deadline
                const ProbeEvent ev = classifyProbePacket(pbuf, static_cast<size_t>(pn),
                                                           instanceFqdn, hostFqdn, ours);
                if (ev == ProbeEvent::Conflict) {
                    conflict = true;
                    break;
                }
                if (ev == ProbeEvent::LostTiebreak) {
                    lostTiebreak = true;
                    break;
                }
            }
        }

        if (!g_appRunning) {
            result.shutdown = true;
            return result;
        }

        if (!conflict && !lostTiebreak) {
            // Three clean probes — the name is ours (§8.1).
            if (attempt > 0)
                logMsg(LogLevel::INFO, "mdns",
                       "mDNS probing succeeded — claimed '" + result.instance + "' after rename");
            return result;
        }

        // A §9 conflict or a lost §8.2 tiebreak — record it for the
        // rate-limit window.
        conflictTimes.push_back(std::chrono::steady_clock::now());

        if (lostTiebreak) {
            // §8.2: the loser "defers to the winning host by waiting one
            // second, and then begins probing for this record again" — same
            // name, sequence restarts at probe #1. A deferral is not a
            // rename, so it must not consume a rename attempt.
            logMsg(LogLevel::INFO, "mdns",
                   "mDNS simultaneous-probe tiebreak lost for '" + result.instance +
                       "' — deferring 1 s, then re-probing the same name");
            if (!interruptibleSleep(1000)) {
                result.shutdown = true;
                return result;
            }
            --attempt;
            continue;
        }

        // §9 conflict — another host owns the name. Pick the next candidate
        // and restart probing from probe #1.
        const std::string previous = result.instance;
        result.instance = nextInstanceLabel(result.instance);
        logMsg(LogLevel::WARN, "mdns",
               "mDNS name conflict — '" + previous +
                   "._satellite._udp.local.' already claimed on this segment; re-probing as '" +
                   result.instance + "'");
    }

    // Fell out of the rename loop without a clean claim — §9 rename cap hit.
    result.gaveUp = true;
    return result;
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

    // Resolve the LAN IPv4 once up front — it is the rdata of the proposed A
    // record carried in the §8.1 probe's authority section, and is reused by
    // the §8.3 startup announcement. (Steady-state query responses re-resolve
    // it per request, since the address can change while the responder runs.)
    // Best-effort: a host with no usable IPv4 still advertises PTR+SRV+TXT and
    // probes only SRV+TXT.
    uint8_t selfIp[4];
    const bool haveSelfIp = getLocalIPv4(selfIp);

    // ── Probing — RFC 6762 §8.1 + §8.2 ──────────────────────────────────────
    // Before announcing, claim the instance name via the full §8.1 probe
    // sequence (see runProbeSequence): a random 0-250 ms startup delay, then
    // three ANY probe queries 250 ms apart, each carrying our proposed unique
    // records in the authority section. A conflicting RESPONSE in a probe
    // window means another host owns the name → rename per §9 and restart. A
    // simultaneous PROBE from a peer is resolved by the §8.2 lexicographic
    // tiebreak rather than treated as an outright conflict. The sequence
    // legitimately adds ~750 ms+ to startup; runProbeSequence sleeps in short
    // slices so a shutdown is still noticed promptly.
    const ProbeResult probe = runProbeSequence(sock, groupAddr, host, selfIp, haveSelfIp);
    if (probe.shutdown) {
        // g_appRunning cleared mid-probe — unwind without announcing.
        g_mdnsResponderActive.store(false, std::memory_order_relaxed);
        setsockopt(sock, IPPROTO_IP, IP_DROP_MEMBERSHIP, reinterpret_cast<const char*>(&mreq),
                   sizeof(mreq));
        closesocket(sock);
        netShutdown();
        return;
    }
    // The final (possibly renamed) instance label — used by the announcement,
    // every steady-state query response, and the shutdown goodbye.
    const std::string instance = probe.instance;
    if (probe.gaveUp) {
        logMsg(LogLevel::ERR, "mdns",
               "mDNS probing gave up after 10 rename attempts — the segment is saturated "
               "with '_satellite._udp.local.' names; advertising anyway as '" + instance +
                   "' (cache conflicts possible)");
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

        // A query matches if it asks for the shared service type (PTR/ANY on
        // `_satellite._udp.local.`) OR — RFC 6762 §8.1 name defence — for one
        // of our own unique names: the instance FQDN or the host FQDN. The
        // latter is how we answer (and thus defend against) a peer that is
        // probing a name we already own. §8.1: "answering such probe queries
        // to defend a unique record is a high priority"; we reply with our
        // records, which asserts our ownership and makes the prober back off.
        const std::string serviceType = mdns::SERVICE_TYPE_DOMAIN;
        const std::string instanceFqdn = instance + "." + serviceType;
        const std::string hostFqdn = host + ".local.";

        bool matched = false;
        bool wantUnicast = false;
        for (const auto& q : questions) {
            // A probe carries qtype ANY; an ordinary lookup uses a specific
            // type. Either way, a query naming a record we own is one we must
            // answer — match ANY plus the specific type of each owned record.
            const bool forInstance =
                nameEqCi(q.name, instanceFqdn) &&
                (q.type == mdns::TYPE_ANY || q.type == mdns::TYPE_SRV || q.type == mdns::TYPE_TXT);
            const bool forHost = nameEqCi(q.name, hostFqdn) &&
                                 (q.type == mdns::TYPE_ANY || q.type == mdns::TYPE_A);
            if (mdns::questionMatchesService(q) || forInstance || forHost) {
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
