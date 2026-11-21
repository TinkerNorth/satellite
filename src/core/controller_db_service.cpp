// SPDX-License-Identifier: LGPL-3.0-or-later
#include "controller_db_service.h"

#include <algorithm>
#include <utility>

namespace satellite {

namespace {

struct CheckResultName {
    DbCheckResult result;
    const char* name;
};

constexpr CheckResultName CHECK_RESULT_NAMES[] = {
    {DbCheckResult::UpToDate, "upToDate"}, {DbCheckResult::Updated, "updated"},
    {DbCheckResult::Offline, "offline"},   {DbCheckResult::Invalid, "invalid"},
    {DbCheckResult::Off, "off"},
};

constexpr int64_t SECONDS_PER_HOUR = 3600;

ManifestDecision finished(DbCheckResult result) {
    ManifestDecision d;
    d.step = ManifestStep::Done;
    d.result = result;
    return d;
}

std::string fileUrl(const std::string& path) { return CONTROLLER_DB_BASE_URL + path; }

} // namespace

const char* dbCheckResultName(DbCheckResult result) {
    for (const CheckResultName& n : CHECK_RESULT_NAMES) {
        if (n.result == result) return n.name;
    }
    return "";
}

ManifestDecision decideOnManifest(const DbFetchResponse& response, long activeVersion) {
    switch (response.outcome) {
    case DbFetch::NotModified:
        return finished(DbCheckResult::UpToDate);
    case DbFetch::Failed:
        return finished(DbCheckResult::Offline);
    case DbFetch::Ok:
        break;
    }
    ManifestDecision d;
    if (parseControllerDbManifest(response.body, d.manifest) != DbParse::Ok) {
        return finished(DbCheckResult::Invalid);
    }
    if (d.manifest.dbVersion <= activeVersion) return finished(DbCheckResult::UpToDate);
    d.step = ManifestStep::Download;
    d.result = DbCheckResult::Updated;
    return d;
}

bool cacheSupersedes(SnapshotBuild cacheBuild, long cacheVersion, long bundledVersion) {
    return cacheBuild == SnapshotBuild::Ok && cacheVersion > bundledVersion;
}

int64_t nextCheckDueSeconds(bool checkedBefore, int64_t lastCheckSeconds, int intervalHours) {
    if (!checkedBefore) return CONTROLLER_DB_FIRST_CHECK_SECONDS;
    const int64_t hours = std::max<int64_t>(1, intervalHours);
    return lastCheckSeconds + hours * SECONDS_PER_HOUR;
}

ControllerDbService::ControllerDbService(IControllerDbPort& port, ILogPort& log,
                                         ConfigStore& configStore)
    : port_(port), log_(log), configStore_(configStore),
      active_(std::make_shared<ControllerDbSnapshot>()) {}

ControllerDbService::~ControllerDbService() { stop(); }

Sha256Fn ControllerDbService::digest() {
    return [this](const std::string& bytes) { return port_.sha256Hex(bytes); };
}

bool ControllerDbService::load() {
    ControllerDbFiles bundledFiles;
    auto bundled = std::make_shared<ControllerDbSnapshot>();
    const bool readable = port_.readBundled(bundledFiles);
    const SnapshotBuild bundledBuild =
        readable ? buildControllerDbSnapshot(bundledFiles, DbSource::Bundled, digest(), *bundled)
                 : SnapshotBuild::BadManifest;
    if (bundledBuild != SnapshotBuild::Ok) {
        log_.logMsg(LogLevel::ERR, "controllerdb",
                    std::string("The bundled controller database does not load (") +
                        snapshotBuildName(bundledBuild) + "); the catalog lists no USB ids");
        return false;
    }

    std::shared_ptr<const ControllerDbSnapshot> chosen = bundled;
    ControllerDbFiles cacheFiles;
    if (port_.readCache(cacheFiles)) {
        auto cache = std::make_shared<ControllerDbSnapshot>();
        const SnapshotBuild cacheBuild =
            buildControllerDbSnapshot(cacheFiles, DbSource::Cache, digest(), *cache);
        if (cacheSupersedes(cacheBuild, cache->db.dbVersion, bundled->db.dbVersion)) chosen = cache;
    }
    {
        std::lock_guard<TrackedMutex> lk(mtx_);
        bundledVersion_ = bundled->db.dbVersion;
        active_ = chosen;
    }
    log_.logMsg(LogLevel::INFO, "controllerdb",
                "Controller database " + std::to_string(chosen->db.dbVersion) + " (" +
                    dbSourceName(chosen->source) + "), " +
                    std::to_string(chosen->db.records.size()) + " records");
    return true;
}

DbCheckResult ControllerDbService::checkNow() {
    {
        std::lock_guard<TrackedMutex> lk(mtx_);
        checking_ = true;
    }
    const DbCheckResult result = runCheck();
    {
        std::lock_guard<TrackedMutex> lk(mtx_);
        checking_ = false;
    }
    return result;
}

// The steps of one check in the contract's order: the switch, the manifest,
// every listed file, the verified snapshot, the cache, the swap.
DbCheckResult ControllerDbService::runCheck() {
    if (!configStore_.current()->controllerDbUpdates) {
        settle(DbCheckResult::Off, "switched off, nothing sent");
        return DbCheckResult::Off;
    }
    std::string etag;
    long activeVersion = 0;
    ControllerDbFiles base;
    {
        std::lock_guard<TrackedMutex> lk(mtx_);
        etag = manifestETag_;
        activeVersion = active_->db.dbVersion;
    }
    const DbFetchResponse manifest = port_.fetch(fileUrl(CONTROLLER_DB_MANIFEST), etag);
    const ManifestDecision decision = decideOnManifest(manifest, activeVersion);
    if (decision.step == ManifestStep::Done) {
        if (decision.result == DbCheckResult::UpToDate && !manifest.etag.empty()) {
            std::lock_guard<TrackedMutex> lk(mtx_);
            manifestETag_ = manifest.etag;
        }
        settle(decision.result, "manifest");
        return decision.result;
    }
    base.manifest = manifest.body;
    auto next = std::make_shared<ControllerDbSnapshot>();
    const DbCheckResult downloaded = download(decision.manifest, base, *next);
    if (downloaded != DbCheckResult::Updated) {
        settle(downloaded, "download of " + std::to_string(decision.manifest.dbVersion));
        return downloaded;
    }
    if (!port_.writeCache(next->files)) {
        log_.logMsg(LogLevel::WARN, "controllerdb",
                    "Controller database " + std::to_string(next->db.dbVersion) +
                        " could not be saved; this run uses it, the next starts from the "
                        "previous copy");
    }
    {
        std::lock_guard<TrackedMutex> lk(mtx_);
        active_ = next;
        manifestETag_ = manifest.etag;
    }
    settle(DbCheckResult::Updated, "now " + std::to_string(next->db.dbVersion));
    return DbCheckResult::Updated;
}

DbCheckResult ControllerDbService::download(const ControllerDbManifest& manifest,
                                            const ControllerDbFiles& base,
                                            ControllerDbSnapshot& out) {
    ControllerDbFiles files = base;
    for (const auto& entry : manifest.files) {
        const DbFetchResponse file = port_.fetch(fileUrl(entry.first), "");
        if (file.outcome != DbFetch::Ok) return DbCheckResult::Offline;
        files.files[entry.first] = file.body;
    }
    const SnapshotBuild build = buildControllerDbSnapshot(files, DbSource::Cache, digest(), out);
    return build == SnapshotBuild::Ok ? DbCheckResult::Updated : DbCheckResult::Invalid;
}

void ControllerDbService::settle(DbCheckResult result, const std::string& detail) {
    const int64_t now = port_.nowEpoch();
    {
        std::lock_guard<TrackedMutex> lk(mtx_);
        checked_ = true;
        lastCheckEpoch_ = now;
        lastResult_ = result;
    }
    log_.logMsg(LogLevel::INFO, "controllerdb",
                std::string("Controller database check: ") + dbCheckResultName(result) + " (" +
                    detail + ")");
}

std::shared_ptr<const ControllerDbSnapshot> ControllerDbService::active() const {
    std::lock_guard<TrackedMutex> lk(mtx_);
    return active_;
}

ControllerDbStatus ControllerDbService::status() const {
    const bool enabled = configStore_.current()->controllerDbUpdates;
    std::lock_guard<TrackedMutex> lk(mtx_);
    ControllerDbStatus s;
    s.bundledVersion = bundledVersion_;
    s.activeVersion = active_->db.dbVersion;
    s.source = active_->source;
    s.enabled = enabled;
    s.checking = checking_;
    s.checked = checked_;
    s.lastCheckEpoch = lastCheckEpoch_;
    s.lastResult = lastResult_;
    return s;
}

void ControllerDbService::start() {
    std::lock_guard<std::mutex> lk(wakeMtx_);
    if (worker_.joinable()) return;
    stopping_ = false;
    startedAt_ = std::chrono::steady_clock::now();
    worker_ = std::thread(&ControllerDbService::workerLoop, this);
}

void ControllerDbService::stop() {
    {
        std::lock_guard<std::mutex> lk(wakeMtx_);
        stopping_ = true;
    }
    wakeCv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void ControllerDbService::requestCheck() {
    {
        std::lock_guard<std::mutex> lk(wakeMtx_);
        requested_ = true;
    }
    wakeCv_.notify_all();
}

// Caller holds wakeMtx_.
std::chrono::steady_clock::time_point ControllerDbService::nextDueLocked() const {
    const int hours = configStore_.current()->updateCheckIntervalHours;
    const int64_t lastSeconds =
        std::chrono::duration_cast<std::chrono::seconds>(lastAttemptAt_ - startedAt_).count();
    const int64_t dueSeconds = nextCheckDueSeconds(attempted_, lastSeconds, hours);
    return startedAt_ + std::chrono::seconds(dueSeconds);
}

// True when a check is to run now, false when the service is stopping.
bool ControllerDbService::waitForNextCheck() {
    std::unique_lock<std::mutex> lk(wakeMtx_);
    while (!stopping_) {
        const auto due = nextDueLocked();
        const bool woken = wakeCv_.wait_until(lk, due, [this] { return stopping_ || requested_; });
        if (stopping_) return false;
        const bool isDue = std::chrono::steady_clock::now() >= due;
        if (woken || isDue) {
            requested_ = false;
            attempted_ = true;
            lastAttemptAt_ = std::chrono::steady_clock::now();
            return true;
        }
    }
    return false;
}

void ControllerDbService::workerLoop() {
    ScopedThreadRole role(ThreadRole::Update);
    while (waitForNextCheck()) checkNow();
}

} // namespace satellite
