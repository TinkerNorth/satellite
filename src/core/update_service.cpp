// SPDX-License-Identifier: LGPL-3.0-or-later

#include "core/update_service.h"

#include "core/ports.h"
#include "core/semver.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace {
enum class RetryStep { None, Download, Repair, Install, Check };

bool applyPreferences(Config& cfg, const std::string& channel, bool autoCheck, bool autoDownload,
                      bool autoInstall, bool& channelChanged) {
    std::string ch = channel;
    if (ch != UPDATE_CHANNEL_STABLE && ch != UPDATE_CHANNEL_PRERELEASE) {
        ch = UPDATE_CHANNEL_STABLE;
    }
    channelChanged = (ch != cfg.updateChannel);
    cfg.updateChannel = ch;
    cfg.autoCheck = autoCheck;
    cfg.autoDownload = autoDownload;
    cfg.autoInstall = autoInstall;
    return true;
}
} // namespace

static bool workInFlight(UpdateState state) {
    switch (state) {
    case UpdateState::Checking:
    case UpdateState::Downloading:
    case UpdateState::Verifying:
    case UpdateState::Installing:
        return true;
    case UpdateState::Idle:
    case UpdateState::UpToDate:
    case UpdateState::UpdateAvailable:
    case UpdateState::Downloaded:
    case UpdateState::Error:
        return false;
    }
    return false;
}

static bool dismissible(UpdateState state) {
    return state == UpdateState::UpdateAvailable || state == UpdateState::Downloaded;
}

static UpdateState failedCheckState(bool userInitiated, bool knownIsStaged, bool knownIsNewer) {
    if (userInitiated) return UpdateState::Error;
    if (knownIsStaged) return UpdateState::Downloaded;
    if (knownIsNewer) return UpdateState::UpdateAvailable;
    return UpdateState::Error;
}

static RetryStep retryStepFor(UpdateState state, UpdateState failedPhase, bool releaseIsNewer) {
    if (state != UpdateState::Error) return RetryStep::None;
    switch (failedPhase) {
    case UpdateState::Downloading:
    case UpdateState::Verifying:
        return releaseIsNewer ? RetryStep::Download : RetryStep::Repair;
    case UpdateState::Installing:
        return RetryStep::Install;
    case UpdateState::Idle:
    case UpdateState::Checking:
    case UpdateState::UpToDate:
    case UpdateState::UpdateAvailable:
    case UpdateState::Downloaded:
    case UpdateState::Error:
        return RetryStep::Check;
    }
    return RetryStep::Check;
}

UpdateService::UpdateService(IUpdaterPort& updater, ILogPort& log, ConfigStore& configStore)
    : updater_(updater), log_(log), configStore_(configStore) {}

UpdateService::~UpdateService() { stop(); }

void UpdateService::start() {
    if (started_) return;
    started_ = true;
    stopping_ = false;
    workerTh_ = std::thread([this] { workerLoop(); });
    timerTh_ = std::thread([this] { timerLoop(); });
}

void UpdateService::stop() {
    if (!started_) return;
    stopping_ = true;
    {
        std::lock_guard<TrackedMutex> lk(mtx_);
        cancelFlag_ = true;
        cv_.notify_all();
    }
    {
        std::lock_guard<std::mutex> lk(timerMtx_);
        timerCv_.notify_all();
    }
    blockingRegion("thread.join");
    if (workerTh_.joinable()) workerTh_.join();
    if (timerTh_.joinable()) timerTh_.join();
    started_ = false;
}

void UpdateService::requestCheck(bool userInitiated) {
    {
        std::lock_guard<TrackedMutex> lk(mtx_);
        const bool checkQueued = checkQueuedLocked();
        if (checkQueued && userInitiated) userInitiatedCheck_ = true;
        if (checkQueued || workInFlight(state_)) return;
        acceptCheckLocked(userInitiated);
    }
    cv_.notify_all();
}

void UpdateService::requestDownload() {
    {
        std::lock_guard<TrackedMutex> lk(mtx_);
        if (state_ != UpdateState::UpdateAvailable && state_ != UpdateState::Error) return;
        if (!info_.available) return;
        if (info_.installMethod == InstallMethod::Manual) return;
        acceptDownloadLocked();
    }
    cv_.notify_all();
}

void UpdateService::requestRepair() {
    {
        std::lock_guard<TrackedMutex> lk(mtx_);
        if (info_.installMethod == InstallMethod::Manual) return;
        if (checkQueuedLocked()) {
            repairPending_ = true;
            userInitiatedCheck_ = true;
            dismissedVersion_.clear();
            return;
        }
        if (workInFlight(state_) || state_ == UpdateState::Downloaded) return;
        repairPending_ = true;
        if (info_.version.empty()) {
            acceptCheckLocked(true);
        } else {
            acceptDownloadLocked();
        }
    }
    cv_.notify_all();
}

void UpdateService::requestInstall() {
    {
        std::lock_guard<TrackedMutex> lk(mtx_);
        const bool downloaded = state_ == UpdateState::Downloaded;
        const bool installFailed =
            state_ == UpdateState::Error && failedPhase_ == UpdateState::Installing;
        if (!downloaded && !installFailed) return;
        if (!isStagedLocked(info_)) return;
        acceptInstallLocked();
    }
    cv_.notify_all();
}

void UpdateService::requestRetry() {
    std::unique_lock<TrackedMutex> lk(mtx_);
    const RetryStep step = retryStepFor(state_, failedPhase_, info_.available);
    lk.unlock();
    switch (step) {
    case RetryStep::None:
        break;
    case RetryStep::Download:
        requestDownload();
        break;
    case RetryStep::Repair:
        requestRepair();
        break;
    case RetryStep::Install:
        requestInstall();
        break;
    case RetryStep::Check:
        requestCheck(true);
        break;
    }
}

void UpdateService::cancelInFlight() {
    bool droppedQueued = false;
    {
        std::lock_guard<TrackedMutex> lk(mtx_);
        cancelFlag_ = true;
        droppedQueued = pendingDownload_;
        if (droppedQueued) {
            pendingDownload_ = false;
            repairPending_ = false;
            abandonDownloadLocked();
        }
    }
    if (droppedQueued) log_.logMsg(LogLevel::INFO, "updater", "Download cancelled");
}

UpdateStatusSnapshot UpdateService::snapshot() const {
    const std::shared_ptr<const Config> cfg = configStore_.current();
    std::lock_guard<TrackedMutex> lk(mtx_);
    return snapshotLocked(*cfg);
}

void UpdateService::setStatusCallback(StatusCallback cb) {
    std::lock_guard<TrackedMutex> lk(mtx_);
    statusCb_ = std::move(cb);
}

void UpdateService::skipVersion(const std::string& version) {
    configStore_.update([&version](Config& cfg) {
        cfg.skipVersion = version;
        return true;
    });
    {
        std::lock_guard<TrackedMutex> lk(mtx_);
        if (info_.version == version) {
            info_ = {};
            state_ = UpdateState::Idle;
            lastError_.clear();
            failedPhase_ = UpdateState::Idle;
        }
        if (staged_.version == version) staged_ = {};
    }
    fireBroadcast();
}

void UpdateService::dismiss() {
    std::string version;
    {
        std::lock_guard<TrackedMutex> lk(mtx_);
        if (!dismissible(state_)) return;
        version = info_.version;
        dismissedVersion_ = version;
    }
    configStore_.update([&version](Config& cfg) {
        cfg.lastSeenVersion = version;
        return true;
    });
    fireBroadcast();
}

void UpdateService::updatePreferences(const std::string& channel, bool autoCheck, bool autoDownload,
                                      bool autoInstall) {
    bool channelChanged = false;
    configStore_.update([&](Config& cfg) {
        return applyPreferences(cfg, channel, autoCheck, autoDownload, autoInstall, channelChanged);
    });
    fireBroadcast();
    if (channelChanged) requestCheck(/*userInitiated=*/false);
}

// Waits for the next job and takes it. Install outranks download outranks
// check.
UpdateService::Job UpdateService::takeNextJob() {
    std::unique_lock<TrackedMutex> lk(mtx_);
    cv_.wait(lk, [&] { return stopping_ || pendingCheck_ || pendingDownload_ || pendingInstall_; });
    if (stopping_) return Job::Stop;
    cancelFlag_ = false;
    if (pendingInstall_) {
        pendingInstall_ = false;
        return Job::Install;
    }
    if (pendingDownload_) {
        pendingDownload_ = false;
        return Job::Download;
    }
    pendingCheck_ = false;
    enterStateLocked(UpdateState::Checking);
    return Job::Check;
}

// After a check: the auto-download preference queues the download when the
// check found one and it can be installed from here, and a pending repair queues
// its download when the release we already run is usable, or drops itself.
void UpdateService::queueDownloadIfWanted(bool fetched) {
    const bool autoDownload = configStore_.current()->autoDownload;
    std::lock_guard<TrackedMutex> lk(mtx_);
    if (workInFlight(state_)) return;
    const bool selfInstallable = info_.installMethod == InstallMethod::SelfInstall;
    const bool offered = fetched && state_ == UpdateState::UpdateAvailable && selfInstallable;
    const bool repairUsable =
        fetched && state_ != UpdateState::Downloaded && !info_.version.empty() && selfInstallable;
    const bool repairWanted = repairPending_ && repairUsable;
    repairPending_ = repairWanted;
    if ((autoDownload && offered) || repairWanted) acceptDownloadLocked();
}

void UpdateService::workerLoop() {
    ScopedThreadRole role(ThreadRole::Update);
    while (!stopping_) {
        const Job job = takeNextJob();
        switch (job) {
        case Job::Stop:
            return;
        case Job::Install:
            doInstall();
            break;
        case Job::Download:
            doDownload();
            break;
        case Job::Check: {
            const bool fetched = doCheck();
            queueDownloadIfWanted(fetched);
            break;
        }
        }
    }
}

void UpdateService::timerLoop() {
    ScopedThreadRole role(ThreadRole::Update);
    using namespace std::chrono_literals;
    auto interruptibleSleep = [this](std::chrono::seconds d) {
        std::unique_lock<std::mutex> lk(timerMtx_);
        timerCv_.wait_for(lk, d, [this] { return stopping_.load(); });
    };
    interruptibleSleep(30s); // settle before the first periodic eval
    while (!stopping_) {
        bool shouldCheck = false;
        {
            const std::shared_ptr<const Config> cfg = configStore_.current();
            if (cfg->autoCheck) {
                int64_t age = nowEpoch() - cfg->lastCheckEpoch;
                int64_t intervalSec = static_cast<int64_t>(cfg->updateCheckIntervalHours) * 3600;
                if (intervalSec < 3600) intervalSec = 3600;
                if (age >= intervalSec) shouldCheck = true;
            }
        }
        if (shouldCheck) requestCheck(/*userInitiated=*/false);
        interruptibleSleep(60s);
    }
}

// Callers must NOT hold mtx_.
void UpdateService::fireBroadcast() {
    UpdateStatusSnapshot snap;
    StatusCallback cb;
    const std::shared_ptr<const Config> cfg = configStore_.current();
    {
        std::lock_guard<TrackedMutex> lk(mtx_);
        snap = snapshotLocked(*cfg);
        cb = statusCb_;
    }
    if (cb) cb(snap);
}

void UpdateService::acceptCheckLocked(bool userInitiated) {
    pendingCheck_ = true;
    userInitiatedCheck_ = userInitiated;
    enterStateLocked(UpdateState::Checking);
}

void UpdateService::acceptDownloadLocked() {
    pendingDownload_ = true;
    dismissedVersion_.clear();
    enterStateLocked(UpdateState::Downloading);
    bytesDownloaded_ = 0;
    bytesTotal_ = info_.assetSize;
}

void UpdateService::acceptInstallLocked() {
    pendingInstall_ = true;
    repairPending_ = false;
    dismissedVersion_.clear();
    enterStateLocked(UpdateState::Installing);
}

void UpdateService::abandonDownloadLocked() {
    const UpdateState settled = info_.available ? UpdateState::UpdateAvailable : UpdateState::Idle;
    enterStateLocked(settled);
    bytesDownloaded_ = 0;
    bytesTotal_ = 0;
}

bool UpdateService::checkQueuedLocked() const {
    return pendingCheck_ || state_ == UpdateState::Checking;
}

bool UpdateService::checkSupersededLocked() const {
    return state_ != UpdateState::Checking || pendingCheck_;
}

void UpdateService::enterStateLocked(UpdateState state) {
    state_ = state;
    lastError_.clear();
    failedPhase_ = UpdateState::Idle;
}

bool UpdateService::isStagedLocked(const UpdateInfo& info) const {
    const bool somethingStaged = !staged_.version.empty();
    const bool sameRelease = info.version == staged_.version && info.assetName == staged_.assetName;
    const bool sameBytes = info.assetSha256 == staged_.assetSha256;
    return somethingStaged && sameRelease && sameBytes;
}

UpdateStatusSnapshot UpdateService::snapshotLocked(const Config& cfg) const {
    UpdateStatusSnapshot s;
    s.state = state_;
    s.currentVersion = SATELLITE_VERSION;
    s.info = info_;
    s.bytesDownloaded = bytesDownloaded_;
    s.totalBytes = bytesTotal_;
    s.message = lastError_;
    s.failedPhase = failedPhase_;
    const bool versionDismissed = !dismissedVersion_.empty() && info_.version == dismissedVersion_;
    s.dismissed = dismissible(state_) && versionDismissed;
    s.platformId = updater_.platformId();
    s.lastCheckEpoch = cfg.lastCheckEpoch;
    s.channel = cfg.updateChannel;
    s.autoCheck = cfg.autoCheck;
    s.autoDownload = cfg.autoDownload;
    s.autoInstall = cfg.autoInstall;
    return s;
}

int64_t UpdateService::nowEpoch() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

bool UpdateService::versionStrictlyNewer(const std::string& a, const std::string& b) {
    return satellite::compareSemver(a, b) > 0;
}

void UpdateService::settleCheck(UpdateState state, const UpdateInfo& info, LogLevel level,
                                const std::string& message) {
    bool staged = false;
    {
        std::lock_guard<TrackedMutex> lk(mtx_);
        if (checkSupersededLocked()) return;
        staged = isStagedLocked(info);
        const UpdateState settled = staged ? UpdateState::Downloaded : state;
        info_ = info;
        dismissedVersion_.clear();
        enterStateLocked(settled);
    }
    const std::string line =
        staged ? "Update " + info.version + " is already downloaded and verified" : message;
    log_.logMsg(level, "updater", line);
    fireBroadcast();
}

void UpdateService::settleFailedCheck(const std::string& err) {
    {
        std::lock_guard<TrackedMutex> lk(mtx_);
        if (checkSupersededLocked()) return;
        const bool knownIsStaged = isStagedLocked(info_);
        const UpdateState settled =
            failedCheckState(userInitiatedCheck_, knownIsStaged, info_.available);
        enterStateLocked(settled);
        if (settled == UpdateState::Error) {
            lastError_ = err.empty() ? "Update check failed (network or API error)" : err;
            failedPhase_ = UpdateState::Checking;
        }
    }
    log_.logMsg(LogLevel::WARN, "updater", "Check failed: " + err);
    fireBroadcast();
}

bool UpdateService::doCheck() {
    bool userInitiated = false;
    {
        std::lock_guard<TrackedMutex> lk(mtx_);
        userInitiated = userInitiatedCheck_;
    }
    fireBroadcast();
    log_.logMsg(LogLevel::INFO, "updater",
                userInitiated ? "Manual update check started" : "Auto update check started");

    const std::shared_ptr<const Config> cfg = configStore_.current();
    const std::string channel = cfg->updateChannel;
    const std::string skipVer = cfg->skipVersion;

    UpdateInfo info;
    std::string err;
    const bool ok = updater_.fetchLatestRelease(channel, SATELLITE_VERSION, info, err);

    configStore_.update([](Config& next) {
        next.lastCheckEpoch = nowEpoch();
        return true;
    });

    if (!ok) {
        // A failed check keeps whatever info the last one left.
        settleFailedCheck(err);
        return false;
    }

    info.available = versionStrictlyNewer(info.version, SATELLITE_VERSION);
    if (!info.available) {
        settleCheck(UpdateState::UpToDate, info, LogLevel::INFO,
                    "Up to date (current: " + std::string(SATELLITE_VERSION) +
                        ", latest: " + info.version + ")");
        return true;
    }
    const bool skipped = !skipVer.empty() && satellite::compareSemver(info.version, skipVer) <= 0;
    if (skipped) {
        info.available = false;
        settleCheck(UpdateState::UpToDate, info, LogLevel::INFO,
                    "Found " + info.version + " but skipVersion suppresses notification");
        return true;
    }
    settleCheck(UpdateState::UpdateAvailable, info, LogLevel::INFO,
                "Update " + info.version + " available (" + info.assetName + ")");
    return true;
}

// Whether one progress report is worth a broadcast: a whole percent when the
// size is known, a quarter megabyte when it is not, so a slow link does not
// flood the dashboard and a fast one still moves the bar.
bool progressWorthBroadcasting(uint64_t doneBefore, uint64_t totalBefore, uint64_t doneNow,
                               uint64_t totalNow) {
    if (totalNow > 0) {
        const uint64_t prevPct = totalBefore > 0 ? (doneBefore * 100) / totalBefore : 0;
        const uint64_t newPct = (doneNow * 100) / totalNow;
        return newPct != prevPct;
    }
    return doneNow - doneBefore >= 256 * 1024;
}

void UpdateService::onDownloadProgress(uint64_t soFar, uint64_t total) {
    bool shouldBroadcast = false;
    {
        std::lock_guard<TrackedMutex> lk(mtx_);
        shouldBroadcast = progressWorthBroadcasting(bytesDownloaded_, bytesTotal_, soFar, total);
        bytesDownloaded_ = soFar;
        bytesTotal_ = total > 0 ? total : bytesTotal_;
    }
    if (shouldBroadcast) fireBroadcast();
}

// Moves into Downloading, or settles where a download makes no sense: nothing
// to download, or a release that is installed by hand. False when settled, or
// when a check accepted since has taken its place.
bool UpdateService::beginDownload(UpdateInfo& info) {
    bool settled = false;
    {
        std::lock_guard<TrackedMutex> lk(mtx_);
        if (pendingCheck_) return false;
        info = info_;
        const bool repair = repairPending_;
        repairPending_ = false;
        if (!info.available && !repair) {
            abandonDownloadLocked();
            lastError_ = "No update to download";
            settled = true;
        } else if (info.installMethod == InstallMethod::Manual) {
            abandonDownloadLocked();
            settled = true;
        } else {
            state_ = UpdateState::Downloading;
            lastError_.clear();
            failedPhase_ = UpdateState::Idle;
            bytesDownloaded_ = 0;
            bytesTotal_ = info.assetSize;
            staged_ = {};
        }
    }
    fireBroadcast();
    return !settled;
}

void UpdateService::doDownload() {
    UpdateInfo info;
    if (!beginDownload(info)) return;
    log_.logMsg(LogLevel::INFO, "updater", "Downloading " + info.assetName);

    std::string localPath, err;
    const bool ok = updater_.downloadArtifact(
        info, [this](uint64_t soFar, uint64_t total) { onDownloadProgress(soFar, total); },
        &cancelFlag_, localPath, err);
    const bool cancelled = !ok && cancelFlag_.load();
    if (cancelled) {
        settleCancelledDownload();
        return;
    }
    if (!ok) {
        {
            std::lock_guard<TrackedMutex> lk(mtx_);
            if (state_ != UpdateState::Downloading) return;
            state_ = UpdateState::Error;
            lastError_ = err.empty() ? "Download failed" : err;
            failedPhase_ = UpdateState::Downloading;
        }
        log_.logMsg(LogLevel::WARN, "updater", "Download failed: " + err);
        fireBroadcast();
        return;
    }

    {
        std::lock_guard<TrackedMutex> lk(mtx_);
        if (state_ != UpdateState::Downloading) return;
        downloadedPath_ = localPath;
        state_ = UpdateState::Verifying;
    }
    fireBroadcast();
    log_.logMsg(LogLevel::INFO, "updater", "Verifying " + localPath);
    doVerify();
}

void UpdateService::settleCancelledDownload() {
    {
        std::lock_guard<TrackedMutex> lk(mtx_);
        if (state_ != UpdateState::Downloading) return;
        abandonDownloadLocked();
    }
    log_.logMsg(LogLevel::INFO, "updater", "Download cancelled");
    fireBroadcast();
}

void UpdateService::doVerify() {
    UpdateInfo info;
    std::string localPath;
    {
        std::lock_guard<TrackedMutex> lk(mtx_);
        info = info_;
        localPath = downloadedPath_;
    }
    std::string err;
    bool ok = updater_.verifyArtifact(localPath, info, err);
    if (!ok) {
        {
            std::lock_guard<TrackedMutex> lk(mtx_);
            if (state_ != UpdateState::Verifying) return;
            state_ = UpdateState::Error;
            lastError_ = err.empty() ? "Signature/checksum verification failed" : err;
            failedPhase_ = UpdateState::Verifying;
        }
        log_.logMsg(LogLevel::ERR, "updater", "Verify failed: " + err);
        fireBroadcast();
        return;
    }
    const bool autoInstall = configStore_.current()->autoInstall;
    {
        std::lock_guard<TrackedMutex> lk(mtx_);
        if (state_ != UpdateState::Verifying) return;
        state_ = UpdateState::Downloaded;
        staged_ = {info.version, info.assetName, info.assetSha256};
        if (autoInstall) acceptInstallLocked();
    }
    log_.logMsg(LogLevel::INFO, "updater", "Update " + info.version + " ready to install");
    fireBroadcast();
}

void UpdateService::doInstall() {
    UpdateInfo info;
    std::string localPath;
    {
        std::lock_guard<TrackedMutex> lk(mtx_);
        if (state_ != UpdateState::Installing) return;
        info = info_;
        localPath = downloadedPath_;
    }
    fireBroadcast();
    log_.logMsg(LogLevel::INFO, "updater",
                "Launching installer for " + info.version + " (" + info.assetName + ")");
    std::string err;
    bool ok = updater_.applyUpdate(localPath, info, err);
    if (!ok) {
        {
            std::lock_guard<TrackedMutex> lk(mtx_);
            if (state_ != UpdateState::Installing) return;
            state_ = UpdateState::Error;
            lastError_ = err.empty() ? "Failed to launch installer" : err;
            failedPhase_ = UpdateState::Installing;
        }
        log_.logMsg(LogLevel::ERR, "updater", "Install failed: " + err);
        fireBroadcast();
        return;
    }
    log_.logMsg(LogLevel::INFO, "updater", "Installer launched; awaiting exit");
}
