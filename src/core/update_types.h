// SPDX-License-Identifier: LGPL-3.0-or-later

// Pure domain types for the OTA update flow.
#pragma once

#include <cstdint>
#include <string>

// Error is sticky: it carries the message and the failed phase so the UI can
// show "we were Downloading and it failed: <reason>". Retrying from any error
// returns the machine to Idle.
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

// SelfInstall — updater downloads + runs the platform installer, exits,
//   relaunches. Manual — surface an external instruction (e.g. `apt upgrade`);
//   applyUpdate is a no-op.
enum class InstallMethod : uint8_t {
    SelfInstall = 0,
    Manual = 1,
};

// Populated by IUpdaterPort::fetchLatestRelease().
struct UpdateInfo {
    bool available = false;       // strictly newer than current
    std::string version;          // e.g. "1.2.3"  (no leading v)
    std::string channel;          // "stable" | "prerelease"
    std::string assetName;        // e.g. "SatelliteSetup-v1.2.3.exe"
    std::string assetUrl;         // direct download URL
    uint64_t assetSize = 0;       // bytes (from GitHub asset metadata)
    std::string assetSha256;      // hex; "" if SHA256SUMS missing/skipped
    std::string releaseNotes;     // truncated markdown body (<= 8 KB)
    std::string htmlUrl;          // link to release page on github.com
    int64_t publishedAtEpoch = 0; // unix seconds
    InstallMethod installMethod = InstallMethod::SelfInstall;
    std::string manualInstruction; // shell command, when installMethod=Manual
};

// Rendered as JSON by the webserver and broadcast on the SSE channel.
struct UpdateStatusSnapshot {
    UpdateState state = UpdateState::Idle;
    std::string currentVersion; // SATELLITE_VERSION
    UpdateInfo info;            // valid when state >= UpdateAvailable

    // Download progress (Downloading + Verifying states).
    uint64_t bytesDownloaded = 0;
    uint64_t totalBytes = 0;

    // User-facing; populated for Error / optionally Installing. Never a stack trace.
    std::string message;

    // The phase running when it hit Error, so the UI can tell "couldn't reach
    // the server" from download/verify/install failures. Idle when state != Error.
    UpdateState failedPhase = UpdateState::Idle;

    // Mirrors Config.lastCheckEpoch but reflects the running value (may be ahead
    // of the persisted config until the next saveConfig()).
    int64_t lastCheckEpoch = 0;

    // User pref snapshot, so the UI needs no separate GET.
    std::string channel;
    bool autoCheck = true;
    bool autoDownload = false;
    bool autoInstall = false;

    std::string platformId; // mirrors IUpdaterPort::platformId()
};
