// SPDX-License-Identifier: LGPL-3.0-or-later
// IUpdaterPort for macOS: fetch release metadata + artifact via NSURLSession,
// verify SHA-256 via CommonCrypto, then a POSIX swap-and-relaunch helper does
// the in-place .app update. No codesign/notarization validation here — we rely
// on the release pipeline producing notarized bundles; the SHA-256 check only
// guards against tampering in transit.
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
