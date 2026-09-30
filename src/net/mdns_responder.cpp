// SPDX-License-Identifier: LGPL-3.0-or-later

// See mdns_responder.h. Socket lifecycle mirrors discovery.cpp; wire work is
// delegated to mdns_protocol.h.
#include "mdns_responder.h"
#include "config.h"
#include "machine_id.h"
#include "net/local_iface.h"
#include "net/mdns_rejoin.h"

#include <chrono>
#include <cctype>
#include <cstring>
#include <deque>
#include <random>
#include <string>
#include <vector>

namespace {

std::atomic<bool> g_mdnsRejoinRequested{false};

// Primary LAN IPv4 via the routing table: a UDP `connect` sends nothing but
// fixes the socket's peer so `getsockname` reports the chosen interface.
// `out` = 4 bytes, network order.
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

bool boundIPv4(uint8_t out[4]) {
    std::string sel;
    {
        std::lock_guard<std::mutex> lk(g_configMtx);
        sel = g_config.networkInterface;
    }
    uint32_t ip = 0;
    if (resolveBoundIPv4(sel, ip)) {
        std::memcpy(out, &ip, 4);
        return true;
    }
    return getLocalIPv4(out);
}

// Host label with any trailing `.local` (and FQDN suffix) trimmed, so the SRV
// target / instance name aren't doubly-suffixed.
std::string shortHostLabel() {
    char hostname[256] = {};
    if (!netGetHostname(hostname, sizeof(hostname)) || hostname[0] == '\0') { return "satellite"; }
    std::string h(hostname);
    auto dot = h.find('.');
    if (dot != std::string::npos) h = h.substr(0, dot);
    return h.empty() ? "satellite" : h;
}

// Populate the shared encoder inputs. Instance and host labels can differ:
// §8.1 probing may rename the instance on a clash while leaving the host alone.
// `ipv4` (4 bytes, network order) is borrowed and must outlive `out`.
void fillServiceInputs(mdns::ResponseInputs& out, const std::string& instanceLabel,
                       const std::string& hostLabel, const uint8_t* ipv4) {
    out.instanceName = instanceLabel;
    out.hostName = hostLabel;
    out.udpPort = static_cast<uint16_t>(g_config.udpPort);
    // `pair`/`http` both carry the single HTTPS client API port. `mid` is the
    // stable per-install id so the dish dedupes us across DHCP address changes.
    out.txtPairs = {
        {"udp", std::to_string(g_config.udpPort)},
        {"pair", std::to_string(DEFAULT_CLIENT_PORT)},
        {"http", std::to_string(DEFAULT_CLIENT_PORT)},
        {"mid", ensureMachineId()},
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

using mdns::nameEqCi;

// RFC 6762 §9 name-conflict disambiguation. Returns the next candidate by
// appending " (2)" or incrementing an existing " (N)" suffix.
std::string nextInstanceLabel(const std::string& label) {
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

// The records of one name out of a probe's set, so §8.2 compares like for like.
std::vector<mdns::ProbeRecord> recordsForName(const std::vector<mdns::ProbeRecord>& recs,
                                              const std::string& fqdn) {
    std::vector<mdns::ProbeRecord> sub;
    for (const auto& r : recs) {
        if (nameEqCi(r.name, fqdn)) sub.push_back(r);
    }
    return sub;
}

// Classify one packet seen during our probing window. `ours` is our proposed
// set for the §8.2 tiebreak. §9: a RESPONSE answering a claimed name → Conflict.
// §8.2: a PROBE (query naming us + authority records) runs the §8.2.1 tiebreak
// → win/identical = None, lose = LostTiebreak.
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
        // §9 conflict: any Answer for a claimed name. Our own probe loops back
        // (IP_MULTICAST_LOOP) but is a query with no answers, so any answer
        // here came from a peer.
        for (const auto& a : ans) {
            const bool hitInstance =
                nameEqCi(a.name, instanceFqdn) &&
                (a.type == mdns::TYPE_SRV || a.type == mdns::TYPE_TXT || a.type == mdns::TYPE_ANY);
            const bool hitHost = nameEqCi(a.name, hostFqdn) && a.type == mdns::TYPE_A;
            if (hitInstance || hitHost) return ProbeEvent::Conflict;
        }
        return ProbeEvent::None;
    }

    // A query is a probe (§8.2) iff it questions our name AND carries authority
    // records. A bare query is an ordinary lookup: it can't conflict with a name
    // we don't yet own, so ignore it while probing.
    bool questionsOurName = false;
    for (const auto& q : qs) {
        if (nameEqCi(q.name, instanceFqdn) || nameEqCi(q.name, hostFqdn)) {
            questionsOurName = true;
            break;
        }
    }
    if (!questionsOurName || authority.empty()) return ProbeEvent::None;

    // §8.2 compares like-for-like records. Restrict BOTH sides to one contested
    // name, else the §8.2.1 "list runs out" rule would let a host win merely
    // because the peer probed fewer names. Instance (SRV+TXT) and host (A) are
    // independent, so a peer probe touching either contests that name alone.
    for (const std::string& contested : {instanceFqdn, hostFqdn}) {
        const std::vector<mdns::ProbeRecord> theirs = recordsForName(authority, contested);
        if (theirs.empty()) continue; // peer is not probing this name
        const std::vector<mdns::ProbeRecord> oursForName = recordsForName(ours, contested);
        const int cmp = mdns::compareRecordSets(oursForName, theirs);
        // cmp <0 → we lose → defer (§8.2); losing any name forces a deferral.
        // >0 win, ==0 identical (no conflict).
        if (cmp < 0) return ProbeEvent::LostTiebreak;
    }
    return ProbeEvent::None;
}

// Result of the RFC 6762 §8.1 probe sequence.
struct ProbeResult {
    std::string instance;  // final (possibly renamed) instance label to advertise
    bool shutdown = false; // g_appRunning cleared mid-probe; caller must not announce
    bool gaveUp = false;   // §9 rename cap hit; advertising unprobed, conflicts possible
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

    // §8.1: random 0-250 ms delay desynchronises hosts that power up together.
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

        // Our proposed record set for this name, carried in every probe and used
        // as "our side" of the §8.2 tiebreak.
        mdns::ResponseInputs probeIn;
        fillServiceInputs(probeIn, result.instance, host, haveSelfIp ? selfIp : nullptr);
        const std::vector<mdns::ProbeRecord> ours = mdns::buildProposedRecords(probeIn);

        uint8_t probeBuf[768];
        const size_t probeLen = mdns::encodeProbeQuery(probeBuf, sizeof(probeBuf), probeIn);
        if (probeLen == 0) {
            // Encoder failure: skip probing rather than spin; the announcement
            // still advertises us.
            logMsg(LogLevel::WARN, "mdns",
                   "mDNS probe encoding failed; skipping probe, announcing unprobed");
            return result;
        }

        bool conflict = false;     // §9 conflict seen this attempt
        bool lostTiebreak = false; // §8.2 tiebreak lost this attempt

        for (int probe = 0; probe < kProbeCount && g_appRunning && !conflict && !lostTiebreak;
             ++probe) {
            // §8.1 rate limit: 15 conflicts in the trailing 10 s window → wait
            // >=5 s before each probe send.
            while (!conflictTimes.empty() &&
                   std::chrono::steady_clock::now() - conflictTimes.front() > kRateLimitWindow)
                conflictTimes.pop_front();
            if (conflictTimes.size() >= kRateLimitConflicts) {
                logMsg(LogLevel::WARN, "mdns",
                       "mDNS probing rate-limited: 15+ conflicts in 10 s; "
                       "waiting 5 s before the next probe");
                if (!interruptibleSleep(kRateLimitDelayMs)) {
                    result.shutdown = true;
                    return result;
                }
            }

            sendto(sock, reinterpret_cast<const char*>(probeBuf), static_cast<int>(probeLen), 0,
                   reinterpret_cast<const sockaddr*>(&groupAddr), sizeof(groupAddr));

            // Listen 250 ms (recvfrom timeout 200 ms) on a steady-clock
            // deadline so every in-window packet is drained.
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(kProbeGapMs);
            while (g_appRunning && std::chrono::steady_clock::now() < deadline) {
                sockaddr_in pfrom{};
                socklen_t pfromLen = sizeof(pfrom);
                uint8_t pbuf[2048];
                const int pn =
                    static_cast<int>(recvfrom(sock, reinterpret_cast<char*>(pbuf), sizeof(pbuf), 0,
                                              reinterpret_cast<sockaddr*>(&pfrom), &pfromLen));
                if (pn <= 0) continue; // timeout slice; re-check the deadline
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
            // Three clean probes: the name is ours (§8.1).
            if (attempt > 0)
                logMsg(LogLevel::INFO, "mdns",
                       "mDNS probing succeeded; claimed '" + result.instance + "' after rename");
            return result;
        }

        // A §9 conflict or a lost §8.2 tiebreak: record it for the rate-limit
        // window.
        conflictTimes.push_back(std::chrono::steady_clock::now());

        if (lostTiebreak) {
            // §8.2: the loser defers 1 s then re-probes the same name from
            // probe #1. A deferral is not a rename, so it must not consume a
            // rename attempt.
            logMsg(LogLevel::INFO, "mdns",
                   "mDNS simultaneous-probe tiebreak lost for '" + result.instance +
                       "'; deferring 1 s, then re-probing the same name");
            if (!interruptibleSleep(1000)) {
                result.shutdown = true;
                return result;
            }
            --attempt;
            continue;
        }

        // §9 conflict: another host owns the name. Pick the next candidate and
        // restart probing from probe #1.
        const std::string previous = result.instance;
        result.instance = nextInstanceLabel(result.instance);
        logMsg(LogLevel::WARN, "mdns",
               "mDNS name conflict: '" + previous +
                   "._satellite._udp.local.' already claimed on this segment; re-probing as '" +
                   result.instance + "'");
    }

    // Fell out of the rename loop without a clean claim: §9 rename cap hit.
    result.gaveUp = true;
    return result;
}

} // namespace

void requestMdnsRejoin() { g_mdnsRejoinRequested.store(true, std::memory_order_relaxed); }

namespace {

// Everything the responder holds for the life of its socket.
struct Responder {
    SOCKET sock = INVALID_SOCKET;
    ip_mreq mreq{};
    in_addr ifAddr{};
    uint8_t selfIp[4] = {};
    bool haveSelfIp = false;
    sockaddr_in groupAddr{};
    std::string host;
    std::string instance;
};

constexpr int RECV_TIMEOUT_MS = 200;
constexpr int ANNOUNCEMENTS = 3;
constexpr auto REJOIN_INTERVAL = std::chrono::seconds(30);

// Port 5353 is usually already held by the OS responder (mDNSResponder /
// avahi-daemon). SO_REUSEADDR + SO_REUSEPORT let us co-bind so multicast is
// delivered to every listener, how mDNS apps coexist. INVALID_SOCKET when
// another responder holds the port without REUSEPORT; non-fatal.
SOCKET openResponderSocket() {
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) return INVALID_SOCKET;
    const int reuse = 1;
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
        logMsg(LogLevel::WARN, "mdns",
               "Could not bind UDP 5353; mDNS discovery disabled (broadcast still active)");
        closesocket(sock);
        return INVALID_SOCKET;
    }
    return sock;
}

// The interface the group is joined on: the bound address when one is known,
// else any.
void setInterfaceAddress(Responder& r, bool haveIp, const uint8_t ip[4]) {
    r.haveSelfIp = haveIp;
    if (haveIp) {
        std::memcpy(r.selfIp, ip, 4);
        std::memcpy(&r.ifAddr.s_addr, r.selfIp, 4);
    } else {
        r.ifAddr.s_addr = INADDR_ANY;
    }
}

// Join the mDNS multicast group so the NIC delivers 224.0.0.251 traffic, and
// send from the same interface. False when the join is refused.
bool joinMulticast(Responder& r) {
    inet_pton(AF_INET, mdns::MULTICAST_GROUP_V4, &r.mreq.imr_multiaddr);
    r.mreq.imr_interface = r.ifAddr;
    if (setsockopt(r.sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&r.mreq),
                   sizeof(r.mreq)) == SOCKET_ERROR) {
        return false;
    }
    setsockopt(r.sock, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&r.ifAddr),
               sizeof(r.ifAddr));
    return true;
}

void leaveMulticast(const Responder& r) {
    setsockopt(r.sock, IPPROTO_IP, IP_DROP_MEMBERSHIP, reinterpret_cast<const char*>(&r.mreq),
               sizeof(r.mreq));
}

// RFC 6762 §11: mDNS packets use IP TTL 255 so a misconfigured router can't
// silently confine us; loop-back on so same-host clients see us. The recv
// timeout is what lets the loop notice g_appRunning clearing promptly.
void tuneResponderSocket(SOCKET sock) {
    const int ttl = 255;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl),
               sizeof(ttl));
    const int loop = 1;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, reinterpret_cast<const char*>(&loop),
               sizeof(loop));
    netSetRecvTimeoutMs(sock, RECV_TIMEOUT_MS);
    netDisableUdpConnReset(sock);
}

sockaddr_in multicastGroupAddress() {
    sockaddr_in groupAddr{};
    groupAddr.sin_family = AF_INET;
    groupAddr.sin_port = htons(mdns::MULTICAST_PORT);
    inet_pton(AF_INET, mdns::MULTICAST_GROUP_V4, &groupAddr.sin_addr);
    return groupAddr;
}

void closeResponder(Responder& r) {
    leaveMulticast(r);
    closesocket(r.sock);
    netShutdown();
}

// Startup announcement (RFC 6762 §8.3): multicast the unsolicited answer set
// so running senders learn of us without re-querying. §8.3 wants 2-8
// announcements ~1 s apart; we send three.
void announceBurst(const Responder& r) {
    mdns::ResponseInputs ann;
    fillServiceInputs(ann, r.instance, r.host, r.haveSelfIp ? r.selfIp : nullptr);
    uint8_t annBuf[1024];
    const size_t annLen = mdns::encodeAnnouncement(annBuf, sizeof(annBuf), ann);
    if (annLen == 0) return;
    for (int i = 0; i < ANNOUNCEMENTS && g_appRunning; ++i) {
        sendto(r.sock, reinterpret_cast<const char*>(annBuf), static_cast<int>(annLen), 0,
               reinterpret_cast<const sockaddr*>(&r.groupAddr), sizeof(r.groupAddr));
        if (i + 1 < ANNOUNCEMENTS) {
            for (int s = 0; s < 10 && g_appRunning; ++s) netSleepMs(100);
        }
    }
}

// Re-join on a resume/network event, or when the bound address moved, then
// re-announce. `force` is the event path; otherwise only a moved address
// rejoins (mdns::evaluateRejoin).
void rejoinMulticast(Responder& r, bool force) {
    uint8_t newIp[4];
    const bool haveNew = boundIPv4(newIp);
    const mdns::RejoinDecision dec =
        mdns::evaluateRejoin(force, r.haveSelfIp, r.selfIp, haveNew, newIp);
    if (!dec.rejoin) return;
    leaveMulticast(r);
    setInterfaceAddress(r, haveNew, newIp);
    if (!joinMulticast(r)) {
        logMsg(LogLevel::WARN, "mdns",
               "Could not rejoin 224.0.0.251; mDNS answers may stop until the next network event");
    }
    logMsg(LogLevel::INFO, "mdns",
           dec.ipChanged ? "Bound address changed; rebound mDNS multicast and re-announcing"
                         : "Resume/network event; rebound mDNS multicast and re-announcing");
    announceBurst(r);
}

// One inbound packet: answered when it asks for something we own, unicast when
// it set the QU bit, and silently dropped when Known-Answer suppression (§7.1)
// leaves nothing to say.
void answerQuery(const Responder& r, const uint8_t* buf, size_t n, const sockaddr_in& from) {
    mdns::Header header;
    std::vector<mdns::Question> questions;
    std::vector<mdns::Answer> knownAnswers;
    if (!mdns::parsePacket(buf, n, header, questions, knownAnswers)) return;

    // Ignore responses (QR bit set); we only answer queries.
    constexpr uint16_t QR_BIT = 0x8000;
    if (header.flags & QR_BIT) return;

    const std::string serviceType = mdns::SERVICE_TYPE_DOMAIN;
    const std::string instanceFqdn = r.instance + "." + serviceType;
    const std::string hostFqdn = r.host + ".local.";
    const mdns::QueryMatch match = mdns::classifyQuestions(questions, instanceFqdn, hostFqdn);
    if (!match.matched) return;

    // A record is best-effort: with no LAN IP we still answer PTR+SRV+TXT
    // and a proper client resolves `<host>.local.` itself.
    uint8_t ipv4[4];
    const bool haveIp = boundIPv4(ipv4);
    mdns::ResponseInputs in;
    fillServiceInputs(in, r.instance, r.host, haveIp ? ipv4 : nullptr);

    // Known-Answer suppression (§7.1): drop any record the querier already
    // holds with TTL >= ½ ours. PTR ages at TTL_SERVICE; SRV/TXT/A at TTL_HOST.
    in.suppressPtr =
        mdns::isKnownAnswerSuppressed(knownAnswers, serviceType, mdns::TYPE_PTR, mdns::TTL_SERVICE);
    in.suppressSrv =
        mdns::isKnownAnswerSuppressed(knownAnswers, instanceFqdn, mdns::TYPE_SRV, mdns::TTL_HOST);
    in.suppressTxt =
        mdns::isKnownAnswerSuppressed(knownAnswers, instanceFqdn, mdns::TYPE_TXT, mdns::TTL_HOST);
    in.suppressA =
        mdns::isKnownAnswerSuppressed(knownAnswers, hostFqdn, mdns::TYPE_A, mdns::TTL_HOST);

    uint8_t out[1024];
    // 0 = every record suppressed (§7.1 "send nothing") or buffer too
    // small; either way, stay silent.
    const size_t outLen = mdns::encodeResponse(out, sizeof(out), header.id, in);
    if (outLen == 0) return;

    // Unicast on the QU bit; else multicast so every cache on the segment updates.
    const sockaddr_in& dest = match.wantUnicast ? from : r.groupAddr;
    sendto(r.sock, reinterpret_cast<const char*>(out), static_cast<int>(outLen), 0,
           reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
}

// RFC 6762 §10.1 goodbye: a TTL-0 record set so caches drop the service
// immediately instead of holding it until the TTL. Best-effort.
void sendGoodbye(const Responder& r) {
    uint8_t byeIp[4];
    const bool haveByeIp = boundIPv4(byeIp);
    mdns::ResponseInputs bye;
    fillServiceInputs(bye, r.instance, r.host, haveByeIp ? byeIp : nullptr);
    bye.goodbye = true;
    uint8_t byeBuf[1024];
    const size_t byeLen = mdns::encodeResponse(byeBuf, sizeof(byeBuf), 0, bye);
    if (byeLen == 0) return;
    sendto(r.sock, reinterpret_cast<const char*>(byeBuf), static_cast<int>(byeLen), 0,
           reinterpret_cast<const sockaddr*>(&r.groupAddr), sizeof(r.groupAddr));
}

} // namespace

// Long on purpose: the responder's whole lifecycle, bring-up to goodbye, in
// one flow; each step is named above, and the unwind paths read in order here.
void mdnsResponderThread() {
    if (!netInit()) return;

    Responder r;
    r.sock = openResponderSocket();
    if (r.sock == INVALID_SOCKET) {
        netShutdown();
        return;
    }
    uint8_t selfIp[4];
    const bool haveSelfIp = boundIPv4(selfIp);
    setInterfaceAddress(r, haveSelfIp, selfIp);
    if (!joinMulticast(r)) {
        logMsg(LogLevel::WARN, "mdns", "Could not join 224.0.0.251; mDNS discovery disabled");
        closesocket(r.sock);
        netShutdown();
        return;
    }
    tuneResponderSocket(r.sock);
    r.groupAddr = multicastGroupAddress();
    r.host = shortHostLabel();

    // Claim the instance name via the §8.1/§8.2 probe sequence before
    // announcing (see runProbeSequence; adds ~750 ms+ to startup).
    const ProbeResult probe = runProbeSequence(r.sock, r.groupAddr, r.host, r.selfIp, r.haveSelfIp);
    if (probe.shutdown) {
        // g_appRunning cleared mid-probe; unwind without announcing.
        g_mdnsResponderActive.store(false, std::memory_order_relaxed);
        closeResponder(r);
        return;
    }
    // Final (possibly renamed) instance label.
    r.instance = probe.instance;
    if (probe.gaveUp) {
        logMsg(LogLevel::ERR, "mdns",
               "mDNS probing gave up after 10 rename attempts; the segment is saturated "
               "with '_satellite._udp.local.' names; advertising anyway as '" +
                   r.instance + "' (cache conflicts possible)");
    }

    logMsg(LogLevel::INFO, "mdns",
           "mDNS responder up; advertising _satellite._udp.local. as '" + r.instance + "'");
    g_mdnsResponderActive.store(true, std::memory_order_relaxed);
    announceBurst(r);

    auto lastRejoinCheck = std::chrono::steady_clock::now();
    while (g_appRunning) {
        const bool forcedRejoin = g_mdnsRejoinRequested.exchange(false, std::memory_order_relaxed);
        const auto nowTp = std::chrono::steady_clock::now();
        if (forcedRejoin || nowTp - lastRejoinCheck >= REJOIN_INTERVAL) {
            rejoinMulticast(r, forcedRejoin);
            lastRejoinCheck = std::chrono::steady_clock::now();
        }

        sockaddr_in from{};
        socklen_t fromLen = sizeof(from);
        uint8_t buf[2048];
        const int n = static_cast<int>(recvfrom(r.sock, reinterpret_cast<char*>(buf), sizeof(buf),
                                                0, reinterpret_cast<sockaddr*>(&from), &fromLen));
        if (n <= 0) continue; // timeout or transient error
        answerQuery(r, buf, static_cast<size_t>(n), from);
    }

    g_mdnsResponderActive.store(false, std::memory_order_relaxed);
    sendGoodbye(r);
    closeResponder(r);
}
