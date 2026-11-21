// SPDX-License-Identifier: LGPL-3.0-or-later

// core/controller_db: the schema-1 parser, the index, the strings fallback,
// the manifest and the snapshot it all builds, against fixtures for each
// branch and against the copy web/controllers/v1 ships.
#include "../src/core/controller_db.h"
#include "../src/core/json.h"

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "test_util.h"

using namespace satellite;

namespace {

std::string readFileAll(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return "";
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::string bundledPath(const std::string& name) {
    return std::string(WEB_DIR) + "/controllers/v1/" + name;
}

// A record carrying every section schema 1 requires, as JsonOut so a test can
// drop or bend one field and dump it.
JsonOut fullRecord(const std::string& id, const std::string& usbId) {
    JsonOut r;
    r["schema"] = 1;
    r["id"] = id;
    r["requires"] = {{"families", {"dualsense"}}, {"ops", JsonOut::array()}};
    r["identity"] = {{"usb", {usbId}}, {"bluetooth", JsonOut::array()}};
    r["input"] = {{"protocol", {{"ref", "dualsense"}, {"streamsUnprompted", true}}},
                  {"direct", {{"rule", "auto"}}},
                  {"capabilities", JsonOut::object()}};
    r["output"] = {{"route", "interruptOut"},
                   {"init", {{"kind", "none"}}},
                   {"capabilities", JsonOut::object()}};
    r["expect"] = {{"endpoints", {{"in", true}, {"out", true}}}};
    r["support"] = {{"verification", {{"state", "verified"}, {"transport", "usb"}}},
                    {"issues", JsonOut::array()},
                    {"sources", JsonOut::array()}};
    return r;
}

std::string dbText(const std::vector<JsonOut>& records, long schema = 1, long dbVersion = 7) {
    JsonOut j;
    j["schema"] = schema;
    j["dbVersion"] = dbVersion;
    j["records"] = JsonOut::array();
    for (const JsonOut& r : records) j["records"].push_back(r);
    return jsonDump(j);
}

// The fake digest a snapshot test hands in: deterministic, and different for
// different bytes, which is all the manifest check needs to see.
std::string fakeSha(const std::string& bytes) {
    return std::string(64, static_cast<char>('a' + (bytes.size() % 6)));
}

struct SnapshotFixture {
    ControllerDbFiles files;

    SnapshotFixture() {
        const std::string records = dbText({fullRecord("sony-dualsense", "054c:0ce6")}, 1, 3);
        const std::string en = R"({"controller.sony-dualsense.name":"DualSense"})";
        files.files["controllers.json"] = records;
        files.files["strings/en.json"] = en;
        JsonOut m;
        m["schema"] = 1;
        m["dbVersion"] = 3;
        m["publishedAt"] = "2026-11-19T00:00:00Z";
        m["files"]["controllers.json"] = {{"sha256", fakeSha(records)}, {"bytes", records.size()}};
        m["files"]["strings/en.json"] = {{"sha256", fakeSha(en)}, {"bytes", en.size()}};
        files.manifest = jsonDump(m);
    }
};

} // namespace

static void test_bundled_copy_parses_whole() {
    TEST("the bundled controllers.json parses to 272 records, none rejected");
    ControllerDb db;
    EXPECT(parseControllerDb(readFileAll(bundledPath("controllers.json")), db) == DbParse::Ok);
    EXPECT_EQ(db.records.size(), size_t{272});
    EXPECT_EQ(db.rejectedRecords, size_t{0});
    EXPECT_EQ(db.dbVersion, 1L);
}

static void test_index_resolves_usb_ids() {
    TEST("the index resolves a usb vid:pid to its record with family, state and issues");
    ControllerDb db;
    parseControllerDb(readFileAll(bundledPath("controllers.json")), db);
    const ControllerRecord* ds = db.find("054c:0ce6");
    EXPECT(ds != nullptr);
    if (ds == nullptr) return;
    EXPECT_EQ(ds->id, std::string("sony-dualsense"));
    EXPECT_EQ(ds->family, std::string("dualsense"));
    EXPECT(ds->verification == Verification::Verified);
    EXPECT_EQ(ds->issues.size(), size_t{1});
    if (ds->issues.size() != 1) return;
    EXPECT_EQ(ds->issues[0].id, std::string("thor-ds5-rumble-usb"));
    EXPECT_EQ(ds->issues[0].transport, std::string("usb"));
    EXPECT_EQ(ds->issues[0].lane, std::string("output.rumble"));
    EXPECT_EQ(ds->issues[0].status, std::string("open"));
    EXPECT_EQ(ds->issues[0].device, std::string("AYN Thor"));
}

static void test_index_resolves_bluetooth_ids() {
    TEST("the index resolves a bluetooth vid:pid too");
    ControllerDb db;
    parseControllerDb(readFileAll(bundledPath("controllers.json")), db);
    const ControllerRecord* sc = db.find("28de:1106");
    EXPECT(sc != nullptr);
    if (sc != nullptr) EXPECT_EQ(sc->bluetooth.size(), size_t{1});
}

static void test_index_keeps_every_record_of_a_shared_id() {
    TEST("an id two records carry indexes both, and find returns the first in file order");
    ControllerDb db;
    const std::string text =
        dbText({fullRecord("first-pad", "24c6:551a"), fullRecord("other-pad", "1234:5678"),
                fullRecord("second-pad", "24c6:551a")});
    EXPECT(parseControllerDb(text, db) == DbParse::Ok);
    EXPECT_EQ(db.findAll("24c6:551a").size(), size_t{2});
    const ControllerRecord* first = db.find("24c6:551a");
    EXPECT(first != nullptr && first->id == "first-pad");
}

static void test_unknown_id_finds_nothing() {
    TEST("an id no record carries finds nothing");
    ControllerDb db;
    parseControllerDb(dbText({fullRecord("a-pad", "1234:5678")}), db);
    EXPECT(db.find("ffff:ffff") == nullptr);
    EXPECT(db.findAll("ffff:ffff").empty());
}

static void test_document_shape_errors() {
    TEST("text that is not a JSON object is malformed");
    ControllerDb db;
    EXPECT(parseControllerDb("not json", db) == DbParse::Malformed);
    EXPECT(parseControllerDb("[1,2]", db) == DbParse::Malformed);

    TEST("schema other than 1, or none, is refused as the wrong schema");
    EXPECT(parseControllerDb(dbText({fullRecord("a-pad", "1234:5678")}, 2), db) ==
           DbParse::WrongSchema);
    EXPECT(parseControllerDb(R"({"dbVersion":1,"records":[]})", db) == DbParse::WrongSchema);

    TEST("records that are not an array are malformed");
    EXPECT(parseControllerDb(R"({"schema":1,"dbVersion":1,"records":{}})", db) ==
           DbParse::Malformed);

    TEST("a dbVersion that is absent or below 1 is malformed");
    EXPECT(parseControllerDb(R"({"schema":1,"records":[]})", db) == DbParse::Malformed);
    EXPECT(parseControllerDb(dbText({}, 1, 0), db) == DbParse::Malformed);
}

static void test_unknown_fields_are_ignored() {
    TEST("fields schema 1 does not name, at the top and in a record, are ignored");
    JsonOut r = fullRecord("a-pad", "1234:5678");
    r["output"]["templates"] = {{"rumble", "00ff"}};
    r["lifecycle"] = {{"onAttach", "x"}};
    JsonOut j;
    j["schema"] = 1;
    j["dbVersion"] = 2;
    j["signature"] = "reserved";
    j["records"] = JsonOut::array({r});
    ControllerDb db;
    EXPECT(parseControllerDb(jsonDump(j), db) == DbParse::Ok);
    EXPECT_EQ(db.records.size(), size_t{1});
    EXPECT_EQ(db.rejectedRecords, size_t{0});
}

static void test_record_missing_a_required_section_is_dropped() {
    const char* const sections[] = {"schema", "id",     "requires", "identity",
                                    "input",  "output", "expect",   "support"};
    for (const char* section : sections) {
        TEST(std::string("a record without '") + section + "' is dropped and the rest are kept");
        JsonOut broken = fullRecord("broken-pad", "1111:2222");
        broken.erase(section);
        ControllerDb db;
        const std::string text = dbText(
            {fullRecord("good-pad", "1234:5678"), broken, fullRecord("next-pad", "abcd:0001")});
        EXPECT(parseControllerDb(text, db) == DbParse::Ok);
        EXPECT_EQ(db.records.size(), size_t{2});
        EXPECT_EQ(db.rejectedRecords, size_t{1});
        EXPECT(db.find("1111:2222") == nullptr);
        EXPECT(db.find("abcd:0001") != nullptr);
    }
}

// A record whose required section is present but cannot be read the way
// Satellite reads it.
static JsonOut withBadId() {
    JsonOut r = fullRecord("x", "1111:2222");
    r["id"] = "Not A Slug";
    return r;
}

static JsonOut withBadUsbId() {
    JsonOut r = fullRecord("bad-usb", "1111:2222");
    r["identity"]["usb"] = {"1111-2222"};
    return r;
}

static JsonOut withUppercaseUsbId() {
    JsonOut r = fullRecord("upper-usb", "1111:2222");
    r["identity"]["usb"] = {"ABCD:2222"};
    return r;
}

static JsonOut withoutFamily() {
    JsonOut r = fullRecord("no-family", "1111:2222");
    r["input"]["protocol"].erase("ref");
    return r;
}

static JsonOut withUnknownState() {
    JsonOut r = fullRecord("odd-state", "1111:2222");
    r["support"]["verification"]["state"] = "trusted";
    return r;
}

static JsonOut withRecordSchema2() {
    JsonOut r = fullRecord("schema-two", "1111:2222");
    r["schema"] = 2;
    return r;
}

static JsonOut withoutBluetoothList() {
    JsonOut r = fullRecord("no-bt", "1111:2222");
    r["identity"].erase("bluetooth");
    return r;
}

static void test_record_with_unreadable_section_is_dropped() {
    struct Case {
        const char* name;
        JsonOut (*make)();
    };
    const Case cases[] = {
        {"an id that is not a slug", withBadId},
        {"a usb id not in vid:pid form", withBadUsbId},
        {"a usb id in uppercase", withUppercaseUsbId},
        {"no input.protocol.ref", withoutFamily},
        {"a verification state schema 1 does not name", withUnknownState},
        {"a record schema other than 1", withRecordSchema2},
        {"identity without its bluetooth list", withoutBluetoothList},
    };
    for (const Case& c : cases) {
        TEST(std::string("a record with ") + c.name + " is dropped and the rest are kept");
        ControllerDb db;
        EXPECT(parseControllerDb(dbText({fullRecord("good-pad", "1234:5678"), c.make()}), db) ==
               DbParse::Ok);
        EXPECT_EQ(db.records.size(), size_t{1});
        EXPECT_EQ(db.rejectedRecords, size_t{1});
    }
}

static void test_issue_without_id_is_skipped() {
    TEST("an issue without an id is skipped and its record kept");
    JsonOut r = fullRecord("a-pad", "1234:5678");
    r["support"]["issues"] = JsonOut::array({{{"transport", "usb"}, {"lane", "input"}},
                                             {{"id", "kept-issue"},
                                              {"transport", "bluetooth"},
                                              {"lane", "input"},
                                              {"status", "fixed"}}});
    ControllerDb db;
    parseControllerDb(dbText({r}), db);
    EXPECT_EQ(db.records.size(), size_t{1});
    if (db.records.size() != 1) return;
    EXPECT_EQ(db.records[0].issues.size(), size_t{1});
    if (db.records[0].issues.size() == 1) {
        EXPECT_EQ(db.records[0].issues[0].id, std::string("kept-issue"));
        EXPECT_EQ(db.records[0].issues[0].device, std::string(""));
    }
}

static void test_verification_names_round_trip() {
    TEST("every verification state reads back from its name");
    for (const VerificationName& v : VERIFICATION_NAMES) {
        Verification parsed = Verification::Flagged;
        EXPECT(parseVerification(v.name, parsed));
        EXPECT(parsed == v.state);
        EXPECT_EQ(std::string(verificationName(v.state)), std::string(v.name));
    }
    Verification unused = Verification::Verified;
    EXPECT(!parseVerification("trusted", unused));
}

static void test_strings_parse_and_fallback() {
    TEST("a strings file is a flat object; values that are not strings are skipped");
    ControllerDbStringTable table;
    EXPECT(parseControllerDbStrings(R"({"a.name":"A","b.name":3})", table) == DbParse::Ok);
    EXPECT_EQ(table.size(), size_t{1});
    EXPECT(parseControllerDbStrings("[]", table) == DbParse::Malformed);
    EXPECT(parseControllerDbStrings("nope", table) == DbParse::Malformed);

    TEST("lookup reads the locale, then en, then nothing");
    ControllerDbStrings strings;
    strings.byLocale["en"] = {{"family.dualsense.name", "DualSense protocol"},
                              {"controller.sony-dualsense.name", "DualSense"}};
    strings.byLocale["de"] = {{"family.dualsense.name", "DualSense-Protokoll"}};
    EXPECT_EQ(strings.lookup("de", "family.dualsense.name"), std::string("DualSense-Protokoll"));
    EXPECT_EQ(strings.lookup("de", "controller.sony-dualsense.name"), std::string("DualSense"));
    EXPECT_EQ(strings.lookup("ja", "controller.sony-dualsense.name"), std::string("DualSense"));
    EXPECT_EQ(strings.lookup("de", "issue.none.title"), std::string(""));
}

static void test_bundled_strings_fall_back_to_en() {
    TEST("the bundled de strings name the family and fall back to en for a controller name");
    ControllerDbStrings strings;
    for (const char* locale : CONTROLLER_DB_LOCALES) {
        ControllerDbStringTable table;
        const std::string path = bundledPath(std::string("strings/") + locale + ".json");
        EXPECT(parseControllerDbStrings(readFileAll(path), table) == DbParse::Ok);
        strings.byLocale[locale] = table;
    }
    EXPECT_EQ(strings.lookup("de", "family.xinput360.name"), std::string("Xbox-360-Protokoll"));
    EXPECT_EQ(strings.lookup("de", "controller.sony-dualsense.name"),
              strings.lookup("en", "controller.sony-dualsense.name"));
    EXPECT(!strings.lookup("de", "controller.sony-dualsense.name").empty());
}

static void test_manifest_paths() {
    TEST("the manifest's file paths are the ones the contract defines, nothing else");
    EXPECT(isControllerDbFilePath("controllers.json"));
    for (const char* locale : CONTROLLER_DB_LOCALES) {
        EXPECT(isControllerDbFilePath(std::string("strings/") + locale + ".json"));
    }
    EXPECT(!isControllerDbFilePath("strings/ja.json"));
    EXPECT(!isControllerDbFilePath("../config.json"));
    EXPECT(!isControllerDbFilePath("strings/../../config.json"));
    EXPECT(!isControllerDbFilePath("manifest.json"));
    EXPECT(!isControllerDbFilePath("schema/controller-db.schema.json"));
}

static void test_manifest_parse() {
    TEST("the bundled manifest parses with the seven files it lists");
    ControllerDbManifest m;
    EXPECT(parseControllerDbManifest(readFileAll(bundledPath("manifest.json")), m) == DbParse::Ok);
    EXPECT_EQ(m.dbVersion, 1L);
    EXPECT_EQ(m.files.size(), size_t{7});
    EXPECT_EQ(m.files["controllers.json"].bytes, uint64_t{369732});

    TEST("a manifest entry for a path the contract does not define is ignored");
    const std::string sha(64, 'a');
    const std::string base =
        R"({"schema":1,"dbVersion":2,"files":{"controllers.json":{"sha256":")" + sha +
        R"(","bytes":1},"strings/en.json":{"sha256":")" + sha + R"(","bytes":1})";
    EXPECT(parseControllerDbManifest(base + R"(,"../evil":{"sha256":")" + sha + R"(","bytes":1}}})",
                                     m) == DbParse::Ok);
    EXPECT_EQ(m.files.size(), size_t{2});

    TEST("a manifest with schema other than 1 is the wrong schema");
    EXPECT(parseControllerDbManifest(R"({"schema":2,"dbVersion":2,"files":{}})", m) ==
           DbParse::WrongSchema);

    TEST("a manifest without controllers.json or strings/en.json is malformed");
    EXPECT(parseControllerDbManifest(
               R"({"schema":1,"dbVersion":2,"files":{"controllers.json":{"sha256":")" + sha +
                   R"(","bytes":1}}})",
               m) == DbParse::Malformed);

    TEST("a digest that is not 64 lowercase hex digits, or bytes that are not a count, is "
         "malformed");
    EXPECT(parseControllerDbManifest(base + R"(,"strings/de.json":{"sha256":"ABC","bytes":1}}})",
                                     m) == DbParse::Malformed);
    EXPECT(parseControllerDbManifest(
               base + R"(,"strings/de.json":{"sha256":")" + sha + R"(","bytes":"1"}}})", m) ==
           DbParse::Malformed);

    TEST("a manifest with no dbVersion is malformed");
    std::string unversioned = base + "}}";
    unversioned.replace(unversioned.find(R"("dbVersion":2,)"), 14, "");
    EXPECT(parseControllerDbManifest(base + "}}", m) == DbParse::Ok);
    EXPECT(parseControllerDbManifest(unversioned, m) == DbParse::Malformed);
}

static void test_file_check() {
    TEST("a file is checked for presence, then size, then digest");
    const std::string bytes = "abc";
    const ManifestFile good{fakeSha(bytes), 3};
    EXPECT(checkControllerDbFile(good, &bytes, fakeSha) == FileCheck::Ok);
    EXPECT(checkControllerDbFile(good, nullptr, fakeSha) == FileCheck::Missing);
    const ManifestFile wrongSize{fakeSha(bytes), 4};
    EXPECT(checkControllerDbFile(wrongSize, &bytes, fakeSha) == FileCheck::SizeMismatch);
    const ManifestFile wrongHash{std::string(64, 'f'), 3};
    EXPECT(checkControllerDbFile(wrongHash, &bytes, fakeSha) == FileCheck::HashMismatch);
}

static void test_snapshot_builds() {
    TEST("a copy whose files match its manifest builds a snapshot");
    SnapshotFixture f;
    ControllerDbSnapshot snap;
    EXPECT(buildControllerDbSnapshot(f.files, DbSource::Cache, fakeSha, snap) == SnapshotBuild::Ok);
    EXPECT(snap.source == DbSource::Cache);
    EXPECT_EQ(snap.db.dbVersion, 3L);
    EXPECT_EQ(snap.manifestSha256, fakeSha(f.files.manifest));
    EXPECT_EQ(snap.strings.lookup("fr", "controller.sony-dualsense.name"),
              std::string("DualSense"));

    TEST("a manifest that does not parse fails the snapshot");
    SnapshotFixture badManifest;
    badManifest.files.manifest = "{";
    EXPECT(buildControllerDbSnapshot(badManifest.files, DbSource::Cache, fakeSha, snap) ==
           SnapshotBuild::BadManifest);

    TEST("a file that does not match its manifest entry fails the snapshot");
    SnapshotFixture badFile;
    badFile.files.files["strings/en.json"] += " ";
    EXPECT(buildControllerDbSnapshot(badFile.files, DbSource::Cache, fakeSha, snap) ==
           SnapshotBuild::BadFile);

    TEST("a file the manifest lists but the copy lacks fails the snapshot");
    SnapshotFixture missing;
    missing.files.files.erase("strings/en.json");
    EXPECT(buildControllerDbSnapshot(missing.files, DbSource::Cache, fakeSha, snap) ==
           SnapshotBuild::BadFile);

    TEST("records whose dbVersion is not the manifest's fail the snapshot");
    SnapshotFixture skew;
    JsonOut m = JsonOut::parse(skew.files.manifest);
    m["dbVersion"] = 4;
    skew.files.manifest = jsonDump(m);
    EXPECT(buildControllerDbSnapshot(skew.files, DbSource::Cache, fakeSha, snap) ==
           SnapshotBuild::VersionMismatch);

    TEST("records of the wrong schema fail the snapshot");
    SnapshotFixture wrongSchema;
    const std::string records = dbText({fullRecord("sony-dualsense", "054c:0ce6")}, 2, 3);
    wrongSchema.files.files["controllers.json"] = records;
    JsonOut m2 = JsonOut::parse(wrongSchema.files.manifest);
    m2["files"]["controllers.json"] = {{"sha256", fakeSha(records)}, {"bytes", records.size()}};
    wrongSchema.files.manifest = jsonDump(m2);
    EXPECT(buildControllerDbSnapshot(wrongSchema.files, DbSource::Cache, fakeSha, snap) ==
           SnapshotBuild::BadRecords);
}

static void test_names() {
    TEST("the names the dashboard and the bundle read");
    EXPECT_EQ(std::string(dbSourceName(DbSource::Bundled)), std::string("bundled"));
    EXPECT_EQ(std::string(dbSourceName(DbSource::Cache)), std::string("cache"));
    EXPECT_EQ(std::string(dbParseName(DbParse::WrongSchema)), std::string("wrongSchema"));
    EXPECT_EQ(std::string(snapshotBuildName(SnapshotBuild::BadFile)), std::string("badFile"));
    EXPECT_EQ(controllerDbETag("abc"), std::string("\"abc\""));
}

static void test_mirror_file() {
    SnapshotFixture f;
    ControllerDbSnapshot snap;
    buildControllerDbSnapshot(f.files, DbSource::Bundled, fakeSha, snap);
    MirrorFile out;

    TEST("the mirror serves the manifest under its own digest");
    EXPECT(controllerDbMirrorFile(snap, "manifest.json", out));
    EXPECT(out.bytes != nullptr && *out.bytes == f.files.manifest);
    EXPECT_EQ(out.etag, controllerDbETag(fakeSha(f.files.manifest)));

    TEST("the mirror serves a listed file under the digest its manifest pins");
    EXPECT(controllerDbMirrorFile(snap, "strings/en.json", out));
    EXPECT(out.bytes != nullptr && *out.bytes == f.files.files["strings/en.json"]);
    EXPECT_EQ(out.etag, controllerDbETag(fakeSha(f.files.files["strings/en.json"])));

    TEST("the mirror serves nothing the manifest does not list");
    EXPECT(!controllerDbMirrorFile(snap, "strings/de.json", out));
    EXPECT(!controllerDbMirrorFile(snap, "../config.json", out));
    EXPECT(!controllerDbMirrorFile(snap, "schema/controller-db.schema.json", out));
}

static void test_if_none_match() {
    TEST("If-None-Match hits on the same tag, a weak copy of it, one in a list, or *");
    const std::string tag = "\"abc\"";
    EXPECT(ifNoneMatchHits("\"abc\"", tag));
    EXPECT(ifNoneMatchHits("W/\"abc\"", tag));
    EXPECT(ifNoneMatchHits("\"x\", \"abc\"", tag));
    EXPECT(ifNoneMatchHits("*", tag));

    TEST("If-None-Match misses on another tag or none");
    EXPECT(!ifNoneMatchHits("", tag));
    EXPECT(!ifNoneMatchHits("\"abd\"", tag));
    EXPECT(!ifNoneMatchHits("abc", tag));
}

int main() {
    test_mirror_file();
    test_if_none_match();
    test_bundled_copy_parses_whole();
    test_index_resolves_usb_ids();
    test_index_resolves_bluetooth_ids();
    test_index_keeps_every_record_of_a_shared_id();
    test_unknown_id_finds_nothing();
    test_document_shape_errors();
    test_unknown_fields_are_ignored();
    test_record_missing_a_required_section_is_dropped();
    test_record_with_unreadable_section_is_dropped();
    test_issue_without_id_is_skipped();
    test_verification_names_round_trip();
    test_strings_parse_and_fallback();
    test_bundled_strings_fall_back_to_en();
    test_manifest_paths();
    test_manifest_parse();
    test_file_check();
    test_snapshot_builds();
    test_names();
    std::cout << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
