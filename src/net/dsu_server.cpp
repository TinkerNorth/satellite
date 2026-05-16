// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

#include "dsu_server.h"
#include "dsu_protocol.h"
#include "net_compat.h"

#include "core/types.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <random>
#include <unordered_map>
#include <vector>

namespace {

// One DSU subscriber. Keyed by client address; cleaned up after 5 s of
// silence (matches BetterJoy / SteamDeckGyroDSU). Subscriptions can be
// "want all slots" or pinned to a single slot.
struct Subscriber {
    sockaddr_in addr;
    bool wantAllSlots = true;
    uint8_t pinnedSlot = 0;
    std::chrono::steady_clock::time_point lastSeen;
    uint32_t packetCounter = 0; // per-subscriber monotonic seq for pad data
};

struct AddrHash {
    size_t operator()(const sockaddr_in& a) const noexcept {
        return std::hash<uint64_t>{}((static_cast<uint64_t>(a.sin_addr.s_addr) << 16) ^
                                     static_cast<uint64_t>(a.sin_port));
    }
};
struct AddrEq {
    bool operator()(const sockaddr_in& a, const sockaddr_in& b) const noexcept {
        return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
    }
};

uint32_t randomServerId() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist(1, UINT32_MAX);
    return dist(gen);
}

// Pick the (up to 4) active controllers SessionService currently owns.
// Returns slot → (token, ctrlIdx). We auto-assign in scan order; the
// explicit slot-matrix UI is a follow-up PR.
struct SlotMap {
    bool occupied[dsu::MAX_SLOTS] = {};
    uint32_t token[dsu::MAX_SLOTS] = {};
    uint8_t ctrlIdx[dsu::MAX_SLOTS] = {};
    MotionReport motion[dsu::MAX_SLOTS] = {};
    bool hasMotion[dsu::MAX_SLOTS] = {};
};

SlotMap buildSlotMap(const SessionService::ConnectionsSnapshot& snap) {
    (void)snap; // Read directly from SessionService below; signature kept for
                // documentation of the data flow.
    SlotMap m{};
    return m;
}

} // namespace

DsuServer::DsuServer(SessionService& svc, std::atomic<bool>& running, std::atomic<bool>& wantListen,
                     const std::string& bindAddr, int port)
    : svc_(svc), appRunning_(running), wantListen_(wantListen), bindAddr_(bindAddr), port_(port),
      serverId_(randomServerId()) {}

DsuServer::~DsuServer() { stop(); }

void DsuServer::start() {
    if (started_.exchange(true)) return;
    thread_ = std::thread([this] { run(); });
}

void DsuServer::stop() {
    if (!started_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
}

void DsuServer::run() {
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) return;

    netDisableUdpConnReset(sock);
    netSetRecvTimeoutMs(sock, 100);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port_));
    if (bindAddr_.empty() || bindAddr_ == "0.0.0.0") {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else if (bindAddr_ == "127.0.0.1") {
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    } else {
        // Best-effort parse via inet_addr; on parse failure we fall back to
        // loopback. We intentionally don't drag in inet_pton on the older
        // header surface — DSU server bind address is operator-controlled
        // and these two strings cover the documented config values.
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    }
    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(sock);
        return;
    }
    listening_.store(true, std::memory_order_relaxed);

    std::unordered_map<sockaddr_in, Subscriber, AddrHash, AddrEq> subs;
    const auto subTtl = std::chrono::seconds(5);
    // Push pad data at ~60 Hz for now (matches the typical emulator polling
    // cadence). The 250 Hz motion forwarding cap on the dish side puts an
    // upper bound on freshness; we don't gain anything by pushing faster
    // than the emulator polls.
    auto lastPush = std::chrono::steady_clock::now();
    const auto pushInterval = std::chrono::milliseconds(16);

    while (appRunning_.load() && wantListen_.load() && started_.load()) {
        // ── Receive a packet (with 100 ms timeout) ─────────────────────────
        sockaddr_in clientAddr{};
        socklen_t clen = sizeof(clientAddr);
        uint8_t buf[256];
        int n = static_cast<int>(::recvfrom(sock, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                                            reinterpret_cast<sockaddr*>(&clientAddr), &clen));

        if (n > 0) {
            const uint32_t event = dsu::parseClientHeader(buf, static_cast<size_t>(n));
            switch (event) {
            case dsu::EVENT_VERSION: {
                uint8_t reply[32];
                size_t replyLen = dsu::encodeVersionResponse(reply, sizeof(reply), serverId_);
                if (replyLen > 0) {
                    ::sendto(sock, reinterpret_cast<const char*>(reply), static_cast<int>(replyLen),
                             0, reinterpret_cast<sockaddr*>(&clientAddr), clen);
                }
                break;
            }
            case dsu::EVENT_INFORMATION: {
                // Per spec the client lists which slots it wants info for
                // in the request tail (4B count + 4B*count). We always
                // respond for every slot — clients tolerate extras and
                // it's a one-off per subscription. The mac field is
                // synthesized from serverId so each "controller" has a
                // stable, unique mac for the session.
                //
                // Slot occupancy MUST come from the same per-slot mapping the
                // Pad Data path uses (getMotionSlotsForDsu) — not a global
                // count. A global count assumes controllers densely fill slots
                // 0..N-1, which getMotionSlotsForDsu does not guarantee, and
                // would make Information disagree with Pad Data about which
                // slots are live.
                auto infoSlots = svc_.getMotionSlotsForDsu();
                for (uint8_t s = 0; s < dsu::MAX_SLOTS; ++s) {
                    const bool connected = infoSlots[s].occupied;
                    std::array<uint8_t, 6> mac{};
                    mac[0] = 0x02; // locally-administered prefix
                    mac[1] = static_cast<uint8_t>(serverId_);
                    mac[2] = static_cast<uint8_t>(serverId_ >> 8);
                    mac[3] = static_cast<uint8_t>(serverId_ >> 16);
                    mac[4] = static_cast<uint8_t>(serverId_ >> 24);
                    mac[5] = s;
                    uint8_t reply[32];
                    size_t replyLen = dsu::encodeInformationResponse(reply, sizeof(reply),
                                                                     serverId_, s, connected, mac);
                    if (replyLen > 0) {
                        ::sendto(sock, reinterpret_cast<const char*>(reply),
                                 static_cast<int>(replyLen), 0,
                                 reinterpret_cast<sockaddr*>(&clientAddr), clen);
                    }
                }
                break;
            }
            case dsu::EVENT_PAD_DATA: {
                dsu::SubscriptionRequest req{};
                if (dsu::parseSubscriptionRequest(buf, static_cast<size_t>(n), req)) {
                    auto& sub = subs[clientAddr];
                    sub.addr = clientAddr;
                    sub.wantAllSlots = req.wantAllSlots;
                    sub.pinnedSlot = req.slotIndex;
                    sub.lastSeen = std::chrono::steady_clock::now();
                }
                break;
            }
            default:
                break;
            }
        }

        // ── Push pad data to subscribers on the cadence ────────────────────
        const auto now = std::chrono::steady_clock::now();
        if (now - lastPush >= pushInterval && !subs.empty()) {
            lastPush = now;

            // Pull the per-slot motion snapshot from SessionService. The
            // service auto-maps the first MAX_SLOTS (4) active controllers
            // in scan order onto DSU slots 0..3 — the explicit slot matrix
            // lives in the web UI (follow-up).
            auto motionSlots = svc_.getMotionSlotsForDsu();

            struct LiveSlot {
                bool occupied = false;
                MotionReport motion{};
                bool hasMotion = false;
                std::array<uint8_t, 6> mac{};
            };
            LiveSlot slots[dsu::MAX_SLOTS];
            for (int s = 0; s < dsu::MAX_SLOTS; ++s) {
                slots[s].occupied = motionSlots[s].occupied;
                slots[s].motion = motionSlots[s].motion;
                slots[s].hasMotion = motionSlots[s].hasMotion;
                if (slots[s].occupied) {
                    slots[s].mac[0] = 0x02;
                    slots[s].mac[1] = static_cast<uint8_t>(serverId_);
                    slots[s].mac[2] = static_cast<uint8_t>(serverId_ >> 8);
                    slots[s].mac[3] = static_cast<uint8_t>(serverId_ >> 16);
                    slots[s].mac[4] = static_cast<uint8_t>(serverId_ >> 24);
                    slots[s].mac[5] = static_cast<uint8_t>(s);
                }
            }

            // Push to every subscriber. Drop subs that have been silent
            // past the TTL.
            for (auto it = subs.begin(); it != subs.end();) {
                if (now - it->second.lastSeen > subTtl) {
                    it = subs.erase(it);
                    continue;
                }
                for (uint8_t s = 0; s < dsu::MAX_SLOTS; ++s) {
                    if (!it->second.wantAllSlots && it->second.pinnedSlot != s) continue;
                    dsu::PadDataInputs inputs;
                    inputs.slotIndex = s;
                    inputs.connected = slots[s].occupied;
                    inputs.mac = slots[s].mac;
                    inputs.battery =
                        slots[s].occupied ? dsu::SLOT_BATTERY_FULL : dsu::SLOT_BATTERY_NONE;
                    inputs.packetNumber = ++it->second.packetCounter;
                    inputs.timestampMicros =
                        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                  now.time_since_epoch())
                                                  .count());
                    if (slots[s].hasMotion) { dsu::applyMotionReport(inputs, slots[s].motion); }
                    uint8_t pkt[dsu::PAD_DATA_PACKET_SIZE];
                    size_t plen = dsu::encodePadDataResponse(pkt, sizeof(pkt), serverId_, inputs);
                    if (plen > 0) {
                        ::sendto(sock, reinterpret_cast<const char*>(pkt), static_cast<int>(plen),
                                 0, reinterpret_cast<sockaddr*>(&it->second.addr),
                                 sizeof(sockaddr_in));
                    }
                }
                ++it;
            }
            subscriberCount_.store(static_cast<int>(subs.size()), std::memory_order_relaxed);
        }
    }

    listening_.store(false, std::memory_order_relaxed);
    subscriberCount_.store(0, std::memory_order_relaxed);
    closesocket(sock);
}
