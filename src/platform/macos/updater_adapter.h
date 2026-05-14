// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * updater_adapter.h — IUpdaterPort implementation for macOS.
 *
 * Fetches release metadata + artifact via NSURLSession (TLS via the
 * system trust store), verifies SHA-256 via CommonCrypto, then runs
 * a small POSIX swap-and-relaunch helper:
 *
 *   1. Unzip satellite-macos-stub-vX.Y.Z.zip into a staging dir.
 *   2. Write a /tmp/satellite-update-XXXX.sh helper that:
 *        a) waits for our PID to exit,
 *        b) atomically mv(1)s the new bundle into place,
 *        c) re-opens satellite.app via `open`.
 *   3. fork+exec the helper, NSApp terminate ourselves.
 *
 * Out of scope here: codesign/notarization validation. We rely on the
 * release pipeline producing notarized bundles when secrets are set;
 * the SHA-256 check guards against tampering in transit.
 */
#pragma once

#include "core/ports.h"

#include <string>

class MacOSUpdaterAdapter : public IUpdaterPort {
  public:
    MacOSUpdaterAdapter(std::string githubOwner, std::string githubRepo);

    bool fetchLatestRelease(const std::string& channel, const std::string& currentVersion,
                            UpdateInfo& out, std::string& outError) override;

    bool downloadArtifact(const UpdateInfo& info,
                          const std::function<void(uint64_t, uint64_t)>& onProgress,
                          const std::atomic<bool>* cancel, std::string& outLocalPath,
                          std::string& outError) override;

    bool verifyArtifact(const std::string& localPath, const UpdateInfo& info,
                        std::string& outError) override;

    bool applyUpdate(const std::string& localPath, const UpdateInfo& info,
                     std::string& outError) override;

    std::string platformId() const override { return "macos"; }

  private:
    std::string owner_;
    std::string repo_;
};
