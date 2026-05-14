// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * updater_adapter.h — IUpdaterPort implementation for Windows.
 *
 * Backed by:
 *   - WinHTTP for HTTPS to api.github.com and the GitHub Releases CDN.
 *   - BCrypt for SHA-256 verification of the downloaded installer.
 *   - ShellExecuteEx to launch SatelliteSetup-vX.Y.Z.exe with
 *     /VERYSILENT /OTA, then PostQuitMessage so our process exits
 *     cleanly and the installer can replace the .exe.
 *
 * The bundled installer's [Run] section is augmented (see installer.iss
 * → WantsOTARelaunch check) so that an /OTA install auto-relaunches
 * satellite.exe at the end, even in silent mode.
 */
#pragma once

#include "core/ports.h"

#include <string>

class WindowsUpdaterAdapter : public IUpdaterPort {
  public:
    WindowsUpdaterAdapter(std::string githubOwner, std::string githubRepo);

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

    std::string platformId() const override { return "windows"; }

  private:
    std::string owner_;
    std::string repo_;
};
