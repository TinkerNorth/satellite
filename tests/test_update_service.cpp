// SPDX-License-Identifier: LGPL-3.0-or-later
#include "../src/core/update_service.h"
#include "../src/core/ports.h"
#include "../src/core/types.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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
    std::string assetSha256 = "5ha256";
    std::atomic<bool> fetchGateOpen{true};
    std::atomic<bool> fetching{false};

    // Downstream phase knobs.
    bool downloadOk = true;
    bool verifyOk = true;
    bool applyOk = true;
    bool blockDownloadUntilCancel = false;
    std::atomic<bool> verifyGateOpen{true};
    std::atomic<bool> verifying{false};

    std::atomic<int> fetchCalls{0};
    std::atomic<int> downloadCalls{0};
    std::atomic<int> verifyCalls{0};
    std::atomic<int> applyCalls{0};
    std::string lastAppliedPath;
    std::string calls;

    bool fetchLatestRelease(const std::string& channel, const std::string& /*currentVersion*/,
                            UpdateInfo& out, std::string& outError) override {
        fetchCalls++;
        calls += "fetch ";
        fetching = true;
        while (!fetchGateOpen.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (!fetchOk) {
            outError = fetchError;
            return false;
        }
        out.version = fetchVersion;
        out.channel = channel;
        out.assetName = "SatelliteSetup-" + fetchVersion + ".exe";
        out.assetSize = assetSize;
        out.assetSha256 = assetSha256;
        out.installMethod = method;
        return true;
    }
    bool downloadArtifact(const UpdateInfo& /*info*/,
                          const std::function<void(uint64_t, uint64_t)>& onProgress,
                          const std::atomic<bool>* cancel, std::string& outLocalPath,
                          std::string& outError) override {
        downloadCalls++;
        calls += "download ";
        if (cancel && cancel->load()) {
            outError = "cancelled";
            return false;
        }
        if (blockDownloadUntilCancel) {
            onProgress(assetSize / 2, assetSize);
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
        verifyCalls++;
        calls += "verify ";
        verifying = true;
        while (!verifyGateOpen.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (!verifyOk) {
            outError = "checksum mismatch";
            return false;
        }
        return true;
    }
    bool applyUpdate(const std::string& localPath, const UpdateInfo& /*info*/,
                     std::string& outError) override {
        lastAppliedPath = localPath;
        applyCalls++;
        calls += "apply ";
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

struct RecordingLog : ILogPort {
    std::mutex mtx;
    std::vector<std::string> lines;
    void logMsg(LogLevel, const std::string&, const std::string& message) override {
        std::lock_guard<std::mutex> lk(mtx);
        lines.push_back(message);
    }
};

static bool logged(RecordingLog& log, const std::string& line) {
    std::lock_guard<std::mutex> lk(log.mtx);
    return std::find(log.lines.begin(), log.lines.end(), line) != log.lines.end();
}

struct BroadcastLog {
    std::mutex mtx;
    int count = 0;
    UpdateStatusSnapshot last;
    std::vector<UpdateState> states;
};

static void recordBroadcast(BroadcastLog& broadcasts, const UpdateStatusSnapshot& snap) {
    std::lock_guard<std::mutex> lk(broadcasts.mtx);
    broadcasts.count++;
    broadcasts.last = snap;
    broadcasts.states.push_back(snap.state);
}

static size_t broadcastMark(BroadcastLog& broadcasts) {
    std::lock_guard<std::mutex> lk(broadcasts.mtx);
    return broadcasts.states.size();
}

static bool broadcastSince(BroadcastLog& broadcasts, size_t mark, UpdateState state) {
    std::lock_guard<std::mutex> lk(broadcasts.mtx);
    const auto from = broadcasts.states.begin() + static_cast<std::ptrdiff_t>(mark);
    return std::find(from, broadcasts.states.end(), state) != broadcasts.states.end();
}

static int broadcastCount(BroadcastLog& broadcasts) {
    std::lock_guard<std::mutex> lk(broadcasts.mtx);
    return broadcasts.count;
}

static UpdateStatusSnapshot lastBroadcast(BroadcastLog& broadcasts) {
    std::lock_guard<std::mutex> lk(broadcasts.mtx);
    return broadcasts.last;
}

struct Rig {
    MockUpdater up;
    RecordingLog log;
    Config cfg;
    std::mutex cfgMtx;
    BroadcastLog broadcasts;
    std::atomic<int> persistCalls{0};
    UpdateService svc{up, log, cfg, cfgMtx};

    Rig() {
        svc.setStatusCallback(
            [this](const UpdateStatusSnapshot& snap) { recordBroadcast(broadcasts, snap); });
        svc.setPersistCallback([this] { persistCalls++; });
    }
};

struct WorkerHold {
    std::vector<UpdateState> at;
    std::atomic<bool> held{false};
    std::atomic<bool> released{false};
};

static void holdWorker(WorkerHold& hold, const UpdateStatusSnapshot& snap) {
    const bool matches = std::find(hold.at.begin(), hold.at.end(), snap.state) != hold.at.end();
    if (!matches || hold.held.exchange(true)) return;
    while (!hold.released.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

static void holdAt(Rig& rig, WorkerHold& hold, std::vector<UpdateState> at) {
    hold.at = std::move(at);
    rig.svc.setStatusCallback(
        [&hold](const UpdateStatusSnapshot& snap) { holdWorker(hold, snap); });
}

struct DownloadOnOffer {
    UpdateService* svc = nullptr;
    std::atomic<bool> armed{false};
};

static void downloadOnOffer(DownloadOnOffer& offer, const UpdateStatusSnapshot& snap) {
    if (snap.state != UpdateState::UpdateAvailable || !offer.armed.exchange(false)) return;
    offer.svc->requestDownload();
}

static bool waitForFlag(const std::atomic<bool>& flag) {
    for (int i = 0; i < 3000 && !flag.load(); i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return flag.load();
}

static bool waitForState(UpdateService& svc, UpdateState want, int timeoutMs = 3000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (svc.snapshot().state == want) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

static bool waitForCount(const std::atomic<int>& counter, int want) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
    while (std::chrono::steady_clock::now() < deadline) {
        if (counter.load() >= want) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

static bool frozenAt(Rig& rig, UpdateState want) {
    const bool reached = waitForState(rig.svc, want);
    rig.svc.stop();
    return reached;
}

static bool freezeAfterCheck(Rig& rig, UpdateState want) {
    rig.svc.start();
    rig.svc.requestCheck(true);
    return frozenAt(rig, want);
}

static bool freezeAfterDownload(Rig& rig, UpdateState want) {
    rig.svc.start();
    rig.svc.requestCheck(true);
    const bool offered = waitForState(rig.svc, UpdateState::UpdateAvailable);
    rig.svc.requestDownload();
    const bool reached = frozenAt(rig, want);
    return offered && reached;
}

static bool freezeAfterInstall(Rig& rig, UpdateState want) {
    const bool downloaded = freezeAfterDownload(rig, UpdateState::Downloaded);
    rig.svc.start();
    rig.svc.requestInstall();
    const bool reached = frozenAt(rig, want);
    return downloaded && reached;
}

static bool leaveFresh(Rig&) { return true; }

static bool freezeOffered(Rig& rig) { return freezeAfterCheck(rig, UpdateState::UpdateAvailable); }

static bool freezeStaged(Rig& rig) { return freezeAfterDownload(rig, UpdateState::Downloaded); }

static bool freezeAfterRepair(Rig& rig, UpdateState want) {
    rig.svc.start();
    rig.svc.requestCheck(true);
    const bool upToDate = waitForState(rig.svc, UpdateState::UpToDate);
    rig.svc.requestRepair();
    const bool reached = frozenAt(rig, want);
    return upToDate && reached;
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

static void test_cancel_settles_a_running_download_back_at_the_offer() {
    TEST("cancelInFlight: a running download settles back at UpdateAvailable, not Error");
    Rig rig;
    rig.up.blockDownloadUntilCancel = true;
    rig.svc.start();
    rig.svc.requestCheck(true);
    EXPECT(waitForState(rig.svc, UpdateState::UpdateAvailable));
    rig.svc.requestDownload();
    EXPECT(waitForCount(rig.up.downloadCalls, 1));
    rig.svc.cancelInFlight();
    EXPECT(frozenAt(rig, UpdateState::UpdateAvailable));
    const UpdateStatusSnapshot snap = rig.svc.snapshot();
    EXPECT(snap.message.empty());
    EXPECT(snap.failedPhase == UpdateState::Idle);
    EXPECT(snap.bytesDownloaded == 0);
    EXPECT(snap.totalBytes == 0);
    EXPECT(snap.info.version == "99.0.0");

    TEST("cancelInFlight: a cancelled download is logged as cancelled");
    EXPECT(logged(rig.log, "Download cancelled"));
}

static void test_cancel_drops_a_queued_download() {
    TEST("cancelInFlight: a queued download that has not started drops back to UpdateAvailable "
         "at once");
    Rig rig;
    EXPECT(freezeAfterCheck(rig, UpdateState::UpdateAvailable));
    rig.svc.requestDownload();
    rig.svc.cancelInFlight();
    const UpdateStatusSnapshot snap = rig.svc.snapshot();
    EXPECT(snap.state == UpdateState::UpdateAvailable);
    EXPECT(snap.totalBytes == 0);
    EXPECT(logged(rig.log, "Download cancelled"));

    TEST("cancelInFlight: the dropped download never runs once the worker resumes");
    rig.svc.start();
    rig.svc.requestCheck(true);
    EXPECT(waitForCount(rig.up.fetchCalls, 2));
    EXPECT(frozenAt(rig, UpdateState::UpdateAvailable));
    EXPECT(rig.up.downloadCalls == 0);

    TEST("cancelInFlight: a download requested again after the drop runs to the end");
    rig.svc.requestDownload();
    rig.svc.start();
    EXPECT(waitForState(rig.svc, UpdateState::Downloaded));
}

static void test_cancel_drops_a_queued_repair_back_to_idle() {
    TEST("cancelInFlight: a queued repair of the release we run drops back to Idle at once");
    Rig rig;
    rig.up.fetchVersion = SATELLITE_VERSION;
    EXPECT(freezeAfterCheck(rig, UpdateState::UpToDate));
    rig.svc.requestRepair();
    rig.svc.cancelInFlight();
    EXPECT(rig.svc.snapshot().state == UpdateState::Idle);

    TEST("cancelInFlight: the dropped repair does not come back after the next check");
    rig.svc.start();
    rig.svc.requestCheck(true);
    EXPECT(waitForCount(rig.up.fetchCalls, 2));
    EXPECT(frozenAt(rig, UpdateState::UpToDate));
    EXPECT(rig.svc.snapshot().state == UpdateState::UpToDate);
    EXPECT(rig.up.downloadCalls == 0);
}

static void test_cancel_settles_a_running_repair_at_idle() {
    TEST("cancelInFlight: a running repair of the release we run settles at Idle, not Error");
    Rig rig;
    rig.up.fetchVersion = SATELLITE_VERSION;
    rig.up.blockDownloadUntilCancel = true;
    rig.svc.start();
    rig.svc.requestCheck(true);
    EXPECT(waitForState(rig.svc, UpdateState::UpToDate));
    rig.svc.requestRepair();
    EXPECT(waitForCount(rig.up.downloadCalls, 1));
    rig.svc.cancelInFlight();
    EXPECT(frozenAt(rig, UpdateState::Idle));
    EXPECT(rig.svc.snapshot().message.empty());
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

static void test_download_queued_before_skip_settles_without_relocking() {
    TEST("doDownload: a download queued before skipVersion settles to Idle on a live worker");
    MockUpdater up;
    MockLog log;
    Config cfg;
    std::mutex cfgMtx;
    UpdateService svc(up, log, cfg, cfgMtx);
    std::atomic<bool> holdWorker{true};
    std::atomic<bool> workerHeld{false};
    svc.setStatusCallback([&](const UpdateStatusSnapshot& snap) {
        if (snap.state != UpdateState::UpdateAvailable || workerHeld.exchange(true)) return;
        while (holdWorker.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
    svc.start();
    svc.requestCheck(true);
    for (int i = 0; i < 3000 && !workerHeld.load(); i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT(workerHeld.load());
    svc.requestDownload();
    svc.skipVersion("99.0.0");
    holdWorker = false;
    bool settled = false;
    for (int i = 0; i < 3000 && !settled; i++) {
        settled = svc.snapshot().message == "No update to download";
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT(settled);
    EXPECT(svc.snapshot().state == UpdateState::Idle);

    TEST("doDownload: a download queued before skipVersion settles with no bytes and no failed "
         "phase");
    EXPECT(svc.snapshot().totalBytes == 0);
    EXPECT(svc.snapshot().bytesDownloaded == 0);
    EXPECT(svc.snapshot().failedPhase == UpdateState::Idle);
    svc.requestCheck(true);
    EXPECT(waitForState(svc, UpdateState::UpToDate));
}

static void test_repair_ignores_manual_install_hosts() {
    TEST("requestRepair: a package-managed host is left alone");
    MockUpdater up;
    up.fetchVersion = SATELLITE_VERSION;
    up.method = InstallMethod::Manual;
    MockLog log;
    Config cfg;
    std::mutex cfgMtx;
    UpdateService svc(up, log, cfg, cfgMtx);
    svc.start();
    svc.requestCheck(true);
    EXPECT(waitForState(svc, UpdateState::UpToDate));
    svc.requestRepair();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT(svc.snapshot().state == UpdateState::UpToDate);
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

static void test_dismiss_keeps_an_available_update_on_offer() {
    TEST("dismiss: an available update stays UpdateAvailable, marked dismissed");
    Rig rig;
    EXPECT(freezeAfterCheck(rig, UpdateState::UpdateAvailable));
    const int persistsBefore = rig.persistCalls;
    rig.svc.dismiss();
    const UpdateStatusSnapshot snap = rig.svc.snapshot();
    EXPECT(snap.state == UpdateState::UpdateAvailable);
    EXPECT(snap.dismissed);
    EXPECT(snap.info.version == "99.0.0");

    TEST("dismiss: records lastSeenVersion and persists it");
    EXPECT(rig.cfg.lastSeenVersion == "99.0.0");
    EXPECT(rig.persistCalls == persistsBefore + 1);
}

static void test_dismiss_keeps_a_downloaded_update_staged() {
    TEST("dismiss: a downloaded update stays Downloaded, marked dismissed");
    Rig rig;
    EXPECT(freezeAfterDownload(rig, UpdateState::Downloaded));
    rig.svc.dismiss();
    const UpdateStatusSnapshot snap = rig.svc.snapshot();
    EXPECT(snap.state == UpdateState::Downloaded);
    EXPECT(snap.dismissed);
    EXPECT(rig.cfg.lastSeenVersion == "99.0.0");

    TEST("dismiss: the dismissed download still installs on request");
    rig.svc.requestInstall();
    rig.svc.start();
    EXPECT(waitForCount(rig.up.applyCalls, 1));
}

static void test_dismiss_elsewhere_changes_nothing() {
    TEST("dismiss: after a failed check it records, persists and broadcasts nothing");
    Rig rig;
    EXPECT(freezeAfterCheck(rig, UpdateState::UpdateAvailable));
    rig.up.fetchOk = false;
    rig.svc.start();
    rig.svc.requestCheck(true);
    EXPECT(waitForCount(rig.up.fetchCalls, 2));
    EXPECT(frozenAt(rig, UpdateState::Error));
    const int persistsBefore = rig.persistCalls;
    const int broadcastsBefore = broadcastCount(rig.broadcasts);
    rig.svc.dismiss();
    EXPECT(rig.cfg.lastSeenVersion.empty());
    EXPECT(rig.persistCalls == persistsBefore);
    EXPECT(broadcastCount(rig.broadcasts) == broadcastsBefore);

    TEST("dismiss: after a failed check it leaves the update undismissed for later");
    rig.svc.start();
    rig.svc.requestCheck(false);
    EXPECT(waitForCount(rig.up.fetchCalls, 3));
    EXPECT(frozenAt(rig, UpdateState::UpdateAvailable));
    EXPECT(!rig.svc.snapshot().dismissed);
}

static void test_a_check_that_fetched_clears_the_dismissal() {
    TEST("dismiss: the next check that fetched offers the update again");
    Rig rig;
    EXPECT(freezeAfterCheck(rig, UpdateState::UpdateAvailable));
    rig.svc.dismiss();
    EXPECT(rig.svc.snapshot().dismissed);
    rig.svc.start();
    rig.svc.requestCheck(false);
    EXPECT(waitForCount(rig.up.fetchCalls, 2));
    EXPECT(frozenAt(rig, UpdateState::UpdateAvailable));
    EXPECT(!rig.svc.snapshot().dismissed);

    TEST("dismiss: the next check that fetched brings a dismissed staged installer back too");
    Rig staged;
    EXPECT(freezeAfterDownload(staged, UpdateState::Downloaded));
    staged.svc.dismiss();
    staged.svc.start();
    staged.svc.requestCheck(false);
    EXPECT(waitForCount(staged.up.fetchCalls, 2));
    EXPECT(frozenAt(staged, UpdateState::Downloaded));
    EXPECT(!staged.svc.snapshot().dismissed);
}

struct ClearingCase {
    const char* label;
    void (UpdateService::*request)();
};

static void test_an_accepted_download_or_repair_clears_the_dismissal() {
    const ClearingCase cases[] = {
        {"dismiss: an accepted download clears the dismissal, even when it is cancelled",
         &UpdateService::requestDownload},
        {"dismiss: an accepted repair clears the dismissal, even when it is cancelled",
         &UpdateService::requestRepair},
    };
    for (const ClearingCase& clearing : cases) {
        TEST(clearing.label);
        Rig rig;
        EXPECT(freezeOffered(rig));
        rig.svc.dismiss();
        (rig.svc.*clearing.request)();
        rig.svc.cancelInFlight();
        const UpdateStatusSnapshot snap = rig.svc.snapshot();
        EXPECT(snap.state == UpdateState::UpdateAvailable);
        EXPECT(!snap.dismissed);
    }
}

static void test_a_download_after_a_dismissal_lands_undismissed() {
    TEST("dismiss: a download accepted after a dismissal lands as an undismissed Downloaded");
    Rig rig;
    EXPECT(freezeOffered(rig));
    rig.svc.dismiss();
    rig.svc.requestDownload();
    rig.svc.start();
    EXPECT(frozenAt(rig, UpdateState::Downloaded));
    EXPECT(!rig.svc.snapshot().dismissed);
}

static void test_an_accepted_install_clears_the_dismissal() {
    TEST("dismiss: an accepted install clears the dismissal, even when the launch fails");
    Rig rig;
    rig.up.applyOk = false;
    EXPECT(freezeAfterDownload(rig, UpdateState::Downloaded));
    rig.svc.dismiss();
    rig.svc.requestInstall();
    rig.svc.start();
    EXPECT(frozenAt(rig, UpdateState::Error));
    rig.up.fetchOk = false;
    rig.svc.start();
    rig.svc.requestCheck(false);
    EXPECT(waitForCount(rig.up.fetchCalls, 2));
    EXPECT(frozenAt(rig, UpdateState::Downloaded));
    EXPECT(!rig.svc.snapshot().dismissed);
}

static void test_check_is_visible_before_the_worker_runs_it() {
    TEST("requestCheck: an accepted check is Checking in the very next snapshot, before any "
         "worker runs it");
    Rig rig;
    rig.svc.requestCheck(true);
    EXPECT(rig.svc.snapshot().state == UpdateState::Checking);
}

static void test_check_after_an_error_drops_it_at_once() {
    TEST("requestCheck: accepted after a failed check, the next snapshot drops the error");
    Rig rig;
    rig.up.fetchOk = false;
    rig.up.fetchError = "no network";
    EXPECT(freezeAfterCheck(rig, UpdateState::Error));
    rig.svc.requestCheck(true);
    const UpdateStatusSnapshot snap = rig.svc.snapshot();
    EXPECT(snap.state == UpdateState::Checking);
    EXPECT(snap.message.empty());
    EXPECT(snap.failedPhase == UpdateState::Idle);

    TEST("requestCheck: the worker then runs the check the snapshot shows");
    rig.svc.start();
    EXPECT(waitForCount(rig.up.fetchCalls, 2));
}

static void test_download_is_visible_before_the_worker_runs_it() {
    TEST("requestDownload: an accepted download is a fresh Downloading in the very next snapshot");
    Rig rig;
    rig.up.downloadOk = false;
    rig.up.assetSize = 4096;
    EXPECT(freezeAfterDownload(rig, UpdateState::Error));
    EXPECT(rig.svc.snapshot().bytesDownloaded == 4096);
    rig.svc.requestDownload();
    const UpdateStatusSnapshot snap = rig.svc.snapshot();
    EXPECT(snap.state == UpdateState::Downloading);
    EXPECT(snap.bytesDownloaded == 0);
    EXPECT(snap.totalBytes == 4096);
    EXPECT(snap.message.empty());
    EXPECT(snap.failedPhase == UpdateState::Idle);

    TEST("requestDownload: the worker then runs the download the snapshot shows");
    rig.up.downloadOk = true;
    rig.svc.start();
    EXPECT(waitForState(rig.svc, UpdateState::Downloaded));
}

static void test_repair_of_a_known_release_is_visible_at_once() {
    TEST("requestRepair: with a known release, the very next snapshot is Downloading it");
    Rig rig;
    rig.up.fetchVersion = SATELLITE_VERSION;
    EXPECT(freezeAfterCheck(rig, UpdateState::UpToDate));
    rig.svc.requestRepair();
    const UpdateStatusSnapshot snap = rig.svc.snapshot();
    EXPECT(snap.state == UpdateState::Downloading);
    EXPECT(snap.totalBytes == rig.up.assetSize);

    TEST("requestRepair: the worker then downloads the release we already run");
    rig.svc.start();
    EXPECT(waitForState(rig.svc, UpdateState::Downloaded));
}

static void test_repair_with_no_known_release_is_visible_at_once() {
    TEST("requestRepair: with no known release, the very next snapshot is Checking");
    Rig rig;
    rig.up.fetchVersion = SATELLITE_VERSION;
    rig.svc.requestRepair();
    EXPECT(rig.svc.snapshot().state == UpdateState::Checking);

    TEST("requestRepair: the worker then checks once and downloads the release we already run");
    rig.svc.start();
    EXPECT(waitForState(rig.svc, UpdateState::Downloaded));
    EXPECT(rig.up.fetchCalls == 1);
}

static void test_install_is_visible_at_once() {
    TEST("requestInstall: an accepted install is Installing in the very next snapshot");
    Rig rig;
    EXPECT(freezeAfterDownload(rig, UpdateState::Downloaded));
    rig.svc.requestInstall();
    EXPECT(rig.svc.snapshot().state == UpdateState::Installing);

    TEST("requestInstall: the worker then launches the staged installer");
    rig.svc.start();
    EXPECT(waitForCount(rig.up.applyCalls, 1));
}

static void test_check_is_rejected_while_work_is_queued() {
    TEST("requestCheck: rejected while a download is queued");
    Rig downloading;
    EXPECT(freezeAfterCheck(downloading, UpdateState::UpdateAvailable));
    downloading.svc.requestDownload();
    downloading.svc.requestCheck(true);
    EXPECT(downloading.svc.snapshot().state == UpdateState::Downloading);

    TEST("requestCheck: rejected while an install is queued");
    Rig installing;
    EXPECT(freezeAfterDownload(installing, UpdateState::Downloaded));
    installing.svc.requestInstall();
    installing.svc.requestCheck(true);
    EXPECT(installing.svc.snapshot().state == UpdateState::Installing);
}

struct RejectedCase {
    const char* label;
    bool (*prepare)(Rig&);
    void (UpdateService::*request)();
    UpdateState stays;
};

static void test_requests_outside_their_states_change_nothing() {
    const RejectedCase cases[] = {
        {"requestDownload: rejected while Downloaded, so the staged installer stays", freezeStaged,
         &UpdateService::requestDownload, UpdateState::Downloaded},
        {"requestRepair: rejected while Downloaded, so the staged installer stays", freezeStaged,
         &UpdateService::requestRepair, UpdateState::Downloaded},
        {"requestInstall: rejected while an update is only on offer", freezeOffered,
         &UpdateService::requestInstall, UpdateState::UpdateAvailable},
        {"requestRetry: with an update on offer it neither checks nor downloads", freezeOffered,
         &UpdateService::requestRetry, UpdateState::UpdateAvailable},
        {"requestRetry: on an idle service it does not check", leaveFresh,
         &UpdateService::requestRetry, UpdateState::Idle},
    };
    for (const RejectedCase& rejected : cases) {
        TEST(rejected.label);
        Rig rig;
        EXPECT(rejected.prepare(rig));
        (rig.svc.*rejected.request)();
        EXPECT(rig.svc.snapshot().state == rejected.stays);
    }
}

static void test_requests_do_not_broadcast() {
    TEST("an accepted check does not broadcast from the caller's thread");
    Rig fresh;
    fresh.svc.requestCheck(true);
    EXPECT(broadcastCount(fresh.broadcasts) == 0);

    TEST("an accepted download, its cancel and a repair do not broadcast from the caller's "
         "thread");
    Rig offered;
    EXPECT(freezeAfterCheck(offered, UpdateState::UpdateAvailable));
    const int beforeDownload = broadcastCount(offered.broadcasts);
    offered.svc.requestDownload();
    offered.svc.cancelInFlight();
    offered.svc.requestRepair();
    EXPECT(broadcastCount(offered.broadcasts) == beforeDownload);

    TEST("an accepted retry of an install does not broadcast from the caller's thread");
    Rig failed;
    failed.up.applyOk = false;
    EXPECT(freezeAfterInstall(failed, UpdateState::Error));
    const int beforeRetry = broadcastCount(failed.broadcasts);
    failed.svc.requestRetry();
    EXPECT(broadcastCount(failed.broadcasts) == beforeRetry);
}

static void test_install_queued_before_skip_never_runs() {
    TEST("an install queued before skipVersion of its release never launches the installer");
    Rig rig;
    EXPECT(freezeAfterDownload(rig, UpdateState::Downloaded));
    rig.svc.requestInstall();
    rig.svc.skipVersion("99.0.0");
    EXPECT(rig.svc.snapshot().state == UpdateState::Idle);
    rig.svc.start();
    rig.svc.requestCheck(true);
    EXPECT(waitForCount(rig.up.fetchCalls, 2));
    EXPECT(frozenAt(rig, UpdateState::UpToDate));
    EXPECT(rig.up.applyCalls == 0);
}

struct RetriedDownloadCase {
    const char* label;
    bool MockUpdater::* failing;
    UpdateState failedPhase;
    int verifiesAfterRetry;
};

static void test_retry_after_a_failed_download_or_verify_downloads_again() {
    const RetriedDownloadCase cases[] = {
        {"requestRetry: after a failed download, a fresh Downloading at once that reaches "
         "Downloaded without another check",
         &MockUpdater::downloadOk, UpdateState::Downloading, 1},
        {"requestRetry: after a failed verify, a fresh Downloading at once that is verified "
         "afresh without another check",
         &MockUpdater::verifyOk, UpdateState::Verifying, 2},
    };
    for (const RetriedDownloadCase& retried : cases) {
        TEST(retried.label);
        Rig rig;
        rig.up.*retried.failing = false;
        EXPECT(freezeAfterDownload(rig, UpdateState::Error));
        EXPECT(rig.svc.snapshot().failedPhase == retried.failedPhase);
        rig.svc.requestRetry();
        const UpdateStatusSnapshot snap = rig.svc.snapshot();
        EXPECT(snap.state == UpdateState::Downloading);
        EXPECT(snap.failedPhase == UpdateState::Idle);
        rig.up.*retried.failing = true;
        rig.svc.start();
        EXPECT(frozenAt(rig, UpdateState::Downloaded));
        EXPECT(rig.up.downloadCalls == 2);
        EXPECT(rig.up.verifyCalls == retried.verifiesAfterRetry);
        EXPECT(rig.up.fetchCalls == 1);
    }
}

static void test_retry_after_a_failed_repair_repairs_again() {
    TEST("requestRetry: after a failed repair of the release we run, the repair downloads again");
    Rig rig;
    rig.up.fetchVersion = SATELLITE_VERSION;
    rig.up.downloadOk = false;
    EXPECT(freezeAfterRepair(rig, UpdateState::Error));
    EXPECT(!rig.svc.snapshot().info.available);
    rig.svc.requestRetry();
    EXPECT(rig.svc.snapshot().state == UpdateState::Downloading);

    TEST("requestRetry: the retried repair runs to Downloaded");
    rig.up.downloadOk = true;
    rig.svc.start();
    EXPECT(frozenAt(rig, UpdateState::Downloaded));
}

static void test_retry_after_a_failed_check_checks_again_as_the_user() {
    TEST("requestRetry: after a failed check, the next snapshot is Checking");
    Rig rig;
    EXPECT(freezeAfterCheck(rig, UpdateState::UpdateAvailable));
    rig.up.fetchOk = false;
    rig.svc.start();
    rig.svc.requestCheck(true);
    EXPECT(waitForCount(rig.up.fetchCalls, 2));
    EXPECT(frozenAt(rig, UpdateState::Error));
    rig.svc.requestRetry();
    EXPECT(rig.svc.snapshot().state == UpdateState::Checking);

    TEST("requestRetry: the retried check is the user's, so its failure stays an error");
    rig.svc.start();
    EXPECT(waitForCount(rig.up.fetchCalls, 3));
    EXPECT(frozenAt(rig, UpdateState::Error));
    EXPECT(rig.svc.snapshot().failedPhase == UpdateState::Checking);
}

static void test_check_is_accepted_while_downloaded() {
    TEST("requestCheck: accepted while Downloaded, and Checking in the very next snapshot");
    Rig rig;
    EXPECT(freezeAfterDownload(rig, UpdateState::Downloaded));
    rig.svc.requestCheck(false);
    EXPECT(rig.svc.snapshot().state == UpdateState::Checking);
}

static void test_a_check_that_finds_the_staged_release_keeps_it() {
    TEST("a check that finds the staged release settles back at Downloaded");
    Rig rig;
    EXPECT(freezeAfterDownload(rig, UpdateState::Downloaded));
    rig.svc.start();
    rig.svc.requestCheck(false);
    EXPECT(waitForCount(rig.up.fetchCalls, 2));
    EXPECT(frozenAt(rig, UpdateState::Downloaded));
    EXPECT(rig.svc.snapshot().info.version == "99.0.0");

    TEST("a check that finds the staged release leaves its installer to launch, undownloaded");
    rig.svc.requestInstall();
    rig.svc.start();
    EXPECT(waitForCount(rig.up.applyCalls, 1));
    rig.svc.stop();
    EXPECT(rig.up.downloadCalls == 1);
    EXPECT(rig.up.lastAppliedPath == "/tmp/satellite-update");
}

static void test_a_check_that_finds_the_staged_repair_keeps_it() {
    TEST("a check that finds a staged repair of the release we run settles back at Downloaded");
    Rig rig;
    rig.up.fetchVersion = SATELLITE_VERSION;
    EXPECT(freezeAfterRepair(rig, UpdateState::Downloaded));
    rig.svc.start();
    rig.svc.requestCheck(true);
    EXPECT(waitForCount(rig.up.fetchCalls, 2));
    EXPECT(frozenAt(rig, UpdateState::Downloaded));
    EXPECT(!rig.svc.snapshot().info.available);
}

static void test_a_check_that_finds_a_rebuilt_asset_offers_it_afresh() {
    TEST("a check that finds the staged version with a different checksum offers it afresh");
    Rig rig;
    EXPECT(freezeAfterDownload(rig, UpdateState::Downloaded));
    rig.up.assetSha256 = "rebuilt";
    rig.svc.start();
    rig.svc.requestCheck(false);
    EXPECT(waitForCount(rig.up.fetchCalls, 2));
    EXPECT(frozenAt(rig, UpdateState::UpdateAvailable));
    EXPECT(rig.svc.snapshot().state == UpdateState::UpdateAvailable);
}

static void test_a_failed_background_check_returns_to_downloaded() {
    TEST("a background check that fails while an installer is staged returns to Downloaded");
    Rig rig;
    EXPECT(freezeAfterDownload(rig, UpdateState::Downloaded));
    rig.up.fetchOk = false;
    rig.svc.start();
    rig.svc.requestCheck(false);
    EXPECT(waitForCount(rig.up.fetchCalls, 2));
    EXPECT(frozenAt(rig, UpdateState::Downloaded));
    const UpdateStatusSnapshot snap = rig.svc.snapshot();
    EXPECT(snap.message.empty());
    EXPECT(snap.failedPhase == UpdateState::Idle);
}

static void test_a_failed_user_check_keeps_the_staged_installer_for_the_next_check() {
    TEST("a user check that fails while an installer is staged lands in Error(Checking)");
    Rig rig;
    EXPECT(freezeAfterDownload(rig, UpdateState::Downloaded));
    rig.up.fetchOk = false;
    rig.svc.start();
    rig.svc.requestCheck(true);
    EXPECT(waitForCount(rig.up.fetchCalls, 2));
    EXPECT(frozenAt(rig, UpdateState::Error));
    EXPECT(rig.svc.snapshot().failedPhase == UpdateState::Checking);

    TEST("requestInstall: rejected after a failed check, even with an installer staged");
    rig.svc.requestInstall();
    EXPECT(rig.svc.snapshot().state == UpdateState::Error);

    TEST("the next check that fetched brings the staged installer back, ready to install");
    rig.up.fetchOk = true;
    rig.svc.start();
    rig.svc.requestCheck(true);
    EXPECT(waitForCount(rig.up.fetchCalls, 3));
    EXPECT(frozenAt(rig, UpdateState::Downloaded));
    rig.svc.requestInstall();
    EXPECT(rig.svc.snapshot().state == UpdateState::Installing);
}

static void test_a_check_after_a_failed_install_keeps_the_installer_reachable() {
    TEST("after a failed install, a background check that fetched returns to Downloaded");
    Rig rig;
    rig.up.applyOk = false;
    EXPECT(freezeAfterInstall(rig, UpdateState::Error));
    rig.svc.start();
    rig.svc.requestCheck(false);
    EXPECT(waitForCount(rig.up.fetchCalls, 2));
    EXPECT(frozenAt(rig, UpdateState::Downloaded));

    TEST("after a failed install, a background check that fails returns to Downloaded too");
    rig.svc.requestInstall();
    rig.svc.start();
    EXPECT(waitForCount(rig.up.applyCalls, 2));
    EXPECT(frozenAt(rig, UpdateState::Error));
    rig.up.fetchOk = false;
    rig.svc.start();
    rig.svc.requestCheck(false);
    EXPECT(waitForCount(rig.up.fetchCalls, 3));
    EXPECT(frozenAt(rig, UpdateState::Downloaded));

    TEST("after a failed install and a check, the staged installer launches again");
    rig.up.applyOk = true;
    rig.svc.requestInstall();
    rig.svc.start();
    EXPECT(waitForCount(rig.up.applyCalls, 3));
    rig.svc.stop();
    EXPECT(rig.up.downloadCalls == 1);
}

static void test_a_user_check_right_after_a_repair_check_keeps_the_repair_staged() {
    TEST("a user check accepted right after a repair's check still ends with the repair staged");
    WorkerHold hold;
    Rig rig;
    rig.up.fetchVersion = SATELLITE_VERSION;
    holdAt(rig, hold, {UpdateState::UpToDate});
    rig.svc.requestRepair();
    rig.svc.start();
    EXPECT(waitForFlag(hold.held));
    rig.svc.requestCheck(true);
    hold.released = true;
    EXPECT(waitForCount(rig.up.fetchCalls, 2));
    EXPECT(waitForCount(rig.up.verifyCalls, 1));
    EXPECT(frozenAt(rig, UpdateState::Downloaded));
    EXPECT(rig.svc.snapshot().state == UpdateState::Downloaded);
    EXPECT(rig.up.downloadCalls == 1);

    TEST("a user check accepted right after a repair's check runs before the repair download");
    EXPECT(rig.up.calls == "fetch fetch download verify ");
}

static void test_a_new_download_supersedes_the_staged_installer() {
    TEST("a download that begins supersedes the staged installer, even when it then fails");
    Rig rig;
    rig.up.applyOk = false;
    EXPECT(freezeAfterInstall(rig, UpdateState::Error));
    rig.up.downloadOk = false;
    rig.svc.requestDownload();
    rig.svc.start();
    EXPECT(waitForCount(rig.up.downloadCalls, 2));
    EXPECT(frozenAt(rig, UpdateState::Error));
    rig.up.fetchOk = false;
    rig.svc.start();
    rig.svc.requestCheck(false);
    EXPECT(waitForCount(rig.up.fetchCalls, 2));
    EXPECT(frozenAt(rig, UpdateState::UpdateAvailable));
    rig.svc.requestInstall();
    EXPECT(rig.svc.snapshot().state == UpdateState::UpdateAvailable);
}

static void test_skipping_the_staged_release_forgets_its_installer() {
    TEST("skipVersion of the staged release forgets its installer, so no check revives it");
    Rig rig;
    EXPECT(freezeAfterDownload(rig, UpdateState::Downloaded));
    rig.svc.skipVersion("99.0.0");
    rig.svc.start();
    rig.svc.requestCheck(true);
    EXPECT(waitForCount(rig.up.fetchCalls, 2));
    EXPECT(frozenAt(rig, UpdateState::UpToDate));
    EXPECT(rig.svc.snapshot().state == UpdateState::UpToDate);
}

static void test_a_repair_whose_check_finds_the_staged_installer_keeps_it() {
    TEST("a repair whose check finds an installer already staged does not download it again");
    Rig rig;
    EXPECT(freezeAfterDownload(rig, UpdateState::Downloaded));
    rig.up.fetchVersion = "99.1.0";
    rig.svc.start();
    rig.svc.requestCheck(true);
    EXPECT(waitForCount(rig.up.fetchCalls, 2));
    EXPECT(frozenAt(rig, UpdateState::UpdateAvailable));
    rig.svc.skipVersion("99.1.0");
    rig.up.fetchVersion = "99.0.0";
    rig.svc.requestRepair();
    rig.svc.start();
    EXPECT(waitForCount(rig.up.fetchCalls, 3));
    EXPECT(frozenAt(rig, UpdateState::Downloaded));
    EXPECT(rig.svc.snapshot().state == UpdateState::Downloaded);
    EXPECT(rig.up.downloadCalls == 1);
}

static void test_an_install_accepted_after_a_repair_check_ends_the_repair() {
    TEST("an install accepted right after a repair check ends the repair, so no download follows");
    WorkerHold hold;
    Rig rig;
    rig.up.applyOk = false;
    EXPECT(freezeAfterDownload(rig, UpdateState::Downloaded));
    rig.up.fetchVersion = "99.1.0";
    rig.svc.start();
    rig.svc.requestCheck(true);
    EXPECT(waitForCount(rig.up.fetchCalls, 2));
    EXPECT(frozenAt(rig, UpdateState::UpdateAvailable));
    rig.svc.skipVersion("99.1.0");
    rig.up.fetchVersion = "99.0.0";
    holdAt(rig, hold, {UpdateState::Downloaded});
    rig.svc.requestRepair();
    rig.svc.start();
    EXPECT(waitForFlag(hold.held));
    rig.svc.requestInstall();
    hold.released = true;
    EXPECT(waitForCount(rig.up.applyCalls, 1));
    EXPECT(frozenAt(rig, UpdateState::Error));
    rig.up.fetchVersion = "99.2.0";
    rig.svc.start();
    rig.svc.requestCheck(true);
    EXPECT(waitForCount(rig.up.fetchCalls, 4));
    EXPECT(frozenAt(rig, UpdateState::UpdateAvailable));
    EXPECT(rig.svc.snapshot().state == UpdateState::UpdateAvailable);
    EXPECT(rig.up.downloadCalls == 1);
}

static void test_a_repair_joins_a_queued_background_check() {
    TEST("requestRepair: arriving while a background check is queued, the very next snapshot is "
         "that check");
    Rig rig;
    rig.up.fetchVersion = SATELLITE_VERSION;
    rig.svc.requestCheck(false);
    rig.svc.requestRepair();
    EXPECT(rig.svc.snapshot().state == UpdateState::Checking);

    TEST("requestRepair: joined to a queued background check, it ends with the repair staged");
    rig.svc.start();
    EXPECT(frozenAt(rig, UpdateState::Downloaded));
    EXPECT(rig.up.fetchCalls == 1);
    rig.svc.requestInstall();
    EXPECT(rig.svc.snapshot().state == UpdateState::Installing);
}

static void test_a_repair_joins_a_running_background_check() {
    TEST("requestRepair: joined to a running background check, it ends with the repair staged");
    WorkerHold hold;
    Rig rig;
    rig.up.fetchVersion = SATELLITE_VERSION;
    holdAt(rig, hold, {UpdateState::Checking});
    rig.svc.requestCheck(false);
    rig.svc.start();
    EXPECT(waitForFlag(hold.held));
    rig.svc.requestRepair();
    hold.released = true;
    EXPECT(waitForCount(rig.up.verifyCalls, 1));
    EXPECT(frozenAt(rig, UpdateState::Downloaded));
    EXPECT(rig.up.fetchCalls == 1);
    EXPECT(rig.up.downloadCalls == 1);
}

static void test_a_failed_check_under_a_joined_repair_reports_the_error() {
    TEST("requestRepair: a background check it joined that fails lands in Error(Checking), even "
         "with an update known");
    Rig rig;
    EXPECT(freezeAfterCheck(rig, UpdateState::UpdateAvailable));
    rig.up.fetchOk = false;
    rig.svc.requestCheck(false);
    rig.svc.requestRepair();
    rig.svc.start();
    EXPECT(waitForCount(rig.up.fetchCalls, 2));
    EXPECT(frozenAt(rig, UpdateState::Error));
    EXPECT(rig.svc.snapshot().failedPhase == UpdateState::Checking);

    TEST("requestRepair: a joined repair whose check failed does not download after the next "
         "check");
    rig.up.fetchOk = true;
    rig.svc.start();
    rig.svc.requestCheck(true);
    EXPECT(waitForCount(rig.up.fetchCalls, 3));
    EXPECT(frozenAt(rig, UpdateState::UpdateAvailable));
    EXPECT(rig.svc.snapshot().state == UpdateState::UpdateAvailable);
    EXPECT(rig.up.downloadCalls == 0);
}

static void test_a_repair_joined_to_a_check_clears_the_dismissal() {
    TEST("dismiss: a repair joined to a queued check clears the dismissal");
    Rig rig;
    EXPECT(freezeAfterCheck(rig, UpdateState::UpdateAvailable));
    rig.svc.dismiss();
    rig.up.fetchOk = false;
    rig.svc.requestCheck(false);
    rig.svc.requestRepair();
    rig.svc.start();
    EXPECT(waitForCount(rig.up.fetchCalls, 2));
    EXPECT(frozenAt(rig, UpdateState::Error));
    rig.svc.start();
    rig.svc.requestCheck(false);
    EXPECT(waitForCount(rig.up.fetchCalls, 3));
    EXPECT(frozenAt(rig, UpdateState::UpdateAvailable));
    EXPECT(!rig.svc.snapshot().dismissed);
}

static void test_a_repair_on_a_package_managed_host_does_not_join_a_check() {
    TEST("requestRepair: on a package-managed host it does not join a check, so a background "
         "failure stays quiet");
    Rig rig;
    rig.up.method = InstallMethod::Manual;
    EXPECT(freezeAfterCheck(rig, UpdateState::UpdateAvailable));
    rig.up.fetchOk = false;
    rig.svc.requestCheck(false);
    rig.svc.requestRepair();
    rig.svc.start();
    EXPECT(waitForCount(rig.up.fetchCalls, 2));
    rig.svc.stop();
    EXPECT(rig.svc.snapshot().state == UpdateState::UpdateAvailable);
}

static void test_an_install_that_fails_after_a_skip_leaves_the_skip_alone() {
    TEST("skipVersion during an install whose launch then fails stays Idle, not Error");
    WorkerHold hold;
    Rig rig;
    rig.up.applyOk = false;
    EXPECT(freezeStaged(rig));
    holdAt(rig, hold, {UpdateState::Installing});
    rig.svc.requestInstall();
    rig.svc.start();
    EXPECT(waitForFlag(hold.held));
    rig.svc.skipVersion("99.0.0");
    hold.released = true;
    EXPECT(waitForCount(rig.up.applyCalls, 1));
    rig.svc.stop();
    const UpdateStatusSnapshot snap = rig.svc.snapshot();
    EXPECT(snap.state == UpdateState::Idle);
    EXPECT(snap.failedPhase == UpdateState::Idle);

    TEST("skipVersion during a failing install leaves no installer for Try again to relaunch");
    rig.svc.requestRetry();
    rig.svc.requestInstall();
    EXPECT(rig.svc.snapshot().state == UpdateState::Idle);
}

static void test_a_download_that_fails_after_a_skip_leaves_the_skip_alone() {
    TEST("skipVersion during a running download that then fails stays Idle, not Error");
    WorkerHold hold;
    Rig rig;
    rig.up.downloadOk = false;
    EXPECT(freezeOffered(rig));
    holdAt(rig, hold, {UpdateState::Downloading});
    rig.svc.requestDownload();
    rig.svc.start();
    EXPECT(waitForFlag(hold.held));
    rig.svc.skipVersion("99.0.0");
    hold.released = true;
    EXPECT(waitForCount(rig.up.downloadCalls, 1));
    rig.svc.stop();
    const UpdateStatusSnapshot snap = rig.svc.snapshot();
    EXPECT(snap.state == UpdateState::Idle);
    EXPECT(snap.failedPhase == UpdateState::Idle);

    TEST("skipVersion during a failing download leaves Try again nothing to repair");
    rig.svc.requestRetry();
    EXPECT(rig.svc.snapshot().state == UpdateState::Idle);
}

static void test_a_verify_that_fails_after_a_skip_leaves_the_skip_alone() {
    TEST("skipVersion during a verify that then fails stays Idle, not Error");
    Rig rig;
    rig.up.verifyOk = false;
    EXPECT(freezeOffered(rig));
    rig.up.verifyGateOpen = false;
    rig.svc.requestDownload();
    rig.svc.start();
    EXPECT(waitForFlag(rig.up.verifying));
    rig.svc.skipVersion("99.0.0");
    rig.up.verifyGateOpen = true;
    rig.svc.stop();
    const UpdateStatusSnapshot snap = rig.svc.snapshot();
    EXPECT(snap.state == UpdateState::Idle);
    EXPECT(snap.failedPhase == UpdateState::Idle);
}

static void test_a_check_overtaken_by_a_skip_settles_nothing() {
    TEST("skipVersion during a running check drops that check's result, so the skipped release "
         "is not offered again");
    Rig rig;
    EXPECT(freezeOffered(rig));
    rig.up.fetching = false;
    rig.up.fetchGateOpen = false;
    rig.svc.requestCheck(false);
    rig.svc.start();
    EXPECT(waitForFlag(rig.up.fetching));
    rig.svc.skipVersion("99.0.0");
    rig.up.fetchGateOpen = true;
    rig.svc.stop();
    EXPECT(rig.svc.snapshot().state == UpdateState::Idle);
}

static void test_a_failing_check_overtaken_by_a_skip_writes_no_error() {
    TEST("skipVersion during a check that then fails stays Idle, not Error(Checking)");
    Rig rig;
    EXPECT(freezeOffered(rig));
    rig.up.fetching = false;
    rig.up.fetchGateOpen = false;
    rig.up.fetchOk = false;
    rig.svc.requestCheck(true);
    rig.svc.start();
    EXPECT(waitForFlag(rig.up.fetching));
    rig.svc.skipVersion("99.0.0");
    rig.up.fetchGateOpen = true;
    rig.svc.stop();
    const UpdateStatusSnapshot snap = rig.svc.snapshot();
    EXPECT(snap.state == UpdateState::Idle);
    EXPECT(snap.failedPhase == UpdateState::Idle);
}

static void test_a_check_overtaken_by_a_skip_and_a_new_check_broadcasts_no_stale_offer() {
    TEST("a check overtaken by skipVersion and a new user check never broadcasts its stale "
         "offer");
    Rig rig;
    EXPECT(freezeOffered(rig));
    rig.up.fetching = false;
    rig.up.fetchGateOpen = false;
    rig.svc.requestCheck(false);
    rig.svc.start();
    EXPECT(waitForFlag(rig.up.fetching));
    rig.svc.skipVersion("99.0.0");
    rig.svc.requestCheck(true);
    const size_t mark = broadcastMark(rig.broadcasts);
    rig.up.fetchGateOpen = true;
    EXPECT(waitForCount(rig.up.fetchCalls, 3));
    EXPECT(frozenAt(rig, UpdateState::UpToDate));
    EXPECT(!broadcastSince(rig.broadcasts, mark, UpdateState::UpdateAvailable));
}

static void test_a_check_overtaken_by_a_skip_and_a_new_check_lets_no_download_in() {
    TEST("a check overtaken by skipVersion and a new user check settles nothing, so no download "
         "of the skipped release slips in");
    DownloadOnOffer offer;
    Rig rig;
    EXPECT(freezeOffered(rig));
    offer.svc = &rig.svc;
    rig.svc.setStatusCallback(
        [&offer](const UpdateStatusSnapshot& snap) { downloadOnOffer(offer, snap); });
    rig.up.fetching = false;
    rig.up.fetchGateOpen = false;
    rig.svc.requestCheck(false);
    rig.svc.start();
    EXPECT(waitForFlag(rig.up.fetching));
    rig.svc.skipVersion("99.0.0");
    rig.svc.requestCheck(true);
    offer.armed = true;
    rig.up.fetchGateOpen = true;
    EXPECT(waitForCount(rig.up.fetchCalls, 3));
    EXPECT(frozenAt(rig, UpdateState::UpToDate));
    EXPECT(rig.svc.snapshot().state == UpdateState::UpToDate);
    EXPECT(rig.up.downloadCalls == 0);
}

static void test_a_check_taken_after_a_skip_is_checking_from_the_take() {
    TEST("a check accepted before skipVersion is Checking from the moment the worker takes it, "
         "so a user check arriving then joins it");
    WorkerHold hold;
    Rig rig;
    EXPECT(freezeOffered(rig));
    rig.svc.requestCheck(false);
    rig.svc.skipVersion("99.0.0");
    EXPECT(rig.svc.snapshot().state == UpdateState::Idle);
    holdAt(rig, hold, {UpdateState::Checking, UpdateState::Idle});
    rig.svc.start();
    EXPECT(waitForFlag(hold.held));
    rig.svc.requestCheck(true);
    hold.released = true;
    EXPECT(waitForCount(rig.up.fetchCalls, 2));
    EXPECT(frozenAt(rig, UpdateState::UpToDate));
    EXPECT(rig.up.fetchCalls == 2);
}

static void test_a_download_cancelled_after_a_skip_and_a_new_check_leaves_the_check_alone() {
    TEST("a download cancelled after skipVersion and a new user check leaves that check's "
         "Checking alone");
    Rig rig;
    rig.up.blockDownloadUntilCancel = true;
    EXPECT(freezeOffered(rig));
    rig.svc.requestDownload();
    rig.svc.start();
    EXPECT(waitForCount(rig.up.downloadCalls, 1));
    rig.svc.skipVersion("99.0.0");
    rig.svc.requestCheck(true);
    const size_t mark = broadcastMark(rig.broadcasts);
    rig.svc.cancelInFlight();
    EXPECT(waitForCount(rig.up.fetchCalls, 2));
    EXPECT(frozenAt(rig, UpdateState::UpToDate));
    EXPECT(!broadcastSince(rig.broadcasts, mark, UpdateState::Idle));
}

static void test_a_queued_download_overtaken_by_a_skip_and_a_check_gives_way() {
    TEST("a queued download overtaken by skipVersion and a new user check gives way to the "
         "check");
    Rig rig;
    EXPECT(freezeOffered(rig));
    rig.svc.requestDownload();
    rig.svc.skipVersion("99.0.0");
    rig.svc.requestCheck(true);
    const size_t mark = broadcastMark(rig.broadcasts);
    rig.svc.start();
    EXPECT(waitForCount(rig.up.fetchCalls, 2));
    EXPECT(frozenAt(rig, UpdateState::UpToDate));
    EXPECT(!broadcastSince(rig.broadcasts, mark, UpdateState::Idle));
    EXPECT(rig.up.downloadCalls == 0);
}

static void test_auto_install_is_not_skipped_by_a_check_at_verify() {
    TEST("auto-install: a check that arrives as the download is verified does not skip the "
         "install");
    WorkerHold hold;
    Rig rig;
    rig.cfg.autoInstall = true;
    EXPECT(freezeOffered(rig));
    holdAt(rig, hold, {UpdateState::Downloaded, UpdateState::Installing});
    rig.svc.requestDownload();
    rig.svc.start();
    EXPECT(waitForFlag(hold.held));
    rig.svc.requestCheck(false);
    hold.released = true;
    EXPECT(waitForCount(rig.up.applyCalls, 1));
}

static void test_a_skip_during_a_download_stops_it_before_verify() {
    TEST("skipVersion during a running download stops it before it is verified or staged");
    WorkerHold hold;
    Rig rig;
    EXPECT(freezeAfterCheck(rig, UpdateState::UpdateAvailable));
    holdAt(rig, hold, {UpdateState::Downloading});
    rig.svc.requestDownload();
    rig.svc.start();
    EXPECT(waitForFlag(hold.held));
    rig.svc.skipVersion("99.0.0");
    hold.released = true;
    EXPECT(waitForCount(rig.up.downloadCalls, 1));
    rig.svc.requestCheck(true);
    EXPECT(waitForCount(rig.up.fetchCalls, 2));
    EXPECT(frozenAt(rig, UpdateState::UpToDate));
    EXPECT(rig.svc.snapshot().state == UpdateState::UpToDate);
    EXPECT(rig.up.verifyCalls == 0);
}

static void test_a_skip_during_verify_leaves_nothing_staged() {
    TEST("skipVersion during verify leaves nothing staged for a later check to bring back");
    Rig rig;
    EXPECT(freezeAfterCheck(rig, UpdateState::UpdateAvailable));
    rig.up.verifyGateOpen = false;
    rig.svc.requestDownload();
    rig.svc.start();
    EXPECT(waitForFlag(rig.up.verifying));
    rig.svc.skipVersion("99.0.0");
    rig.up.verifyGateOpen = true;
    rig.svc.requestCheck(true);
    EXPECT(waitForCount(rig.up.fetchCalls, 2));
    EXPECT(frozenAt(rig, UpdateState::UpToDate));
    EXPECT(rig.svc.snapshot().state == UpdateState::UpToDate);
}

static void test_a_failed_background_check_keeps_the_known_update() {
    TEST("a background check that fails while an update is known settles back at "
         "UpdateAvailable");
    Rig rig;
    EXPECT(freezeAfterCheck(rig, UpdateState::UpdateAvailable));
    rig.svc.dismiss();
    rig.up.fetchOk = false;
    rig.up.fetchError = "no network";
    rig.svc.start();
    rig.svc.requestCheck(false);
    EXPECT(waitForCount(rig.up.fetchCalls, 2));
    EXPECT(frozenAt(rig, UpdateState::UpdateAvailable));
    const UpdateStatusSnapshot snap = rig.svc.snapshot();
    EXPECT(snap.state == UpdateState::UpdateAvailable);
    EXPECT(snap.info.version == "99.0.0");
    EXPECT(snap.message.empty());
    EXPECT(snap.failedPhase == UpdateState::Idle);
    EXPECT(logged(rig.log, "Check failed: no network"));

    TEST("a failed check keeps the dismissal");
    EXPECT(snap.dismissed);
}

static void test_a_failed_user_check_still_reports_the_error() {
    TEST("a user check that fails lands in Error(Checking) even with an update known");
    Rig rig;
    EXPECT(freezeAfterCheck(rig, UpdateState::UpdateAvailable));
    rig.up.fetchOk = false;
    rig.up.fetchError = "no network";
    rig.svc.start();
    rig.svc.requestCheck(true);
    EXPECT(waitForCount(rig.up.fetchCalls, 2));
    EXPECT(frozenAt(rig, UpdateState::Error));
    const UpdateStatusSnapshot snap = rig.svc.snapshot();
    EXPECT(snap.failedPhase == UpdateState::Checking);
    EXPECT(snap.message == "no network");
    EXPECT(snap.info.version == "99.0.0");
}

static void test_a_failed_background_check_with_nothing_known_reports_the_error() {
    TEST("a background check that fails with no update known lands in Error(Checking)");
    Rig rig;
    rig.up.fetchOk = false;
    rig.svc.start();
    rig.svc.requestCheck(false);
    EXPECT(frozenAt(rig, UpdateState::Error));
    EXPECT(rig.svc.snapshot().failedPhase == UpdateState::Checking);
}

static void test_a_failed_background_check_starts_no_auto_download() {
    TEST("auto-download: a background check that fails does not download the update it kept");
    Rig rig;
    EXPECT(freezeAfterCheck(rig, UpdateState::UpdateAvailable));
    rig.cfg.autoDownload = true;
    rig.up.fetchOk = false;
    rig.svc.start();
    rig.svc.requestCheck(false);
    EXPECT(waitForCount(rig.up.fetchCalls, 2));
    rig.svc.stop();
    EXPECT(rig.svc.snapshot().state == UpdateState::UpdateAvailable);
    EXPECT(rig.up.downloadCalls == 0);
}

static void test_a_user_check_folded_into_a_queued_background_check_shows_its_failure() {
    TEST("a user check that arrives while a background check is queued has its failure shown");
    Rig rig;
    EXPECT(freezeAfterCheck(rig, UpdateState::UpdateAvailable));
    rig.up.fetchOk = false;
    rig.svc.requestCheck(false);
    rig.svc.requestCheck(true);
    rig.svc.start();
    EXPECT(waitForCount(rig.up.fetchCalls, 2));
    EXPECT(frozenAt(rig, UpdateState::Error));
    EXPECT(rig.svc.snapshot().failedPhase == UpdateState::Checking);
    EXPECT(rig.up.fetchCalls == 2);
}

static void test_a_user_check_during_a_running_background_check_shows_its_failure() {
    TEST("a user check that arrives while a background check runs has its failure shown");
    WorkerHold hold;
    Rig rig;
    EXPECT(freezeAfterCheck(rig, UpdateState::UpdateAvailable));
    rig.up.fetchOk = false;
    holdAt(rig, hold, {UpdateState::Checking});
    rig.svc.requestCheck(false);
    rig.svc.start();
    EXPECT(waitForFlag(hold.held));
    rig.svc.requestCheck(true);
    hold.released = true;
    EXPECT(waitForCount(rig.up.fetchCalls, 2));
    EXPECT(frozenAt(rig, UpdateState::Error));
    EXPECT(rig.up.fetchCalls == 2);
}

struct RelaunchCase {
    const char* label;
    void (UpdateService::*relaunch)();
};

static void test_a_failed_install_relaunches_the_staged_installer() {
    const RelaunchCase cases[] = {
        {"requestInstall: after a failed install, Installing at once and the same verified file "
         "launches again, no new download",
         &UpdateService::requestInstall},
        {"requestRetry: after a failed install, Installing at once and the same verified file "
         "launches again, no new download",
         &UpdateService::requestRetry},
    };
    for (const RelaunchCase& relaunching : cases) {
        TEST(relaunching.label);
        Rig rig;
        rig.up.applyOk = false;
        EXPECT(freezeAfterInstall(rig, UpdateState::Error));
        EXPECT(rig.svc.snapshot().failedPhase == UpdateState::Installing);
        (rig.svc.*relaunching.relaunch)();
        EXPECT(rig.svc.snapshot().state == UpdateState::Installing);
        rig.up.applyOk = true;
        rig.svc.start();
        EXPECT(waitForCount(rig.up.applyCalls, 2));
        rig.svc.stop();
        EXPECT(rig.up.downloadCalls == 1);
        EXPECT(rig.up.lastAppliedPath == "/tmp/satellite-update");
    }
}

static void test_install_after_a_failed_verify_is_rejected() {
    TEST("requestInstall: rejected after a failed verify, so an unverified file never runs");
    Rig rig;
    rig.up.verifyOk = false;
    EXPECT(freezeAfterDownload(rig, UpdateState::Error));
    rig.svc.requestInstall();
    EXPECT(rig.svc.snapshot().state == UpdateState::Error);
    rig.svc.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    rig.svc.stop();
    EXPECT(rig.up.applyCalls == 0);
}

static void test_broadcasts_carry_the_failed_phase() {
    TEST("a broadcast carries failedPhase, the same as the snapshot");
    Rig rig;
    rig.up.downloadOk = false;
    EXPECT(freezeAfterDownload(rig, UpdateState::Error));
    const UpdateStatusSnapshot last = lastBroadcast(rig.broadcasts);
    EXPECT(last.state == UpdateState::Error);
    EXPECT(last.failedPhase == UpdateState::Downloading);
}

static void test_broadcasts_carry_the_dismissal() {
    TEST("a broadcast carries dismissed, the same as the snapshot");
    Rig rig;
    EXPECT(freezeAfterCheck(rig, UpdateState::UpdateAvailable));
    rig.svc.dismiss();
    EXPECT(lastBroadcast(rig.broadcasts).dismissed);
}

static void test_progressWorthBroadcasting() {
    TEST("progress: with a known size, only a whole percent moved is worth a broadcast");
    EXPECT(!progressWorthBroadcasting(0, 1000, 5, 1000));
    EXPECT(progressWorthBroadcasting(0, 1000, 10, 1000));
    EXPECT(!progressWorthBroadcasting(10, 1000, 19, 1000));
    EXPECT(progressWorthBroadcasting(10, 1000, 20, 1000));

    TEST("progress: a size that arrives late is measured from zero percent");
    EXPECT(!progressWorthBroadcasting(5, 0, 5, 1000));
    EXPECT(progressWorthBroadcasting(5, 0, 10, 1000));

    TEST("progress: with no size, every quarter megabyte is worth one");
    EXPECT(!progressWorthBroadcasting(0, 0, 256 * 1024 - 1, 0));
    EXPECT(progressWorthBroadcasting(0, 0, 256 * 1024, 0));
    EXPECT(!progressWorthBroadcasting(256 * 1024, 0, 256 * 1024 + 100, 0));
}

int main() {
    std::cout << "Running update_service tests...\n\n";
    test_progressWorthBroadcasting();
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
    test_cancel_settles_a_running_download_back_at_the_offer();
    test_cancel_drops_a_queued_download();
    test_cancel_drops_a_queued_repair_back_to_idle();
    test_cancel_settles_a_running_repair_at_idle();
    test_manual_method_does_not_download();
    test_download_queued_before_skip_settles_without_relocking();
    test_repair_ignores_manual_install_hosts();
    test_auto_download_and_install_chain();
    test_update_preferences_persists();
    test_skip_version_clears_available();
    test_dismiss_keeps_an_available_update_on_offer();
    test_dismiss_keeps_a_downloaded_update_staged();
    test_dismiss_elsewhere_changes_nothing();
    test_a_check_that_fetched_clears_the_dismissal();
    test_an_accepted_download_or_repair_clears_the_dismissal();
    test_a_download_after_a_dismissal_lands_undismissed();
    test_an_accepted_install_clears_the_dismissal();
    test_check_is_visible_before_the_worker_runs_it();
    test_check_after_an_error_drops_it_at_once();
    test_download_is_visible_before_the_worker_runs_it();
    test_repair_of_a_known_release_is_visible_at_once();
    test_repair_with_no_known_release_is_visible_at_once();
    test_install_is_visible_at_once();
    test_check_is_rejected_while_work_is_queued();
    test_requests_outside_their_states_change_nothing();
    test_requests_do_not_broadcast();
    test_install_queued_before_skip_never_runs();
    test_retry_after_a_failed_download_or_verify_downloads_again();
    test_retry_after_a_failed_repair_repairs_again();
    test_retry_after_a_failed_check_checks_again_as_the_user();
    test_check_is_accepted_while_downloaded();
    test_a_check_that_finds_the_staged_release_keeps_it();
    test_a_check_that_finds_the_staged_repair_keeps_it();
    test_a_check_that_finds_a_rebuilt_asset_offers_it_afresh();
    test_a_failed_background_check_returns_to_downloaded();
    test_a_failed_user_check_keeps_the_staged_installer_for_the_next_check();
    test_a_check_after_a_failed_install_keeps_the_installer_reachable();
    test_a_user_check_right_after_a_repair_check_keeps_the_repair_staged();
    test_a_new_download_supersedes_the_staged_installer();
    test_skipping_the_staged_release_forgets_its_installer();
    test_a_repair_whose_check_finds_the_staged_installer_keeps_it();
    test_an_install_accepted_after_a_repair_check_ends_the_repair();
    test_a_repair_joins_a_queued_background_check();
    test_a_repair_joins_a_running_background_check();
    test_a_failed_check_under_a_joined_repair_reports_the_error();
    test_a_repair_joined_to_a_check_clears_the_dismissal();
    test_a_repair_on_a_package_managed_host_does_not_join_a_check();
    test_an_install_that_fails_after_a_skip_leaves_the_skip_alone();
    test_a_skip_during_a_download_stops_it_before_verify();
    test_a_download_that_fails_after_a_skip_leaves_the_skip_alone();
    test_a_skip_during_verify_leaves_nothing_staged();
    test_a_verify_that_fails_after_a_skip_leaves_the_skip_alone();
    test_a_check_overtaken_by_a_skip_settles_nothing();
    test_a_failing_check_overtaken_by_a_skip_writes_no_error();
    test_a_check_overtaken_by_a_skip_and_a_new_check_broadcasts_no_stale_offer();
    test_a_check_overtaken_by_a_skip_and_a_new_check_lets_no_download_in();
    test_a_check_taken_after_a_skip_is_checking_from_the_take();
    test_a_download_cancelled_after_a_skip_and_a_new_check_leaves_the_check_alone();
    test_a_queued_download_overtaken_by_a_skip_and_a_check_gives_way();
    test_auto_install_is_not_skipped_by_a_check_at_verify();
    test_a_failed_background_check_keeps_the_known_update();
    test_a_failed_user_check_still_reports_the_error();
    test_a_failed_background_check_with_nothing_known_reports_the_error();
    test_a_failed_background_check_starts_no_auto_download();
    test_a_user_check_folded_into_a_queued_background_check_shows_its_failure();
    test_a_user_check_during_a_running_background_check_shows_its_failure();
    test_a_failed_install_relaunches_the_staged_installer();
    test_install_after_a_failed_verify_is_rejected();
    test_broadcasts_carry_the_failed_phase();
    test_broadcasts_carry_the_dismissal();

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
