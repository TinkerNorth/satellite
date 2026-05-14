// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * core/update_types.h — Pure domain types for the OTA update flow.
 *
 * No platform headers, no networking — just the data the update state
 * machine moves around between IUpdaterPort, UpdateService, and the
 * webserver / tray UIs.
 */
#pragma once

#include <cstdint>
#include <string>

// ── State machine ───────────────────────────────────────────────────────────
// Linear with one branch:
//
//   Idle ──check──► Checking ──► UpToDate ──┐
//                         │                 │
//                         ▼                 ▼
//                   UpdateAvailable ──► (auto-download? Downloading ─►
//                         │                 │     Verifying ─► Downloaded)
//                         │                 ▼
//                         └──┬──► Downloading ─► Verifying ─► Downloaded
//                            │                                   │
//                            ▼                                   ▼
//                          Error                              Installing ─► (exit)
//
// Error is sticky: it carries the message and the previous state, so the
// UI can show "we were Downloading and it failed: <reason>". The user
// can retry from any error state, returning the machine to Idle.
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

// ── Install method ──────────────────────────────────────────────────────────
// SelfInstall — the updater takes over: download artifact, run platform
//   installer (Inno Setup /VERYSILENT on Windows; bundle swap on macOS;
//   AppImage replace-in-place on Linux), exit, relaunch.
// Manual      — surface an external instruction to the user (e.g. `apt
//   upgrade satellite` for .deb builds). applyUpdate is a no-op.
enum class InstallMethod : uint8_t {
    SelfInstall = 0,
    Manual = 1,
};

// ── Release / artifact descriptor ───────────────────────────────────────────
// Populated by IUpdaterPort::fetchLatestRelease(). Carries everything the
// UI needs to render a "v1.2.3 — release notes — 4.2 MB" prompt and
// everything the downloader needs to fetch the right asset.
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

// ── Snapshot delivered to the UI ────────────────────────────────────────────
// One immutable struct the webserver can render as JSON and the SSE
// channel can broadcast on every state transition.
struct UpdateStatusSnapshot {
    UpdateState state = UpdateState::Idle;
    std::string currentVersion; // SATELLITE_VERSION
    UpdateInfo info;            // valid when state >= UpdateAvailable

    // Download progress (Downloading + Verifying states).
    uint64_t bytesDownloaded = 0;
    uint64_t totalBytes = 0;

    // User-facing message — populated for Error, optionally for Installing
    // ("Restarting in 3 seconds…"). Never carries a stack trace.
    std::string message;

    // Most-recent successful check, unix seconds. Mirrors Config.lastCheckEpoch
    // but reflects the *running* service value (which may be ahead of the
    // persisted config until the next saveConfig()).
    int64_t lastCheckEpoch = 0;

    // User pref snapshot — surfaced so the UI doesn't need a separate GET.
    std::string channel;
    bool autoCheck = true;
    bool autoDownload = false;
    bool autoInstall = false;

    // Identification of the running binary's install method. Determines
    // whether the "Install now" affordance is shown vs the manual-command
    // copy-button. Mirrors IUpdaterPort::platformId().
    std::string platformId;
};
