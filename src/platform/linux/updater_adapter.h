// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * updater_adapter.h — IUpdaterPort implementation for Linux.
 *
 * Linux has several install methods that map to different update flows:
 *
 *   AppImage (platformId "linux-appimage"):
 *     The binary itself is the AppImage and lives at $APPIMAGE. We
 *     download the new AppImage, chmod +x, then swap with the running
 *     one via a tiny detached shell helper that waits for our PID to
 *     exit and re-execs. SelfInstall path.
 *
 *   Debian package (platformId "linux-deb"):
 *     The binary is owned by dpkg/apt. We surface a manual instruction
 *     ("sudo apt update && sudo apt upgrade satellite") and do NOT
 *     attempt to self-replace — that would break apt's bookkeeping.
 *     Manual path. Users who added our APT repo (see
 *     packaging/repo/README.md) get auto-updates from `apt upgrade`
 *     and never see the in-app prompt at all once they've upgraded.
 *
 *   RPM package (platformId "linux-rpm"):
 *     Same shape as Deb, with `sudo dnf upgrade satellite` as the
 *     manual instruction. Detected via `rpm -qf /proc/self/exe` (cheap,
 *     no shell-out — we just stat $RPM_INSTALL_PATH-style locations).
 *
 *   AUR package (platformId "linux-aur"):
 *     The Arch satellite-bin AUR package wraps the AppImage under
 *     /opt/satellite and shims /usr/bin/satellite to it. Detected via
 *     the shim path. Manual instruction is "yay -Syu satellite-bin".
 *
 *   Portable / built-from-source (platformId "linux-portable"):
 *     We have no idea where the binary came from. Manual path with a
 *     "download from github.com/.../releases" instruction.
 *
 * Detection runs at construction time and is exposed via platformId().
 * HTTPS is provided by libcurl, which is linked through pkg-config.
 */
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
