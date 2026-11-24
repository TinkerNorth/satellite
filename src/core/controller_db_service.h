// SPDX-License-Identifier: LGPL-3.0-or-later

// Keeps the active controller database: the bundled copy, or the cached
// download when it is newer, and the check that fetches a newer one from
// dish.tinkernorth.com (docs/ARCHITECTURE.md "Controller database"). The
// decisions are pure functions below; the IO is IControllerDbPort.
//
// Thread model: checkNow() runs on the caller's thread; start() spawns one
// worker that runs it on the schedule and on requestCheck(). The active
// snapshot and the status sit under one mutex and are read by copy.
#pragma once

#include "core/config_store.h"
#include "core/controller_db.h"
#include "core/ports.h"
#include "core/tracked_mutex.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace satellite {

inline constexpr const char* CONTROLLER_DB_BASE_URL =
    "https://dish.tinkernorth.com/controllers/v1/";

// The contract's first check comes ten seconds after launch.
inline constexpr int CONTROLLER_DB_FIRST_CHECK_SECONDS = 10;

enum class DbCheckResult : uint8_t { UpToDate, Updated, Offline, Invalid, Off };

const char* dbCheckResultName(DbCheckResult result);

enum class ManifestStep : uint8_t { Done, Download };

struct ManifestDecision {
    ManifestStep step = ManifestStep::Done;
    DbCheckResult result = DbCheckResult::Offline;
    ControllerDbManifest manifest;
};

// What one manifest GET means for a check against the active dbVersion.
ManifestDecision decideOnManifest(const DbFetchResponse& response, long activeVersion);

// The cache replaces the bundled copy only when it builds and is newer.
bool cacheSupersedes(SnapshotBuild cacheBuild, long cacheVersion, long bundledVersion);

// Seconds from the service's start at which the next automatic check is due:
// the first ten seconds in, then every `intervalHours` (at least one) after
// the last one.
int64_t nextCheckDueSeconds(bool checkedBefore, int64_t lastCheckSeconds, int intervalHours);

struct ControllerDbStatus {
    long bundledVersion = 0;
    long activeVersion = 0;
    DbSource source = DbSource::Bundled;
    bool enabled = true;
    bool checking = false;
    bool checked = false;
    int64_t lastCheckEpoch = 0;
    DbCheckResult lastResult = DbCheckResult::UpToDate;
};

class ControllerDbService {
  public:
    ControllerDbService(IControllerDbPort& port, ILogPort& log, ConfigStore& configStore);
    ~ControllerDbService();

    ControllerDbService(const ControllerDbService&) = delete;
    ControllerDbService& operator=(const ControllerDbService&) = delete;

    // The bundled copy, then the cache over it when newer. False when not even
    // the bundled copy builds; active() then holds an empty database.
    bool load();

    // One check, synchronously: off, up to date, or a verified newer copy
    // written to the cache and made active.
    DbCheckResult checkNow();

    void start();
    void stop();

    // Wakes the worker for one check now, whatever the schedule says.
    void requestCheck();

    std::shared_ptr<const ControllerDbSnapshot> active() const;
    ControllerDbStatus status() const;

  private:
    DbCheckResult runCheck();
    DbCheckResult download(const ControllerDbManifest& manifest, const ControllerDbFiles& base,
                           ControllerDbSnapshot& out);
    void settle(DbCheckResult result, const std::string& detail);
    void workerLoop();
    bool waitForNextCheck();
    std::chrono::steady_clock::time_point nextDueLocked() const;
    Sha256Fn digest();

    IControllerDbPort& port_;
    ILogPort& log_;
    ConfigStore& configStore_;

    mutable TrackedMutex mtx_{"controllerdb.state"};
    std::shared_ptr<const ControllerDbSnapshot> active_;
    long bundledVersion_ = 0;
    std::string manifestETag_;
    bool checking_ = false;
    bool checked_ = false;
    int64_t lastCheckEpoch_ = 0;
    DbCheckResult lastResult_ = DbCheckResult::UpToDate;

    // The worker's interruptible wait; untracked like UpdateService's timer.
    std::mutex wakeMtx_;
    std::condition_variable wakeCv_;
    bool requested_ = false;
    bool stopping_ = false;
    std::chrono::steady_clock::time_point startedAt_{};
    std::chrono::steady_clock::time_point lastAttemptAt_{};
    bool attempted_ = false;
    std::thread worker_;
};

} // namespace satellite
