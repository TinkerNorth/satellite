// SPDX-License-Identifier: LGPL-3.0-or-later

// The future-reader set (tests/fixtures/controller-db-future, copied byte for
// byte from the fleet's EXPECT.md set): a published dbVersion 2 this schema 1
// reader must take whole, run through the production check, index, catalog
// and mirror with the shipped dbVersion 1 in force.
//
// Satellite reads records only for their names and the catalog's
// emulates.usb lists; it runs no Direct path, so every record EXPECT.md
// resolves "with that family's parser at the pad's next attach"
// (future-pad-served on xinput360, future-pad-served-ds4 on dualshock4) is,
// for this build, a named record that adds nothing to a list.
#include "../src/adapters/controller_db_adapter.h"
#include "../src/core/catalog.h"
#include "../src/core/controller_db.h"
#include "../src/core/controller_db_service.h"
#include "../src/core/json.h"

#include <sodium.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "test_util.h"

using namespace satellite;

namespace {

const char* const FUTURE_FILES[] = {
    "manifest.json",   "controllers.json",   "strings/en.json",
    "strings/bs.json", "strings/de.json",    "strings/es.json",
    "strings/fr.json", "strings/pt-BR.json", "strings/it.json",
};

std::string readFixture(const std::string& path) {
    std::ifstream f(std::string(FUTURE_DIR) + "/" + path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

struct NullLog : ILogPort {
    void logMsg(LogLevel, const std::string&, const std::string&) override {}
};

// The website serving the future set; the shipped copy as the bundle; the
// real SHA-256; a cache that records what it was asked to write.
struct FuturePort : IControllerDbPort {
    ControllerDbAdapter real{std::string(WEB_DIR) + "/controllers/v1", "", nullptr, nullptr};
    std::map<std::string, std::string> site;
    ControllerDbFiles cache;
    bool cachePresent = false;

    FuturePort() {
        for (const char* path : FUTURE_FILES) {
            site[std::string(CONTROLLER_DB_BASE_URL) + path] = readFixture(path);
        }
    }
    DbFetchResponse fetch(const std::string& url, const std::string&) override {
        DbFetchResponse r;
        const auto it = site.find(url);
        if (it == site.end()) return r;
        r.outcome = DbFetch::Ok;
        r.body = it->second;
        return r;
    }
    std::string sha256Hex(const std::string& bytes) override { return real.sha256Hex(bytes); }
    bool readBundled(ControllerDbFiles& out) override { return real.readBundled(out); }
    bool readCache(ControllerDbFiles& out) override {
        out = cache;
        return cachePresent;
    }
    bool writeCache(const ControllerDbFiles& files) override {
        cache = files;
        cachePresent = true;
        return true;
    }
    int64_t nowEpoch() override { return 1798761600; }
};

struct Fixture {
    FuturePort port;
    NullLog log;
    ConfigStore config{Config{}, nullptr};
    ControllerDbService svc{port, log, config};
};

bool lists(const std::vector<std::string>& ids, const std::string& id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

const ControllerRecord* recordById(const ControllerDb& db, const std::string& id) {
    for (const ControllerRecord& r : db.records) {
        if (r.id == id) return &r;
    }
    return nullptr;
}

} // namespace

static void test_the_whole_set() {
    Fixture f;
    EXPECT(f.svc.load());

    TEST("future set: the check ends updated and dbVersion 2 replaces dbVersion 1 whole");
    EXPECT(f.svc.checkNow() == DbCheckResult::Updated);
    const auto active = f.svc.active();
    EXPECT_EQ(active->db.dbVersion, 2L);
    EXPECT(active->source == DbSource::Cache);
    EXPECT(recordById(active->db, "xbox-elite-series-2-controller") == nullptr);

    TEST("future set: the manifest's unknown key signature does not spoil the set");
    EXPECT_EQ(active->manifest.dbVersion, 2L);

    TEST("future set: strings/it.json, a locale this build does not ship, is left out, never "
         "written");
    EXPECT(f.port.cache.files.count("strings/it.json") == 0);
    EXPECT_EQ(f.port.cache.files.size(), size_t{7});
    EXPECT(active->strings.byLocale.count("it") == 0);

    TEST("future set: controllers.json's unknown key generatedBy is ignored");
    EXPECT(active->db.records.size() >= 7);
}

static void test_record_by_record() {
    Fixture f;
    EXPECT(f.svc.load());
    EXPECT(f.svc.checkNow() == DbCheckResult::Updated);
    const ControllerDb& db = f.svc.active()->db;

    TEST("future set: sony-dualsense keeps its family and both issues, text os values and all");
    const ControllerRecord* ds = recordById(db, "sony-dualsense");
    EXPECT(ds != nullptr && ds->family == "dualsense");
    EXPECT(ds != nullptr && ds->issues.size() == 2);
    EXPECT(ds != nullptr && !ds->inert);

    TEST("future set: a record needing a family this build lacks is named and inert");
    const ControllerRecord* x360 = recordById(db, "xbox-360-controller");
    EXPECT(x360 != nullptr && x360->inert);
    const ControllerRecord* newFamily = recordById(db, "future-pad-new-family");
    EXPECT(newFamily != nullptr && newFamily->inert);

    TEST("future set: a record needing an op this build lacks is inert");
    const ControllerRecord* newOp = recordById(db, "future-pad-new-op");
    EXPECT(newOp != nullptr && newOp->inert);
    const ControllerRecord* mapping = recordById(db, "8bitdo-m30-d-input");
    EXPECT(mapping != nullptr && mapping->inert);

    TEST("future set: the two served pads on known families load, usable");
    const ControllerRecord* served = recordById(db, "future-pad-served");
    EXPECT(served != nullptr && served->family == "xinput360" && !served->inert);
    const ControllerRecord* servedDs4 = recordById(db, "future-pad-served-ds4");
    EXPECT(servedDs4 != nullptr && servedDs4->family == "dualshock4" && !servedDs4->inert);

    TEST("future set: the misauthored record is left out and takes no other record with it");
    EXPECT(recordById(db, "future-pad-misauthored") == nullptr);
    EXPECT(db.find("1209:0f04") == nullptr);
    EXPECT_EQ(db.records.size(), size_t{8});
    EXPECT_EQ(db.rejectedRecords, size_t{1});

    TEST("future set: the Bluetooth Steam Controller is as in dbVersion 1");
    const ControllerRecord* steam = db.find("28de:1106");
    EXPECT(steam != nullptr && steam->id == "valve-steam-controller-bluetooth" && !steam->inert);
}

static void test_catalog_lists() {
    Fixture f;
    EXPECT(f.svc.load());
    EXPECT(f.svc.checkNow() == DbCheckResult::Updated);
    const ControllerDb& db = f.svc.active()->db;

    TEST("future set: an inert record leaves the build's own id in force (xbox360 keeps "
         "045e:028e)");
    EXPECT(lists(catalogEmulatedUsb("xbox360", db), "045e:028e"));

    TEST("future set: no served record adds an id to a list, inert or not");
    for (const char* slug : {"xbox360", "ds4", "dualsense", "switchpro", "dualsenseedge"}) {
        for (const std::string& id : catalogEmulatedUsb(slug, db)) {
            EXPECT(id.compare(0, 5, "1209:") != 0);
        }
    }

    TEST("future set: dualsense keeps 054c:0ce6 from its usable record");
    EXPECT(lists(catalogEmulatedUsb("dualsense", db), "054c:0ce6"));
}

static void test_mirror() {
    Fixture f;
    EXPECT(f.svc.load());
    EXPECT(f.svc.checkNow() == DbCheckResult::Updated);
    const auto active = f.svc.active();

    TEST("future set: the mirror's manifest lists only files the mirror serves");
    MirrorFile manifest;
    EXPECT(controllerDbMirrorFile(*active, CONTROLLER_DB_MANIFEST, manifest));
    Json offered;
    EXPECT(manifest.bytes != nullptr && jsonParse(*manifest.bytes, offered));
    const Json files = jsonObject(offered, "files");
    EXPECT_EQ(files.size(), size_t{7});
    for (auto it = files.begin(); it != files.end(); ++it) {
        MirrorFile served;
        EXPECT(controllerDbMirrorFile(*active, it.key(), served));
    }

    TEST("future set: the mirror's manifest keeps the published keys it does not know");
    EXPECT(offered.contains("signature"));
    EXPECT_EQ(jsonInt(offered, "dbVersion"), 2L);

    TEST("future set: the mirror's manifest ETag is the SHA-256 of the bytes it serves");
    EXPECT_EQ(manifest.etag, controllerDbETag(f.port.sha256Hex(*manifest.bytes)));
}

int main() {
    return satellite::test::runMain("controller_db_future", []() -> int {
        if (sodium_init() < 0) return 1;
        test_the_whole_set();
        test_record_by_record();
        test_catalog_lists();
        test_mirror();
        return 0;
    });
}
