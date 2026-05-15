// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * net/dsu_server.h — Cemuhook DSU server thread.
 *
 * Listens on udp://<bindAddr>:<dsuPort> (default 127.0.0.1:26760) and
 * re-emits forwarded controllers' IMU + sticks/buttons to subscribed
 * emulators speaking the DSU client protocol. Wire format lives in
 * dsu_protocol.h; this module is just the socket loop + subscription
 * bookkeeping.
 *
 * The server pulls motion / button data through SessionService — it does
 * NOT keep a parallel cache of controller state. That keeps the source of
 * truth single (the dish-side senders) and means slot 0..3 is whichever
 * 4 controllers SessionService currently has active in index order
 * (auto-mapped; the explicit slot-matrix UI is a follow-up).
 */
#pragma once

#include "core/session_service.h"

#include <atomic>
#include <thread>

class DsuServer {
  public:
    DsuServer(SessionService& svc, std::atomic<bool>& running, std::atomic<bool>& wantListen,
              const std::string& bindAddr, int port);
    ~DsuServer();

    // Start the server thread. Idempotent — calling twice is a no-op.
    void start();

    // Stop and join the thread. Idempotent.
    void stop();

    // Has the listening socket bound successfully? Used by the web UI to
    // surface DSU server health.
    bool isListening() const { return listening_.load(std::memory_order_relaxed); }

    // Number of clients currently subscribed (for Information). The web UI
    // surfaces this as "DSU: 2 emulators subscribed" in the diagnostics pane.
    int subscriberCount() const { return subscriberCount_.load(std::memory_order_relaxed); }

  private:
    void run();

    SessionService& svc_;
    std::atomic<bool>& appRunning_;
    std::atomic<bool>& wantListen_;
    std::string bindAddr_;
    int port_;

    std::thread thread_;
    std::atomic<bool> started_{false};
    std::atomic<bool> listening_{false};
    std::atomic<int> subscriberCount_{0};

    // Stable random id this server instance reports to clients. Persisted
    // for the lifetime of the process so a client that re-subscribes after
    // a brief loss can resume against the same logical server.
    uint32_t serverId_;
};
