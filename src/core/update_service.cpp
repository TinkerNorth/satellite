// SPDX-License-Identifier: LGPL-3.0-or-later

#include "core/update_service.h"

#include "core/ports.h"
#include "core/semver.h"

#include <algorithm>
#include <chrono>
#include <cstring>

UpdateService::UpdateService(IUpdaterPort& updater, ILogPort& log, Config& sharedConfig,
                             std::mutex& configMtx)
    : updater_(updater), log_(log), config_(sharedConfig), configMtx_(configMtx) {}

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
    cancelFlag_ = true;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        cv_.notify_all();
    }
    {
        std::lock_guard<std::mutex> lk(timerMtx_);
        timerCv_.notify_all();
    }
    if (workerTh_.joinable()) workerTh_.join();
    if (timerTh_.joinable()) timerTh_.join();
    started_ = false;
}

void UpdateService::requestCheck(bool userInitiated) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (pendingCheck_ || state_ == UpdateState::Checking ||
            state_ == UpdateState::Downloading || state_ == UpdateState::Verifying ||
            state_ == UpdateState::Installing) {
            return;
        }
        pendingCheck_ = true;
        userInitiatedCheck_ = userInitiated;
    }
    cv_.notify_all();
}

void UpdateService::requestDownload() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (state_ != UpdateState::UpdateAvailable && state_ != UpdateState::Error) return;
        if (!info_.available) return;
        if (info_.installMethod == InstallMethod::Manual) return;
        pendingDownload_ = true;
    }
    cv_.notify_all();
}

void UpdateService::requestInstall() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (state_ != UpdateState::Downloaded) return;
        pendingInstall_ = true;
    }
    cv_.notify_all();
}

void UpdateService::cancelInFlight() {
    cancelFlag_ = true;
    cv_.notify_all();
}

UpdateStatusSnapshot UpdateService::snapshot() const {
    std::lock_guard<std::mutex> lk(mtx_);
    UpdateStatusSnapshot s;
    s.state = state_;
    s.currentVersion = SATELLITE_VERSION;
    s.info = info_;
    s.bytesDownloaded = bytesDownloaded_;
    s.totalBytes = bytesTotal_;
    s.message = lastError_;
    s.failedPhase = failedPhase_;
    s.platformId = updater_.platformId();
    {
        std::lock_guard<std::mutex> ck(configMtx_);
        s.lastCheckEpoch = config_.lastCheckEpoch;
        s.channel = config_.updateChannel;
        s.autoCheck = config_.autoCheck;
        s.autoDownload = config_.autoDownload;
        s.autoInstall = config_.autoInstall;
    }
    return s;
}

void UpdateService::setStatusCallback(StatusCallback cb) {
    std::lock_guard<std::mutex> lk(mtx_);
    statusCb_ = std::move(cb);
}

void UpdateService::setPersistCallback(PersistCallback cb) {
    std::lock_guard<std::mutex> lk(mtx_);
    persistCb_ = std::move(cb);
}

void UpdateService::skipVersion(const std::string& version) {
    {
        std::lock_guard<std::mutex> ck(configMtx_);
        config_.skipVersion = version;
    }
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (info_.version == version) {
            info_ = {};
            state_ = UpdateState::Idle;
            lastError_.clear();
            failedPhase_ = UpdateState::Idle;
        }
    }
    if (persistCb_) persistCb_();
    fireBroadcast();
}

void UpdateService::dismiss() {
    {
        std::lock_guard<std::mutex> ck(configMtx_);
        config_.lastSeenVersion = info_.version;
    }
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (state_ == UpdateState::UpdateAvailable) state_ = UpdateState::Idle;
    }
    if (persistCb_) persistCb_();
    fireBroadcast();
}

void UpdateService::updatePreferences(const std::string& channel, bool autoCheck, bool autoDownload,
                                      bool autoInstall) {
    bool channelChanged = false;
    {
        std::lock_guard<std::mutex> ck(configMtx_);
        std::string ch = channel;
        if (ch != UPDATE_CHANNEL_STABLE && ch != UPDATE_CHANNEL_PRERELEASE) {
            ch = UPDATE_CHANNEL_STABLE;
        }
        channelChanged = (ch != config_.updateChannel);
        config_.updateChannel = ch;
        config_.autoCheck = autoCheck;
        config_.autoDownload = autoDownload;
        config_.autoInstall = autoInstall;
    }
    if (persistCb_) persistCb_();
    fireBroadcast();
    if (channelChanged) requestCheck(/*userInitiated=*/false);
}

void UpdateService::workerLoop() {
    while (!stopping_) {
        bool doCheckNow = false, doDownloadNow = false, doInstallNow = false;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [&] {
                return stopping_ || pendingCheck_ || pendingDownload_ || pendingInstall_;
            });
            if (stopping_) return;
            if (pendingInstall_) {
                pendingInstall_ = false;
                doInstallNow = true;
            } else if (pendingDownload_) {
                pendingDownload_ = false;
                doDownloadNow = true;
            } else if (pendingCheck_) {
                pendingCheck_ = false;
                doCheckNow = true;
            }
        }
        cancelFlag_ = false;
        if (doInstallNow) {
            doInstall();
        } else if (doDownloadNow) {
            doDownload();
            bool autoInstall = false;
            {
                std::lock_guard<std::mutex> ck(configMtx_);
                autoInstall = config_.autoInstall;
            }
            if (autoInstall) {
                std::lock_guard<std::mutex> lk(mtx_);
                if (state_ == UpdateState::Downloaded) pendingInstall_ = true;
            }
        } else if (doCheckNow) {
            doCheck(userInitiatedCheck_);
            bool autoDownload = false;
            {
                std::lock_guard<std::mutex> ck(configMtx_);
                autoDownload = config_.autoDownload;
            }
            if (autoDownload) {
                std::lock_guard<std::mutex> lk(mtx_);
                if (state_ == UpdateState::UpdateAvailable &&
                    info_.installMethod == InstallMethod::SelfInstall) {
                    pendingDownload_ = true;
                }
            }
        }
    }
}

void UpdateService::timerLoop() {
    using namespace std::chrono_literals;
    auto interruptibleSleep = [this](std::chrono::seconds d) {
        std::unique_lock<std::mutex> lk(timerMtx_);
        timerCv_.wait_for(lk, d, [this] { return stopping_.load(); });
    };
    interruptibleSleep(30s); // settle before the first periodic eval
    while (!stopping_) {
        bool shouldCheck = false;
        {
            std::lock_guard<std::mutex> ck(configMtx_);
            if (config_.autoCheck) {
                int64_t age = nowEpoch() - config_.lastCheckEpoch;
                int64_t intervalSec = static_cast<int64_t>(config_.updateCheckIntervalHours) * 3600;
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
    {
        std::lock_guard<std::mutex> lk(mtx_);
        snap.state = state_;
        snap.currentVersion = SATELLITE_VERSION;
        snap.info = info_;
        snap.bytesDownloaded = bytesDownloaded_;
        snap.totalBytes = bytesTotal_;
        snap.message = lastError_;
        snap.platformId = updater_.platformId();
        cb = statusCb_;
    }
    {
        std::lock_guard<std::mutex> ck(configMtx_);
        snap.lastCheckEpoch = config_.lastCheckEpoch;
        snap.channel = config_.updateChannel;
        snap.autoCheck = config_.autoCheck;
        snap.autoDownload = config_.autoDownload;
        snap.autoInstall = config_.autoInstall;
    }
    if (cb) cb(snap);
}

int64_t UpdateService::nowEpoch() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

bool UpdateService::versionStrictlyNewer(const std::string& a, const std::string& b) {
    return satellite::compareSemver(a, b) > 0;
}

void UpdateService::doCheck(bool userInitiated) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        state_ = UpdateState::Checking;
        lastError_.clear();
        failedPhase_ = UpdateState::Idle;
    }
    fireBroadcast();
    log_.logMsg(LogLevel::INFO, "updater",
                userInitiated ? "Manual update check started" : "Auto update check started");

    std::string channel, skipVer;
    {
        std::lock_guard<std::mutex> ck(configMtx_);
        channel = config_.updateChannel;
        skipVer = config_.skipVersion;
    }

    UpdateInfo info;
    std::string err;
    bool ok = updater_.fetchLatestRelease(channel, SATELLITE_VERSION, info, err);

    {
        std::lock_guard<std::mutex> ck(configMtx_);
        config_.lastCheckEpoch = nowEpoch();
    }
    if (persistCb_) persistCb_();

    if (!ok) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            state_ = UpdateState::Error;
            lastError_ = err.empty() ? "Update check failed (network or API error)" : err;
            failedPhase_ = UpdateState::Checking;
        }
        log_.logMsg(LogLevel::WARN, "updater", "Check failed: " + err);
        fireBroadcast();
        return;
    }

    info.available = versionStrictlyNewer(info.version, SATELLITE_VERSION);

    if (!info.available) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            state_ = UpdateState::UpToDate;
            info_ = info;
        }
        log_.logMsg(LogLevel::INFO, "updater",
                    "Up to date (current: " + std::string(SATELLITE_VERSION) +
                        ", latest: " + info.version + ")");
        fireBroadcast();
        return;
    }

    if (!skipVer.empty() && satellite::compareSemver(info.version, skipVer) <= 0) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            info.available = false;
            info_ = info;
            state_ = UpdateState::UpToDate;
        }
        log_.logMsg(LogLevel::INFO, "updater",
                    "Found " + info.version + " but skipVersion suppresses notification");
        fireBroadcast();
        return;
    }

    {
        std::lock_guard<std::mutex> lk(mtx_);
        info_ = info;
        state_ = UpdateState::UpdateAvailable;
    }
    log_.logMsg(LogLevel::INFO, "updater",
                "Update " + info.version + " available (" + info.assetName + ")");
    fireBroadcast();
}

void UpdateService::doDownload() {
    UpdateInfo info;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        info = info_;
        if (!info.available) {
            state_ = UpdateState::Idle;
            lastError_ = "No update to download";
            fireBroadcast();
            return;
        }
        if (info.installMethod == InstallMethod::Manual) {
            state_ = UpdateState::UpdateAvailable;
            fireBroadcast();
            return;
        }
        state_ = UpdateState::Downloading;
        lastError_.clear();
        failedPhase_ = UpdateState::Idle;
        bytesDownloaded_ = 0;
        bytesTotal_ = info.assetSize;
        cancelFlag_ = false;
    }
    fireBroadcast();
    log_.logMsg(LogLevel::INFO, "updater", "Downloading " + info.assetName);

    auto onProgress = [this](uint64_t soFar, uint64_t total) {
        bool shouldBroadcast = false;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (total > 0) {
                uint64_t prevPct = bytesTotal_ > 0 ? (bytesDownloaded_ * 100) / bytesTotal_ : 0;
                uint64_t newPct = (soFar * 100) / total;
                if (newPct != prevPct) shouldBroadcast = true;
            } else {
                if (soFar - bytesDownloaded_ >= 256 * 1024) shouldBroadcast = true;
            }
            bytesDownloaded_ = soFar;
            bytesTotal_ = total > 0 ? total : bytesTotal_;
        }
        if (shouldBroadcast) fireBroadcast();
    };

    std::string localPath, err;
    bool ok = updater_.downloadArtifact(info, onProgress, &cancelFlag_, localPath, err);
    if (!ok) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            state_ = UpdateState::Error;
            lastError_ = err.empty() ? "Download failed" : err;
            failedPhase_ = UpdateState::Downloading;
        }
        log_.logMsg(LogLevel::WARN, "updater", "Download failed: " + err);
        fireBroadcast();
        return;
    }

    {
        std::lock_guard<std::mutex> lk(mtx_);
        downloadedPath_ = localPath;
        state_ = UpdateState::Verifying;
    }
    fireBroadcast();
    log_.logMsg(LogLevel::INFO, "updater", "Verifying " + localPath);
    doVerify();
}

void UpdateService::doVerify() {
    UpdateInfo info;
    std::string localPath;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        info = info_;
        localPath = downloadedPath_;
    }
    std::string err;
    bool ok = updater_.verifyArtifact(localPath, info, err);
    if (!ok) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            state_ = UpdateState::Error;
            lastError_ = err.empty() ? "Signature/checksum verification failed" : err;
            failedPhase_ = UpdateState::Verifying;
        }
        log_.logMsg(LogLevel::ERR, "updater", "Verify failed: " + err);
        fireBroadcast();
        return;
    }
    {
        std::lock_guard<std::mutex> lk(mtx_);
        state_ = UpdateState::Downloaded;
    }
    log_.logMsg(LogLevel::INFO, "updater", "Update " + info.version + " ready to install");
    fireBroadcast();
}

void UpdateService::doInstall() {
    UpdateInfo info;
    std::string localPath;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (state_ != UpdateState::Downloaded) return;
        info = info_;
        localPath = downloadedPath_;
        state_ = UpdateState::Installing;
    }
    fireBroadcast();
    log_.logMsg(LogLevel::INFO, "updater",
                "Launching installer for " + info.version + " (" + info.assetName + ")");
    std::string err;
    bool ok = updater_.applyUpdate(localPath, info, err);
    if (!ok) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
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
