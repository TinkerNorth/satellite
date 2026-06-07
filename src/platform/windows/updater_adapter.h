// SPDX-License-Identifier: LGPL-3.0-or-later
// IUpdaterPort for Windows: WinHTTP to GitHub, BCrypt SHA-256, ShellExecuteEx
// to run the Inno installer. An /OTA install auto-relaunches satellite.exe at
// the end even in silent mode (see installer.iss WantsOTARelaunch).
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
