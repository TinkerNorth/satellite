// SPDX-License-Identifier: LGPL-3.0-or-later
#include "../src/core/update_service.h"
#include "../src/core/ports.h"
#include "../src/core/types.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

static int g_pass = 0;
static int g_fail = 0;
static std::string g_currentTest;

#define TEST(name)                                                                                 \
    do { g_currentTest = (name); } while (0)

#define EXPECT(cond)                                                                               \
    do {                                                                                           \
        if (cond) {                                                                                \
            g_pass++;                                                                              \
        } else {                                                                                   \
            g_fail++;                                                                              \
            std::cerr << "  FAIL [" << g_currentTest << "] " << __FILE__ << ":" << __LINE__        \
                      << "  " << #cond << "\n";                                                    \
        }                                                                                          \
    } while (0)

struct MockUpdater : IUpdaterPort {
    // Fetch knobs.
    bool fetchOk = true;
    std::string fetchVersion = "99.0.0"; // newer than SATELLITE_VERSION
    std::string fetchError;
    InstallMethod method = InstallMethod::SelfInstall;
    uint64_t assetSize = 1024;

    // Downstream phase knobs.
    bool downloadOk = true;
    bool verifyOk = true;
    bool applyOk = true;
    bool blockDownloadUntilCancel = false;

    int fetchCalls = 0;

    bool fetchLatestRelease(const std::string& channel, const std::string& /*currentVersion*/,
                            UpdateInfo& out, std::string& outError) override {
        fetchCalls++;
        if (!fetchOk) {
            outError = fetchError;
            return false;
        }
        out.version = fetchVersion;
        out.channel = channel;
        out.assetName = "SatelliteSetup-v" + fetchVersion + ".exe";
        out.assetSize = assetSize;
        out.installMethod = method;
        return true;
    }
    bool downloadArtifact(const UpdateInfo& /*info*/,
                          const std::function<void(uint64_t, uint64_t)>& onProgress,
                          const std::atomic<bool>* cancel, std::string& outLocalPath,
                          std::string& outError) override {
        if (blockDownloadUntilCancel) {
            for (int i = 0; i < 3000; i++) {
                if (cancel && cancel->load()) {
                    outError = "cancelled";
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        onProgress(assetSize, assetSize);
        if (!downloadOk) {
            outError = "download boom";
            return false;
        }
        outLocalPath = "/tmp/satellite-update";
        return true;
    }
    bool verifyArtifact(const std::string& /*localPath*/, const UpdateInfo& /*info*/,
                        std::string& outError) override {
        if (!verifyOk) {
            outError = "checksum mismatch";
            return false;
        }
        return true;
    }
    bool applyUpdate(const std::string& /*localPath*/, const UpdateInfo& /*info*/,
                     std::string& outError) override {
        if (!applyOk) {
            outError = "launch failed";
            return false;
        }
        return true;
    }
    std::string platformId() const override { return "test"; }
};

struct MockLog : ILogPort {
    void logMsg(LogLevel, const std::string&, const std::string&) override {}
};

static bool waitForState(UpdateService& svc, UpdateState want, int timeoutMs = 3000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (svc.snapshot().state == want) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

static void test_semver_compare() {
    TEST("versionStrictlyNewer: numeric core comparison");
    EXPECT(UpdateService::versionStrictlyNewer("1.0.1", "1.0.0"));
    EXPECT(UpdateService::versionStrictlyNewer("1.1.0", "1.0.9"));
    EXPECT(UpdateService::versionStrictlyNewer("2.0.0", "1.9.9"));
    EXPECT(!UpdateService::versionStrictlyNewer("1.0.0", "1.0.0"));
    EXPECT(!UpdateService::versionStrictlyNewer("1.0.0", "1.0.1"));
    EXPECT(!UpdateService::versionStrictlyNewer("0.9.9", "1.0.0"));
}

static void test_semver_prerelease() {
    TEST("versionStrictlyNewer: a release outranks the same-core prerelease");
    EXPECT(UpdateService::versionStrictlyNewer("1.2.0", "1.2.0-rc.1"));
    EXPECT(!UpdateService::versionStrictlyNewer("1.2.0-rc.1", "1.2.0"));
    EXPECT(UpdateService::versionStrictlyNewer("1.2.0-rc.2", "1.2.0-rc.1"));
    EXPECT(!UpdateService::versionStrictlyNewer("1.2.0-rc.1", "1.2.0-rc.1"));
}

static void test_semver_malformed() {
    TEST("versionStrictlyNewer: unparseable components treated as 0");
    EXPECT(!UpdateService::versionStrictlyNewer("", ""));
    EXPECT(!UpdateService::versionStrictlyNewer("abc", "x.y.z"));
    EXPECT(UpdateService::versionStrictlyNewer("1", "0"));
    EXPECT(UpdateService::versionStrictlyNewer("1.0.0", "")); // "" parses to 0.0.0
}

static void test_check_finds_update() {
    TEST("check: newer release transitions to UpdateAvailable with populated info");
    MockUpdater up;
    MockLog log;
    Config cfg;
    std::mutex cfgMtx;
    UpdateService svc(up, log, cfg, cfgMtx);
    svc.start();
    svc.requestCheck(true);
    EXPECT(waitForState(svc, UpdateState::UpdateAvailable));
    auto snap = svc.snapshot();
    EXPECT(snap.info.available);
    EXPECT(snap.info.version == "99.0.0");
    EXPECT(up.fetchCalls == 1);
}

static void test_check_up_to_date() {
    TEST("check: a non-newer release transitions to UpToDate");
    MockUpdater up;
    up.fetchVersion = "0.0.1";
    MockLog log;
    Config cfg;
    std::mutex cfgMtx;
    UpdateService svc(up, log, cfg, cfgMtx);
    svc.start();
    svc.requestCheck(false);
    EXPECT(waitForState(svc, UpdateState::UpToDate));
    EXPECT(!svc.snapshot().info.available);
}

static void test_check_network_error() {
    TEST("check: fetch failure transitions to Error with failedPhase=Checking");
    MockUpdater up;
    up.fetchOk = false;
    up.fetchError = "no network";
    MockLog log;
    Config cfg;
    std::mutex cfgMtx;
    UpdateService svc(up, log, cfg, cfgMtx);
    svc.start();
    svc.requestCheck(true);
    EXPECT(waitForState(svc, UpdateState::Error));
    auto snap = svc.snapshot();
    EXPECT(snap.failedPhase == UpdateState::Checking);
    EXPECT(snap.message == "no network");
}

static void test_check_skip_version_suppresses() {
    TEST("check: skipVersion at/above the found version suppresses the notification");
    MockUpdater up;
    up.fetchVersion = "99.0.0";
    MockLog log;
    Config cfg;
    cfg.skipVersion = "99.0.0";
    std::mutex cfgMtx;
    UpdateService svc(up, log, cfg, cfgMtx);
    svc.start();
    svc.requestCheck(false);
    EXPECT(waitForState(svc, UpdateState::UpToDate));
    EXPECT(!svc.snapshot().info.available);
}

static void test_full_install_flow() {
    TEST("download→verify→install: happy path reaches Installing");
    MockUpdater up;
    MockLog log;
    Config cfg;
    std::mutex cfgMtx;
    UpdateService svc(up, log, cfg, cfgMtx);
    svc.start();
    svc.requestCheck(true);
    EXPECT(waitForState(svc, UpdateState::UpdateAvailable));
    svc.requestDownload();
    EXPECT(waitForState(svc, UpdateState::Downloaded));
    svc.requestInstall();
    EXPECT(waitForState(svc, UpdateState::Installing));
}

static void test_download_failure() {
    TEST("download: failure transitions to Error with failedPhase=Downloading");
    MockUpdater up;
    up.downloadOk = false;
    MockLog log;
    Config cfg;
    std::mutex cfgMtx;
    UpdateService svc(up, log, cfg, cfgMtx);
    svc.start();
    svc.requestCheck(true);
    EXPECT(waitForState(svc, UpdateState::UpdateAvailable));
    svc.requestDownload();
    EXPECT(waitForState(svc, UpdateState::Error));
    EXPECT(svc.snapshot().failedPhase == UpdateState::Downloading);
}

static void test_verify_failure() {
    TEST("verify: checksum failure transitions to Error with failedPhase=Verifying");
    MockUpdater up;
    up.verifyOk = false;
    MockLog log;
    Config cfg;
    std::mutex cfgMtx;
    UpdateService svc(up, log, cfg, cfgMtx);
    svc.start();
    svc.requestCheck(true);
    EXPECT(waitForState(svc, UpdateState::UpdateAvailable));
    svc.requestDownload();
    EXPECT(waitForState(svc, UpdateState::Error));
    EXPECT(svc.snapshot().failedPhase == UpdateState::Verifying);
}

static void test_cancel_in_flight() {
    TEST("cancelInFlight: aborts an in-progress download into Error");
    MockUpdater up;
    up.blockDownloadUntilCancel = true;
    MockLog log;
    Config cfg;
    std::mutex cfgMtx;
    UpdateService svc(up, log, cfg, cfgMtx);
    svc.start();
    svc.requestCheck(true);
    EXPECT(waitForState(svc, UpdateState::UpdateAvailable));
    svc.requestDownload();
    EXPECT(waitForState(svc, UpdateState::Downloading));
    svc.cancelInFlight();
    EXPECT(waitForState(svc, UpdateState::Error));
    EXPECT(svc.snapshot().message == "cancelled");
}

static void test_manual_method_does_not_download() {
    TEST("requestDownload: Manual install method is ignored (no SelfInstall download)");
    MockUpdater up;
    up.method = InstallMethod::Manual;
    MockLog log;
    Config cfg;
    std::mutex cfgMtx;
    UpdateService svc(up, log, cfg, cfgMtx);
    svc.start();
    svc.requestCheck(true);
    EXPECT(waitForState(svc, UpdateState::UpdateAvailable));
    svc.requestDownload();
    // Stays UpdateAvailable; give the worker a beat to (not) act.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT(svc.snapshot().state == UpdateState::UpdateAvailable);
}

static void test_auto_download_and_install_chain() {
    TEST("auto flags: check chains straight through to Installing");
    MockUpdater up;
    MockLog log;
    Config cfg;
    cfg.autoDownload = true;
    cfg.autoInstall = true;
    std::mutex cfgMtx;
    UpdateService svc(up, log, cfg, cfgMtx);
    svc.start();
    svc.requestCheck(false);
    EXPECT(waitForState(svc, UpdateState::Installing));
}

static void test_update_preferences_persists() {
    TEST("updatePreferences: writes config, clamps bad channel, fires persist callback");
    MockUpdater up;
    MockLog log;
    Config cfg;
    std::mutex cfgMtx;
    UpdateService svc(up, log, cfg, cfgMtx);
    int persistCalls = 0;
    svc.setPersistCallback([&] { persistCalls++; });

    svc.updatePreferences("prerelease", true, true, false);
    EXPECT(cfg.updateChannel == "prerelease");
    EXPECT(cfg.autoCheck && cfg.autoDownload && !cfg.autoInstall);
    EXPECT(persistCalls >= 1);

    svc.updatePreferences("garbage", false, false, false); // clamps to stable
    EXPECT(cfg.updateChannel == std::string(UPDATE_CHANNEL_STABLE));
}

static void test_skip_version_clears_available() {
    TEST("skipVersion: matching the available version clears back to Idle + persists");
    MockUpdater up;
    MockLog log;
    Config cfg;
    std::mutex cfgMtx;
    UpdateService svc(up, log, cfg, cfgMtx);
    int persistCalls = 0;
    svc.setPersistCallback([&] { persistCalls++; });
    svc.start();
    svc.requestCheck(true);
    EXPECT(waitForState(svc, UpdateState::UpdateAvailable));

    svc.skipVersion("99.0.0");
    EXPECT(cfg.skipVersion == "99.0.0");
    EXPECT(waitForState(svc, UpdateState::Idle));
    EXPECT(persistCalls >= 1);
}

static void test_dismiss_clears_available() {
    TEST("dismiss: records lastSeenVersion and drops UpdateAvailable to Idle");
    MockUpdater up;
    MockLog log;
    Config cfg;
    std::mutex cfgMtx;
    UpdateService svc(up, log, cfg, cfgMtx);
    svc.start();
    svc.requestCheck(true);
    EXPECT(waitForState(svc, UpdateState::UpdateAvailable));
    svc.dismiss();
    EXPECT(cfg.lastSeenVersion == "99.0.0");
    EXPECT(waitForState(svc, UpdateState::Idle));
}

int main() {
    std::cout << "Running update_service tests...\n\n";
    test_semver_compare();
    test_semver_prerelease();
    test_semver_malformed();
    test_check_finds_update();
    test_check_up_to_date();
    test_check_network_error();
    test_check_skip_version_suppresses();
    test_full_install_flow();
    test_download_failure();
    test_verify_failure();
    test_cancel_in_flight();
    test_manual_method_does_not_download();
    test_auto_download_and_install_chain();
    test_update_preferences_persists();
    test_skip_version_clears_available();
    test_dismiss_clears_available();

    std::cout << "\n=== Test Results ===\n";
    std::cout << "  Passed: " << g_pass << "\n";
    std::cout << "  Failed: " << g_fail << "\n";
    if (g_fail > 0) {
        std::cout << "  STATUS: FAIL\n";
        return 1;
    }
    std::cout << "  STATUS: ALL PASSED\n";
    return 0;
}
