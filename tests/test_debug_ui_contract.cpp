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

using satellite::buildDebugJson;
using satellite::Json;
using satellite::StatusFields;

// The Debug page reads /api/debug through one table of rows, DEBUG_GROUPS in
// web/debug.js: a top-level field as `d.<field>`, a counter as
// `sub(d, '<group>').<field>`. A name the table reads that buildDebugJson does
// not write renders as a dash, and a dash where the counter that tells two
// renderings apart belongs (haptics sent as audio against haptics reduced to
// rumble) is a page that cannot answer the question it exists for.

struct GroupedField {
    std::string group;
    std::string field;
    bool operator<(const GroupedField& other) const {
        return group != other.group ? group < other.group : field < other.field;
    }
};

static std::string webPath(const std::string& relative) {
    return std::string(WEB_DIR) + "/" + relative;
}

static std::string readFileAll(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return "";
    return {(std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()};
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
    return {values.begin(), values.end()};
}

// The row table alone: the page's other readers (the backend list, the
// pipeline card) take other payloads under the same parameter name.
static std::string debugGroupsBody() {
    const std::string source = readFileAll(webPath("debug.js"));
    const std::regex pattern("const DEBUG_GROUPS = \\[([\\s\\S]*?)\\n\\];");
    std::smatch body;
    return std::regex_search(source, body, pattern) ? body[1].str() : std::string();
}

static std::set<std::string> directFieldsRead(const std::string& body) {
    return distinct(firstGroups(body, std::regex("\\bd\\.([A-Za-z_][A-Za-z0-9_]*)")));
}

static std::set<GroupedField> groupedFieldsRead(const std::string& body) {
    std::set<GroupedField> found;
    const std::regex pattern(
        "\\bsub\\(d, '([A-Za-z_][A-Za-z0-9_]*)'\\)\\.([A-Za-z_][A-Za-z0-9_]*)");
    const std::sregex_iterator end;
    for (std::sregex_iterator it(body.begin(), body.end(), pattern); it != end; ++it) {
        found.insert(GroupedField{(*it)[1].str(), (*it)[2].str()});
    }
    return found;
}

static Json serverDebugJson() { return Json::parse(buildDebugJson(StatusFields{})); }

static void reportMissing(const std::string& what, const std::string& where) {
    std::cerr << "    missing " << what << " in " << where << "\n";
}

static void test_scan_sees_the_reads() {
    TEST("debug UI contract: the scan finds the row table, its direct reads and its grouped reads");
    const std::string body = debugGroupsBody();
    EXPECT(!body.empty());
    EXPECT(!directFieldsRead(body).empty());
    EXPECT(!groupedFieldsRead(body).empty());
}

static void test_direct_fields_are_written() {
    TEST("debug UI contract: every top-level field the row table reads is one buildDebugJson "
         "writes");
    const Json server = serverDebugJson();
    for (const std::string& field : directFieldsRead(debugGroupsBody())) {
        const bool written = server.contains(field);
        if (!written) reportMissing("d." + field, "buildDebugJson");
        EXPECT(written);
    }
}

static void test_grouped_fields_are_written() {
    TEST("debug UI contract: every counter the row table reads is in the group buildDebugJson "
         "writes it under");
    const Json server = serverDebugJson();
    for (const GroupedField& read : groupedFieldsRead(debugGroupsBody())) {
        const bool groupWritten = server.contains(read.group) && server[read.group].is_object();
        const bool written = groupWritten && server[read.group].contains(read.field);
        if (!written) reportMissing("sub(d, '" + read.group + "')." + read.field, "buildDebugJson");
        EXPECT(written);
    }
}

static void test_the_haptic_renderings_are_told_apart() {
    TEST("debug UI contract: the row table shows haptics sent as audio and haptics reduced to "
         "rumble as separate counters");
    const std::set<GroupedField> reads = groupedFieldsRead(debugGroupsBody());
    EXPECT(reads.count(GroupedField{"audio", "hapticSent"}) != 0);
    EXPECT(reads.count(GroupedField{"audio", "hapticReducedToRumble"}) != 0);
    EXPECT(reads.count(GroupedField{"tx", "hapticAudio"}) != 0);
    EXPECT(reads.count(GroupedField{"tx", "rumble"}) != 0);
}

int main() {
    return satellite::test::runMain("test_debug_ui_contract", [] {
        std::cout << "Running debug UI contract tests...\n\n";
        test_scan_sees_the_reads();
        test_direct_fields_are_written();
        test_grouped_fields_are_written();
        test_the_haptic_renderings_are_told_apart();
    });
}
