// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * core/update_service.h — OTA update orchestrator (state machine).
 *
 * Owns the one and only UpdateState transition. Drives an IUpdaterPort
 * for the actual IO. Persists user preferences via callbacks (we
 * deliberately don't take an IConfigPort dependency — the existing
 * webserver code mutates g_config + saveConfig() directly and we
 * mirror that pattern).
 *
 * Thread model:
 *   - The webserver, tray, and timer fire requests via requestCheck() /
 *     requestDownload() / requestInstall() — all non-blocking, they just
 *     enqueue work onto the internal worker thread.
 *   - One dedicated worker thread (started by start(), joined by stop())
 *     runs the actual fetch/download/verify/apply against IUpdaterPort.
 *   - A second timer thread polls the worker every minute to decide
 *     whether the auto-check interval has elapsed.
 *   - All state mutations are serialized under one mutex; the SSE
 *     callback is invoked outside the lock to avoid SSE-thread starvation.
 */
#pragma once

#include "core/ports.h"
#include "core/types.h"
#include "core/update_types.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

class ILogPort;

class UpdateService {
  public:
    using StatusCallback = std::function<void(const UpdateStatusSnapshot&)>;
    // Persist-callback: invoked after the service mutates Config fields
    // (lastCheckEpoch / lastSeenVersion / skipVersion / channel / autoX
    // flags) so the caller can call saveConfig(). Always invoked with
    // the service's internal lock NOT held.
    using PersistCallback = std::function<void()>;

    UpdateService(IUpdaterPort& updater, ILogPort& log, Config& sharedConfig,
                  std::mutex& configMtx);
    ~UpdateService();

    UpdateService(const UpdateService&) = delete;
    UpdateService& operator=(const UpdateService&) = delete;

    // Spawn worker + timer threads. Call once at composition root.
    void start();

    // Signal threads to wind down and join. Safe to call multiple times.
    void stop();

    // ── Imperative entry points (non-blocking) ─────────────────────────
    // Each enqueues exactly one job and returns immediately. If a job
    // of the same kind is already running, the call is dropped (with
    // a single line logged) — there is no queue depth.
    void requestCheck(bool userInitiated);
    void requestDownload();
    void requestInstall();
    void cancelInFlight();

    // ── State queries ──────────────────────────────────────────────────
    UpdateStatusSnapshot snapshot() const;

    // ── UI notification ────────────────────────────────────────────────
    // Set a single sink for status snapshots; called after every state
    // transition. Webserver uses this to push SSE events.
    void setStatusCallback(StatusCallback cb);
    void setPersistCallback(PersistCallback cb);

    // ── User decisions ─────────────────────────────────────────────────
    // Skip a specific version. Future checks won't notify until something
    // newer appears. Persists via the persist callback.
    void skipVersion(const std::string& version);
    // "Remind me later" — clears the in-memory "available" state but
    // keeps lastSeenVersion so we won't re-banner until newer.
    void dismiss();
    // Apply settings changes (channel + autoCheck/autoDownload/autoInstall).
    // Mutates Config and triggers persist + an immediate timer-eval so a
    // freshly-enabled autoCheck fires within the minute.
    void updatePreferences(const std::string& channel, bool autoCheck,
                           bool autoDownload, bool autoInstall);

  private:
    void workerLoop();
    void timerLoop();

    // Pure state-machine helpers, all invoked from the worker thread.
    void doCheck(bool userInitiated);
    void doDownload();
    void doVerify();
    void doInstall();

    // Build a snapshot, copy the callback, drop the lock, fire. Safe to
    // call from any state-transition site as long as mtx_ is NOT held.
    void fireBroadcast();

    // Helpers
    static bool versionStrictlyNewer(const std::string& a, const std::string& b);
    static int64_t nowEpoch();

    IUpdaterPort& updater_;
    ILogPort& log_;
    Config& config_;            // shared with the rest of the app
    std::mutex& configMtx_;     // protects config_ (held by webserver too)

    // Service-owned state. cv_ wakes the worker for any pending job.
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    UpdateState state_ = UpdateState::Idle;
    UpdateInfo info_{};          // valid only when state_ >= UpdateAvailable
    std::string lastError_;
    uint64_t bytesDownloaded_ = 0;
    uint64_t bytesTotal_ = 0;
    std::string downloadedPath_;
    bool userInitiatedCheck_ = false;

    // Pending-job flags, set by request*() and consumed by the worker.
    bool pendingCheck_ = false;
    bool pendingDownload_ = false;
    bool pendingInstall_ = false;
    std::atomic<bool> cancelFlag_{false};
    std::atomic<bool> stopping_{false};

    // Threads
    std::thread workerTh_;
    std::thread timerTh_;
    bool started_ = false;

    // Callbacks (set under mtx_, fired without it).
    StatusCallback statusCb_;
    PersistCallback persistCb_;
};
