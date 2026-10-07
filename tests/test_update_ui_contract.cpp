// SPDX-License-Identifier: LGPL-3.0-or-later
#include "../src/net/status_json.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include "test_util.h"

using satellite::buildUpdateJson;
using satellite::Json;

struct SnapshotReader {
    const char* file;
    const char* name;
};

static constexpr SnapshotReader SNAPSHOT_READERS[] = {
    {"updates.js", "s"},
    {"dashboard.js", "upd"},
};

static constexpr const char* STRING_KEY_READER = "updates.js";
static constexpr const char* SOURCE_LOCALE = "en";

static std::string webPath(const std::string& relative) {
    return std::string(WEB_DIR) + "/" + relative;
}

static std::string readFileAll(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return "";
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static std::string withoutBom(const std::string& text) {
    const bool hasBom = text.rfind("\xEF\xBB\xBF", 0) == 0;
    return hasBom ? text.substr(3) : text;
}

static std::vector<std::string> firstGroups(const std::string& text, const std::regex& pattern) {
    std::vector<std::string> found;
    const std::sregex_iterator end;
    for (std::sregex_iterator it(text.begin(), text.end(), pattern); it != end; ++it) {
        found.push_back((*it)[1].str());
    }
    return found;
}

static std::set<std::string> distinct(const std::vector<std::string>& values) {
    return std::set<std::string>(values.begin(), values.end());
}

static std::set<std::string> snapshotFieldsRead(const SnapshotReader& reader) {
    const std::string source = readFileAll(webPath(reader.file));
    const std::regex pattern("\\b" + std::string(reader.name) + "\\.([A-Za-z_][A-Za-z0-9_]*)");
    return distinct(firstGroups(source, pattern));
}

static std::set<std::string> releaseFieldsRead(const SnapshotReader& reader) {
    const std::string source = readFileAll(webPath(reader.file));
    const std::regex pattern("\\b" + std::string(reader.name) +
                             "\\.info\\.([A-Za-z_][A-Za-z0-9_]*)");
    return distinct(firstGroups(source, pattern));
}

static std::set<std::string> stringKeysAskedFor() {
    const std::string source = readFileAll(webPath(STRING_KEY_READER));
    const std::regex pattern("\\bt\\('([^']+)'");
    return distinct(firstGroups(source, pattern));
}

static std::vector<std::string> shippedLocales() {
    const std::string i18n = readFileAll(webPath("i18n.js"));
    const std::regex listPattern("SHIPPED_LOCALES\\s*=\\s*\\[([^\\]]*)\\]");
    std::smatch list;
    if (!std::regex_search(i18n, list, listPattern)) return {};
    const std::regex entryPattern("'([^']+)'");
    return firstGroups(list[1].str(), entryPattern);
}

static Json catalog(const std::string& locale) {
    const std::string text = withoutBom(readFileAll(webPath("lang/" + locale + ".json")));
    return Json::parse(text, nullptr, false);
}

static std::set<std::string> placeholders(const std::string& text) {
    const std::regex pattern("(%[0-9]+\\$[sd])");
    return distinct(firstGroups(text, pattern));
}

static UpdateStatusSnapshot representativeSnapshot() {
    UpdateStatusSnapshot s;
    s.state = UpdateState::Downloading;
    s.currentVersion = "2.1.2";
    s.platformId = "windows";
    s.channel = "stable";
    s.bytesDownloaded = 4096;
    s.totalBytes = 8192;
    s.message = "message";
    s.failedPhase = UpdateState::Idle;
    s.dismissed = true;
    s.lastCheckEpoch = 1700000000;
    s.info.available = true;
    s.info.version = "2.2.0";
    s.info.channel = "stable";
    s.info.assetName = "SatelliteSetup-2.2.0.exe";
    s.info.assetSize = 8192;
    s.info.assetSha256 = "ab";
    s.info.htmlUrl = "https://github.com/TinkerNorth/satellite/releases/tag/2.2.0";
    s.info.publishedAtEpoch = 1700000000;
    s.info.installMethod = InstallMethod::Manual;
    s.info.manualInstruction = "sudo apt upgrade satellite";
    s.info.releaseNotes = "notes";
    return s;
}

static Json serverUpdateJson() { return Json::parse(buildUpdateJson(representativeSnapshot())); }

static void reportMissing(const std::string& what, const std::string& where) {
    std::cerr << "    missing " << what << " in " << where << "\n";
}

static void test_scan_sees_the_reads() {
    TEST("update UI contract: the scan finds snapshot and release reads in every reader");
    for (const SnapshotReader& reader : SNAPSHOT_READERS) {
        EXPECT(!snapshotFieldsRead(reader).empty());
        EXPECT(!releaseFieldsRead(reader).empty());
    }
    EXPECT(!stringKeysAskedFor().empty());
    EXPECT(shippedLocales().size() > 1);
}

static void test_snapshot_fields_are_written() {
    TEST("update UI contract: every snapshot field the web UI reads is one the server writes");
    const Json server = serverUpdateJson();
    for (const SnapshotReader& reader : SNAPSHOT_READERS) {
        for (const std::string& field : snapshotFieldsRead(reader)) {
            const bool written = server.contains(field);
            if (!written) reportMissing(std::string(reader.name) + "." + field, "buildUpdateJson");
            EXPECT(written);
        }
    }
}

static void test_release_fields_are_written() {
    TEST("update UI contract: every release field the web UI reads is one the server writes");
    const Json server = serverUpdateJson();
    const bool hasInfo = server.contains("info") && server["info"].is_object();
    EXPECT(hasInfo);
    if (!hasInfo) return;
    for (const SnapshotReader& reader : SNAPSHOT_READERS) {
        for (const std::string& field : releaseFieldsRead(reader)) {
            const bool written = server["info"].contains(field);
            if (!written) {
                reportMissing(std::string(reader.name) + ".info." + field, "buildUpdateJson");
            }
            EXPECT(written);
        }
    }
}

static void test_string_keys_are_in_every_catalog() {
    TEST("update UI contract: every string key updates.js asks for is in every shipped catalog");
    const std::set<std::string> keys = stringKeysAskedFor();
    for (const std::string& locale : shippedLocales()) {
        const Json strings = catalog(locale);
        const bool parsed = strings.is_object();
        EXPECT(parsed);
        if (!parsed) continue;
        for (const std::string& key : keys) {
            const bool present = strings.contains(key) && strings[key].is_string();
            if (!present) reportMissing(key, locale + ".json");
            EXPECT(present);
        }
    }
}

static void test_translations_keep_the_placeholders() {
    TEST("update UI contract: each catalog's string carries the English placeholders");
    const Json source = catalog(SOURCE_LOCALE);
    EXPECT(source.is_object());
    if (!source.is_object()) return;
    for (const std::string& locale : shippedLocales()) {
        const Json strings = catalog(locale);
        if (!strings.is_object()) continue;
        for (const std::string& key : stringKeysAskedFor()) {
            const bool comparable = source.contains(key) && source[key].is_string() &&
                                    strings.contains(key) && strings[key].is_string();
            if (!comparable) continue;
            const bool same = placeholders(source[key].get<std::string>()) ==
                              placeholders(strings[key].get<std::string>());
            if (!same) reportMissing("the English placeholders of " + key, locale + ".json");
            EXPECT(same);
        }
    }
}

int main() {
    std::cout << "Running update UI contract tests...\n\n";
    test_scan_sees_the_reads();
    test_snapshot_fields_are_written();
    test_release_fields_are_written();
    test_string_keys_are_in_every_catalog();
    test_translations_keep_the_placeholders();

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
