// SPDX-License-Identifier: LGPL-3.0-or-later

// core/controller_db_service: the manifest decision, which copy is active,
// the cadence, and the check against a mock port: off, not modified, newer,
// a torn or tampered download, and the worker's wake.
#include "../src/core/controller_db_service.h"
#include "../src/core/json.h"

#include <chrono>
#include <cstdio>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "test_util.h"
#include "../src/core/hex.h"

using namespace satellite;

namespace {

// Deterministic and different for different bytes, which is all the
// manifest check needs to see.
std::string fakeSha(const std::string& bytes) {
    const std::string part =
        hexEncodeBE32(static_cast<uint32_t>(std::hash<std::string>{}(bytes) >> 32U)) +
        hexEncodeBE32(static_cast<uint32_t>(std::hash<std::string>{}(bytes)));
    return part + part + part + part;
}

std::string recordsText(long dbVersion, const std::string& usbId) {
    JsonOut r;
    r["schema"] = 1;
    r["id"] = "sony-dualsense";
    r["requires"] = {{"families", {"dualsense"}}, {"ops", JsonOut::array()}};
    r["identity"] = {{"usb", {usbId}}, {"bluetooth", JsonOut::array()}};
    r["input"] = {{"protocol", {{"ref", "dualsense"}}}};
    r["output"] = JsonOut::object();
    r["expect"] = JsonOut::object();
    r["support"] = {{"verification", {{"state", "verified"}}}};
    JsonOut j;
    j["schema"] = 1;
    j["dbVersion"] = dbVersion;
    j["records"] = JsonOut::array({r});
    return jsonDump(j);
}

// One published copy: its files and the manifest that pins them.
ControllerDbFiles makeCopy(long dbVersion, const std::string& usbId = "054c:0ce6") {
    ControllerDbFiles c;
    c.files["controllers.json"] = recordsText(dbVersion, usbId);
    c.files["strings/en.json"] = R"({"controller.sony-dualsense.name":"DualSense"})";
    c.files["strings/de.json"] = R"({"family.dualsense.name":"DualSense-Protokoll"})";
    JsonOut m;
    m["schema"] = 1;
    m["dbVersion"] = dbVersion;
    for (const auto& f : c.files) {
        m["files"][f.first] = {{"sha256", fakeSha(f.second)}, {"bytes", f.second.size()}};
    }
    c.manifest = jsonDump(m);
    return c;
}

struct MockLog : ILogPort {
    std::mutex m;
    std::vector<std::pair<LogLevel, std::string>> lines;
    void logMsg(LogLevel level, const std::string&, const std::string& message) override {
        std::lock_guard<std::mutex> lk(m);
        lines.emplace_back(level, message);
    }
    int count(LogLevel level) {
        std::lock_guard<std::mutex> lk(m);
        int n = 0;
        for (const auto& l : lines) n += l.first == level ? 1 : 0;
        return n;
    }
};

// The website as a map from URL to what it answers, plus the two copies on
// disk. Everything a check does is recorded.
struct MockDbPort : IControllerDbPort {
    struct Served {
        std::string body;
        std::string etag;
    };
    std::mutex m;
    std::map<std::string, Served> site;
    bool online = true;
    ControllerDbFiles bundled = makeCopy(1);
    bool bundledReadable = true;
    ControllerDbFiles cache;
    bool cachePresent = false;
    bool writeSucceeds = true;
    int writes = 0;
    std::vector<std::string> fetched;
    std::vector<std::string> ifNoneMatch;
    int64_t now = 1763500000;

    void publish(const ControllerDbFiles& copy, const std::string& etag = "\"m1\"") {
        std::lock_guard<std::mutex> lk(m);
        site.clear();
        site[std::string(CONTROLLER_DB_BASE_URL) + "manifest.json"] = {copy.manifest, etag};
        for (const auto& f : copy.files) {
            site[std::string(CONTROLLER_DB_BASE_URL) + f.first] = {f.second, ""};
        }
    }

    DbFetchResponse fetch(const std::string& url, const std::string& inm) override {
        std::lock_guard<std::mutex> lk(m);
        fetched.push_back(url);
        ifNoneMatch.push_back(inm);
        DbFetchResponse r;
        const auto it = site.find(url);
        if (!online || it == site.end()) return r;
        if (!inm.empty() && inm == it->second.etag) {
            r.outcome = DbFetch::NotModified;
            return r;
        }
        r.outcome = DbFetch::Ok;
        r.body = it->second.body;
        r.etag = it->second.etag;
        return r;
    }
    std::string sha256Hex(const std::string& bytes) override { return fakeSha(bytes); }
    bool readBundled(ControllerDbFiles& out) override {
        out = bundled;
        return bundledReadable;
    }
    bool readCache(ControllerDbFiles& out) override {
        out = cache;
        return cachePresent;
    }
    bool writeCache(const ControllerDbFiles& files) override {
        writes++;
        if (!writeSucceeds) return false;
        cache = files;
        cachePresent = true;
        return true;
    }
    int64_t nowEpoch() override { return now; }
    size_t fetchCount() {
        std::lock_guard<std::mutex> lk(m);
        return fetched.size();
    }
};

// The service with its port, its log and a config whose switch a test sets.
struct Fixture {
    MockDbPort port;
    MockLog log;
    ConfigStore config{Config{}, nullptr};
    ControllerDbService svc{port, log, config};

    void setEnabled(bool on) {
        config.update([on](Config& c) {
            c.controllerDbUpdates = on;
            return true;
        });
    }
};

} // namespace

static void test_manifest_decision() {
    const std::string newer = makeCopy(3).manifest;

    TEST("a 304 for the manifest is up to date and downloads nothing");
    DbFetchResponse notModified;
    notModified.outcome = DbFetch::NotModified;
    ManifestDecision d = decideOnManifest(notModified, 1);
    EXPECT(d.step == ManifestStep::Done);
    EXPECT(d.result == DbCheckResult::UpToDate);

    TEST("a manifest that cannot be fetched is offline");
    d = decideOnManifest(DbFetchResponse{}, 1);
    EXPECT(d.step == ManifestStep::Done);
    EXPECT(d.result == DbCheckResult::Offline);

    TEST("a manifest that does not parse, or is not schema 1, is invalid");
    DbFetchResponse garbled{DbFetch::Ok, "{", ""};
    EXPECT(decideOnManifest(garbled, 1).result == DbCheckResult::Invalid);
    DbFetchResponse schema2{DbFetch::Ok, R"({"schema":2,"dbVersion":9,"files":{}})", ""};
    d = decideOnManifest(schema2, 1);
    EXPECT(d.step == ManifestStep::Done);
    EXPECT(d.result == DbCheckResult::Invalid);

    TEST("a manifest at or below the active dbVersion is up to date");
    DbFetchResponse same{DbFetch::Ok, newer, ""};
    d = decideOnManifest(same, 3);
    EXPECT(d.step == ManifestStep::Done);
    EXPECT(d.result == DbCheckResult::UpToDate);
    EXPECT(decideOnManifest(same, 4).result == DbCheckResult::UpToDate);

    TEST("a manifest above the active dbVersion is downloaded");
    d = decideOnManifest(same, 2);
    EXPECT(d.step == ManifestStep::Download);
    EXPECT_EQ(d.manifest.dbVersion, 3L);
    EXPECT_EQ(d.manifest.files.size(), size_t{3});
}

static void test_cache_supersedes() {
    TEST("the cache is active only when it builds and its dbVersion is greater");
    EXPECT(cacheSupersedes(SnapshotBuild::Ok, 2, 1));
    EXPECT(!cacheSupersedes(SnapshotBuild::Ok, 1, 1));
    EXPECT(!cacheSupersedes(SnapshotBuild::Ok, 1, 2));
    EXPECT(!cacheSupersedes(SnapshotBuild::BadFile, 5, 1));
    EXPECT(!cacheSupersedes(SnapshotBuild::BadManifest, 5, 1));
}

static void test_cadence() {
    TEST("the first automatic check is ten seconds after start");
    EXPECT_EQ(nextCheckDueSeconds(false, 0, 6), int64_t{10});

    TEST("later checks follow updateCheckIntervalHours after the last one");
    EXPECT_EQ(nextCheckDueSeconds(true, 100, 6), int64_t{100 + 6 * 3600});
    EXPECT_EQ(nextCheckDueSeconds(true, 100, 24), int64_t{100 + 24 * 3600});

    TEST("an interval under an hour counts as an hour");
    EXPECT_EQ(nextCheckDueSeconds(true, 100, 0), int64_t{100 + 3600});
    EXPECT_EQ(nextCheckDueSeconds(true, 100, -3), int64_t{100 + 3600});
}

static void test_result_names() {
    TEST("the check results carry the bundle's names");
    EXPECT_EQ(std::string(dbCheckResultName(DbCheckResult::UpToDate)), std::string("upToDate"));
    EXPECT_EQ(std::string(dbCheckResultName(DbCheckResult::Updated)), std::string("updated"));
    EXPECT_EQ(std::string(dbCheckResultName(DbCheckResult::Offline)), std::string("offline"));
    EXPECT_EQ(std::string(dbCheckResultName(DbCheckResult::Invalid)), std::string("invalid"));
    EXPECT_EQ(std::string(dbCheckResultName(DbCheckResult::Off)), std::string("off"));
}

static void test_load() {
    TEST("with no cache the bundled copy is active");
    {
        Fixture f;
        EXPECT(f.svc.load());
        EXPECT(f.svc.active()->source == DbSource::Bundled);
        EXPECT_EQ(f.svc.active()->db.dbVersion, 1L);
        const ControllerDbStatus s = f.svc.status();
        EXPECT_EQ(s.bundledVersion, 1L);
        EXPECT_EQ(s.activeVersion, 1L);
        EXPECT(!s.checked);
    }

    TEST("a newer cache replaces the bundled copy whole");
    {
        Fixture f;
        f.port.cache = makeCopy(2, "1234:5678");
        f.port.cachePresent = true;
        EXPECT(f.svc.load());
        EXPECT(f.svc.active()->source == DbSource::Cache);
        EXPECT_EQ(f.svc.status().activeVersion, 2L);
        EXPECT(f.svc.active()->db.find("1234:5678") != nullptr);
        EXPECT(f.svc.active()->db.find("054c:0ce6") == nullptr);
    }

    TEST("a cache at the bundled dbVersion or below stays unused");
    {
        Fixture f;
        f.port.bundled = makeCopy(2);
        f.port.cache = makeCopy(2, "1234:5678");
        f.port.cachePresent = true;
        EXPECT(f.svc.load());
        EXPECT(f.svc.active()->source == DbSource::Bundled);
    }

    TEST("a cache torn between two versions fails its manifest and stays unused");
    {
        Fixture f;
        f.port.cache = makeCopy(2, "1234:5678");
        f.port.cache.files["controllers.json"] = makeCopy(3).files["controllers.json"];
        f.port.cachePresent = true;
        EXPECT(f.svc.load());
        EXPECT(f.svc.active()->source == DbSource::Bundled);
        EXPECT_EQ(f.svc.status().activeVersion, 1L);
    }

    TEST("a bundled copy that does not build fails the load and leaves an empty database");
    {
        Fixture f;
        f.port.bundled.files["controllers.json"] += ' ';
        EXPECT(!f.svc.load());
        EXPECT(f.svc.active()->db.records.empty());
        EXPECT(f.log.count(LogLevel::ERR) > 0);
    }
}

static void test_check_off() {
    TEST("with the switch off a check sends nothing and records off");
    Fixture f;
    f.svc.load();
    f.setEnabled(false);
    f.port.publish(makeCopy(2));
    EXPECT(f.svc.checkNow() == DbCheckResult::Off);
    EXPECT_EQ(f.port.fetchCount(), size_t{0});
    const ControllerDbStatus s = f.svc.status();
    EXPECT(s.checked);
    EXPECT(!s.enabled);
    EXPECT(s.lastResult == DbCheckResult::Off);
    EXPECT_EQ(s.lastCheckEpoch, f.port.now);
    EXPECT_EQ(s.activeVersion, 1L);
}

static void test_check_up_to_date() {
    TEST("a manifest at the active dbVersion is up to date and fetches nothing else");
    Fixture f;
    f.svc.load();
    f.port.publish(makeCopy(1), "\"v1\"");
    EXPECT(f.svc.checkNow() == DbCheckResult::UpToDate);
    EXPECT_EQ(f.port.fetchCount(), size_t{1});
    EXPECT_EQ(f.port.ifNoneMatch[0], std::string(""));

    TEST("the next check sends the manifest's ETag and a 304 is up to date");
    EXPECT(f.svc.checkNow() == DbCheckResult::UpToDate);
    EXPECT_EQ(f.port.fetchCount(), size_t{2});
    EXPECT_EQ(f.port.ifNoneMatch[1], std::string("\"v1\""));
    EXPECT_EQ(f.port.writes, 0);
}

static void test_check_updates() {
    TEST("a newer copy is downloaded, verified, written to the cache and made active");
    Fixture f;
    f.svc.load();
    const ControllerDbFiles v2 = makeCopy(2, "1234:5678");
    f.port.publish(v2);
    EXPECT(f.svc.checkNow() == DbCheckResult::Updated);
    EXPECT_EQ(f.port.writes, 1);
    EXPECT(f.port.cache.manifest == v2.manifest);
    EXPECT(f.port.cache.files == v2.files);
    EXPECT(f.svc.active()->source == DbSource::Cache);
    EXPECT(f.svc.active()->db.find("1234:5678") != nullptr);
    const ControllerDbStatus s = f.svc.status();
    EXPECT_EQ(s.activeVersion, 2L);
    EXPECT_EQ(s.bundledVersion, 1L);
    EXPECT(s.lastResult == DbCheckResult::Updated);

    TEST("every request is the base URL and a manifest path, with no query");
    EXPECT_EQ(f.port.fetchCount(), size_t{4});
    for (const std::string& url : f.port.fetched) {
        EXPECT(url.rfind(CONTROLLER_DB_BASE_URL, 0) == 0);
        EXPECT(url.find('?') == std::string::npos);
    }

    TEST("a check after the update finds it up to date");
    EXPECT(f.svc.checkNow() == DbCheckResult::UpToDate);
}

static void test_check_failures_keep_the_active_copy() {
    TEST("a file whose bytes do not match the manifest is invalid and changes nothing");
    {
        Fixture f;
        f.svc.load();
        ControllerDbFiles tampered = makeCopy(2);
        const std::string manifest = tampered.manifest;
        tampered.files["strings/de.json"] = R"({"family.dualsense.name":"x"})";
        f.port.publish(tampered);
        f.port.site[std::string(CONTROLLER_DB_BASE_URL) + "manifest.json"].body = manifest;
        EXPECT(f.svc.checkNow() == DbCheckResult::Invalid);
        EXPECT_EQ(f.port.writes, 0);
        EXPECT_EQ(f.svc.status().activeVersion, 1L);
        EXPECT(f.svc.status().lastResult == DbCheckResult::Invalid);
    }

    TEST("a file that cannot be fetched is offline and changes nothing");
    {
        Fixture f;
        f.svc.load();
        f.port.publish(makeCopy(2));
        f.port.site.erase(std::string(CONTROLLER_DB_BASE_URL) + "strings/en.json");
        EXPECT(f.svc.checkNow() == DbCheckResult::Offline);
        EXPECT_EQ(f.port.writes, 0);
        EXPECT_EQ(f.svc.status().activeVersion, 1L);
    }

    TEST("no network is offline");
    {
        Fixture f;
        f.svc.load();
        f.port.online = false;
        EXPECT(f.svc.checkNow() == DbCheckResult::Offline);
        EXPECT(f.svc.status().lastResult == DbCheckResult::Offline);
    }

    TEST("a copy that verifies but cannot be saved is used for this run and warned about");
    {
        Fixture f;
        f.svc.load();
        f.port.writeSucceeds = false;
        f.port.publish(makeCopy(2));
        EXPECT(f.svc.checkNow() == DbCheckResult::Updated);
        EXPECT_EQ(f.svc.status().activeVersion, 2L);
        EXPECT(!f.port.cachePresent);
        EXPECT(f.log.count(LogLevel::WARN) > 0);
    }
}

static bool waitForFetches(MockDbPort& port, size_t n) {
    for (int i = 0; i < 200; i++) {
        if (port.fetchCount() >= n) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

static void test_worker_runs_a_requested_check() {
    TEST("requestCheck wakes the worker for one check, and stop joins it");
    Fixture f;
    f.svc.load();
    f.port.publish(makeCopy(1));
    f.svc.start();
    f.svc.requestCheck();
    EXPECT(waitForFetches(f.port, 1));
    f.svc.stop();
    EXPECT(f.svc.status().checked);
    f.svc.stop();
}

int main() {
    return satellite::test::runMain("controller_db_service", [] {
        test_manifest_decision();
        test_cache_supersedes();
        test_cadence();
        test_result_names();
        test_load();
        test_check_off();
        test_check_up_to_date();
        test_check_updates();
        test_check_failures_keep_the_active_copy();
        test_worker_runs_a_requested_check();
    });
}
