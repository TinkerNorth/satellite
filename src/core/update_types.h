// SPDX-License-Identifier: LGPL-3.0-or-later

// Pure domain types for the OTA update flow.
#pragma once

#include <cstdint>
#include <string>

// Error carries the failed phase so the UI can show which step failed.
// Retrying from any error returns the machine to Idle.
enum class UpdateState : uint8_t {
    Idle = 0,
    Checking,
    UpToDate,
    UpdateAvailable,
    Downloading,
    Verifying,
    Downloaded,
    Installing,
    Error,
};

inline const char* updateStateName(UpdateState s) {
    switch (s) {
    case UpdateState::Idle:
        return "idle";
    case UpdateState::Checking:
        return "checking";
    case UpdateState::UpToDate:
        return "up-to-date";
    case UpdateState::UpdateAvailable:
        return "update-available";
    case UpdateState::Downloading:
        return "downloading";
    case UpdateState::Verifying:
        return "verifying";
    case UpdateState::Downloaded:
        return "downloaded";
    case UpdateState::Installing:
        return "installing";
    case UpdateState::Error:
        return "error";
    }
    return "unknown";
}

// SelfInstall: updater downloads + runs the platform installer, exits,
// relaunches. Manual: surface an external instruction (e.g. `apt upgrade`);
// applyUpdate is a no-op.
enum class InstallMethod : uint8_t {
    SelfInstall = 0,
    Manual = 1,
};

// Populated by IUpdaterPort::fetchLatestRelease().
struct UpdateInfo {
    bool available = false; // strictly newer than current
    std::string version;    // no leading v
    std::string channel;
    std::string assetName;
    std::string assetUrl;
    uint64_t assetSize = 0;
    std::string assetSha256;  // hex; "" if SHA256SUMS missing/skipped
    std::string releaseNotes; // truncated markdown body (<= 8 KB)
    std::string htmlUrl;
    int64_t publishedAtEpoch = 0; // unix seconds
    InstallMethod installMethod = InstallMethod::SelfInstall;
    std::string manualInstruction; // shell command, when installMethod=Manual
};

// Rendered as JSON by the webserver and broadcast on the SSE channel.
struct UpdateStatusSnapshot {
    UpdateState state = UpdateState::Idle;
    std::string currentVersion;
    UpdateInfo info; // valid when state >= UpdateAvailable

    uint64_t bytesDownloaded = 0;
    uint64_t totalBytes = 0;

    // User-facing, never a stack trace.
    std::string message;

    // Phase running when it hit Error. Idle when state != Error.
    UpdateState failedPhase = UpdateState::Idle;

    // Running value, may be ahead of persisted config until the next saveConfig().
    int64_t lastCheckEpoch = 0;

    std::string channel;
    bool autoCheck = true;
    bool autoDownload = false;
    bool autoInstall = false;

    std::string platformId;
};
