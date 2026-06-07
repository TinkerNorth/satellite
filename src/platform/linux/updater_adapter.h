// SPDX-License-Identifier: LGPL-3.0-or-later
// Only AppImage self-installs (swap-and-re-exec); deb/rpm/aur are Manual because
// self-replacing would break the package manager's bookkeeping. Install type is
// detected at construction and drives platformId(). See docs/architecture.md.
#pragma once

#include "core/ports.h"

#include <string>

class LinuxUpdaterAdapter : public IUpdaterPort {
  public:
    LinuxUpdaterAdapter(std::string githubOwner, std::string githubRepo);

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

    std::string platformId() const override { return platformId_; }

  private:
    enum class InstallType { AppImage, Deb, Rpm, Aur, Portable };
    InstallType detectInstallType() const;

    std::string owner_;
    std::string repo_;
    InstallType installType_;
    std::string platformId_;
};
