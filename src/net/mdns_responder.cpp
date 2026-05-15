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
    if (!netGetHostname(hostname, sizeof(hostname)) || hostname[0] == '\0') {
        return "satellite";
    }
    std::string h(hostname);
    // Some platforms return an FQDN; keep only the first label.
    auto dot = h.find('.');
    if (dot != std::string::npos) h = h.substr(0, dot);
    return h.empty() ? "satellite" : h;
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
    logMsg(LogLevel::INFO, "mdns",
           "mDNS responder up — advertising _satellite._udp.local. as '" + host + "'");

    while (g_appRunning) {
        sockaddr_in from{};
        socklen_t fromLen = sizeof(from);
        uint8_t buf[2048];
        int n = static_cast<int>(recvfrom(sock, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                                          reinterpret_cast<sockaddr*>(&from), &fromLen));
        if (n <= 0) continue; // timeout or transient error

        mdns::Header header;
        std::vector<mdns::Question> questions;
        if (!mdns::parsePacket(buf, static_cast<size_t>(n), header, questions)) continue;

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
        mdns::ResponseInputs in;
        in.instanceName = host;
        in.hostName = host;
        in.udpPort = static_cast<uint16_t>(g_config.udpPort);
        in.txtPairs = {
            {"udp", std::to_string(g_config.udpPort)},
            {"pair", std::to_string(g_config.pairPort)},
            {"http", std::to_string(g_config.webPort)},
        };
        uint8_t ipv4[4];
        if (getLocalIPv4(ipv4)) in.ipv4 = ipv4;

        uint8_t out[1024];
        size_t outLen = mdns::encodeResponse(out, sizeof(out), header.id, wantUnicast, in);
        if (outLen == 0) continue;

        // Unicast back to the querier when it set the QU bit; otherwise
        // multicast to the group so every cache on the segment updates.
        const sockaddr_in& dest = wantUnicast ? from : groupAddr;
        sendto(sock, reinterpret_cast<const char*>(out), static_cast<int>(outLen), 0,
               reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
    }

    setsockopt(sock, IPPROTO_IP, IP_DROP_MEMBERSHIP, reinterpret_cast<const char*>(&mreq),
               sizeof(mreq));
    closesocket(sock);
    netShutdown();
}
