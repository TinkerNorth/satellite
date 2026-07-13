// SPDX-License-Identifier: LGPL-3.0-or-later

// OTA update orchestrator. Owns the UpdateState transition and drives an
// IUpdaterPort for IO. Persists prefs via a callback + shared Config&, not a
// port: config is infrastructure the adapters own, not domain state.
//
// Thread model: request*() are non-blocking and enqueue onto one worker thread
// that runs fetch/download/verify/apply. A second timer thread polls each minute
// for the auto-check interval. State is serialized under one mutex; the SSE
// callback fires outside the lock.
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
    // Invoked after the service mutates Config fields so the caller can
    // saveConfig(). Always invoked with the internal lock NOT held.
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

    // Non-blocking; each enqueues one job. A duplicate of an in-flight job is
    // dropped; there is no queue depth.
    void requestCheck(bool userInitiated);
    void requestDownload();
    void requestInstall();
    void cancelInFlight();

    UpdateStatusSnapshot snapshot() const;

    // Single sink for status snapshots, called after every state transition.
    void setStatusCallback(StatusCallback cb);
    void setPersistCallback(PersistCallback cb);

    // Future checks won't notify until something newer than `version` appears.
    void skipVersion(const std::string& version);
    // "Remind me later": clears in-memory "available" but keeps lastSeenVersion.
    void dismiss();
    // Mutates Config + persists + triggers an immediate timer-eval so a freshly
    // enabled autoCheck fires within the minute.
    void updatePreferences(const std::string& channel, bool autoCheck, bool autoDownload,
                           bool autoInstall);

    // Public so the gate rule is unit-testable without the worker thread.
    static bool versionStrictlyNewer(const std::string& a, const std::string& b);

  private:
    void workerLoop();
    void timerLoop();

    // State-machine helpers, all invoked from the worker thread.
    void doCheck(bool userInitiated);
    void doDownload();
    void doVerify();
    void doInstall();

    // Build a snapshot, copy the callback, drop the lock, fire. mtx_ must NOT
    // be held.
    void fireBroadcast();

    static int64_t nowEpoch();

    IUpdaterPort& updater_;
    ILogPort& log_;
    Config& config_;
    std::mutex& configMtx_; // protects config_ (held by webserver too)

    mutable std::mutex mtx_;
    std::condition_variable cv_;
    // Separate cv so stop() interrupts the timer's interval sleep at once
    // instead of waiting out the current 1 s slice. The timer never contends on
    // mtx_, so it gets its own mutex.
    std::mutex timerMtx_;
    std::condition_variable timerCv_;
    UpdateState state_ = UpdateState::Idle;
    UpdateInfo info_{}; // valid only when state_ >= UpdateAvailable
    std::string lastError_;
    UpdateState failedPhase_ = UpdateState::Idle; // valid when state_ == Error
    uint64_t bytesDownloaded_ = 0;
    uint64_t bytesTotal_ = 0;
    std::string downloadedPath_;
    bool userInitiatedCheck_ = false;

    bool pendingCheck_ = false;
    bool pendingDownload_ = false;
    bool pendingInstall_ = false;
    std::atomic<bool> cancelFlag_{false};
    std::atomic<bool> stopping_{false};

    std::thread workerTh_;
    std::thread timerTh_;
    bool started_ = false;

    StatusCallback statusCb_;
    PersistCallback persistCb_;
};
