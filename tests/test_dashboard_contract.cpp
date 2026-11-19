// SPDX-License-Identifier: LGPL-3.0-or-later

// What the dashboard reads of this change, held to where the server writes
// it: the Test button's lanes and caps against core/feedback_test.h and the
// wire cap names, and every string the new surfaces look up against all six
// locales, including the names the server sends (lastGameOutput kinds, check
// results, database sources).
#include "../src/core/controller_db_service.h"
#include "../src/core/descriptor_json.h"
#include "../src/core/feedback_test.h"
#include "../src/core/json.h"
#include "../src/core/types.h"

#include <fstream>
#include <iterator>
#include <regex>
#include <string>
#include <vector>

#include "test_util.h"

using namespace satellite;

namespace {

const char* const LOCALES[] = {"en", "bs", "de", "es", "fr", "pt-BR"};

std::string readFileAll(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return "";
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::string web(const std::string& relative) {
    return readFileAll(std::string(WEB_DIR) + "/" + relative);
}

Json locale(const std::string& name) {
    Json j;
    jsonParse(web("lang/" + name + ".json"), j);
    return j;
}

std::string capName(uint16_t bit) {
    for (const CapName& c : CAP_NAMES) {
        if (c.bit == bit) return c.name;
    }
    return "";
}

struct LanePair {
    std::string lane;
    std::string cap;
};

std::vector<LanePair> dashboardLanes() {
    std::vector<LanePair> out;
    const std::string js = web("dashboard.js");
    const std::regex table("const TEST_LANES = \\[([\\s\\S]*?)\\];");
    std::smatch m;
    if (!std::regex_search(js, m, table)) return out;
    const std::string body = m[1].str();
    const std::regex row("\\{ lane: '([A-Za-z]+)', cap: '([A-Za-z]+)' \\}");
    for (std::sregex_iterator it(body.begin(), body.end(), row), end; it != end; ++it) {
        out.push_back({(*it)[1].str(), (*it)[2].str()});
    }
    return out;
}

// Every t('key') a script names whole; a key built from a server value
// (t('prefix.' + name)) is held to the server's names above instead.
std::vector<std::string> literalKeys(const std::string& js) {
    std::vector<std::string> keys;
    const std::regex call("\\bt\\('([a-z0-9.-]+)'\\s*[,)]");
    for (std::sregex_iterator it(js.begin(), js.end(), call), end; it != end; ++it) {
        keys.push_back((*it)[1].str());
    }
    return keys;
}

void expectInEveryLocale(const std::string& key) {
    for (const char* name : LOCALES) {
        const Json j = locale(name);
        const bool present =
            j.contains(key) && j[key].is_string() && !j[key].get<std::string>().empty();
        if (!present) std::cerr << "  missing " << key << " in " << name << "\n";
        EXPECT(present);
    }
}

} // namespace

static void test_test_lanes_match_the_server() {
    TEST("the dashboard's TEST_LANES are FEEDBACK_LANES, in order, each with its wire cap name");
    const std::vector<LanePair> lanes = dashboardLanes();
    const size_t expected = sizeof(FEEDBACK_LANES) / sizeof(FEEDBACK_LANES[0]);
    EXPECT_EQ(lanes.size(), expected);
    for (size_t i = 0; i < lanes.size() && i < expected; i++) {
        EXPECT_EQ(lanes[i].lane, std::string(FEEDBACK_LANES[i].name));
        EXPECT_EQ(lanes[i].cap, capName(FEEDBACK_LANES[i].cap));
    }
}

static void test_server_names_have_strings() {
    TEST("every lane and every lastGameOutput kind has its label in all six locales");
    for (const FeedbackLaneInfo& lane : FEEDBACK_LANES) {
        expectInEveryLocale(std::string("controllers.lane.") + lane.name);
    }
    for (const GameOutputName& kind : GAME_OUTPUT_NAMES) {
        if (kind.kind != GameOutputKind::None) {
            expectInEveryLocale(std::string("controllers.lane.") + kind.name);
        }
    }

    TEST("every check result and database source has its label in all six locales");
    const DbCheckResult results[] = {DbCheckResult::UpToDate, DbCheckResult::Updated,
                                     DbCheckResult::Offline, DbCheckResult::Invalid,
                                     DbCheckResult::Off};
    for (const DbCheckResult r : results) {
        expectInEveryLocale(std::string("settings.controller-db.result.") + dbCheckResultName(r));
    }
    expectInEveryLocale(std::string("settings.controller-db.source.") +
                        dbSourceName(DbSource::Bundled));
    expectInEveryLocale(std::string("settings.controller-db.source.") +
                        dbSourceName(DbSource::Cache));
}

static void test_new_surfaces_are_translated() {
    TEST("every string the diagnostics, controller database and Test surfaces name is in all six "
         "locales");
    std::vector<std::string> keys = literalKeys(web("diagnostics.js"));
    const std::string settings = web("settings.js");
    const size_t dbBlock = settings.find("// The controller database block");
    EXPECT(dbBlock != std::string::npos);
    if (dbBlock != std::string::npos) {
        const std::vector<std::string> more = literalKeys(settings.substr(dbBlock));
        keys.insert(keys.end(), more.begin(), more.end());
    }
    const std::string dashboard = web("dashboard.js");
    for (const std::string& key : literalKeys(dashboard)) {
        if (key.rfind("controllers.test", 0) == 0 ||
            key.rfind("controllers.last-from-game", 0) == 0 ||
            key.rfind("controllers.ago.", 0) == 0) {
            keys.push_back(key);
        }
    }
    EXPECT(keys.size() > 20);
    for (const std::string& key : keys) expectInEveryLocale(key);
    const std::string html = web("index.html");
    const std::regex attr(
        "data-i18n=\"((?:settings\\.controller-db|debug\\.diagnostics)[a-z.-]*)\"");
    for (std::sregex_iterator it(html.begin(), html.end(), attr), end; it != end; ++it) {
        expectInEveryLocale((*it)[1].str());
    }
}

static void test_check_now_follows_the_switch() {
    TEST("Settings disables Check now while the automatic check is off, and while one runs");
    const std::string js = web("settings.js");
    EXPECT(js.find("check.disabled = s.enabled !== true || s.checking === true;") !=
           std::string::npos);
    const std::string html = web("index.html");
    EXPECT(html.find("id=\"settings-btnCdbCheck\"") != std::string::npos);
    EXPECT(html.find("id=\"settings-controllerDbUpdates\"") != std::string::npos);
}

int main() {
    test_check_now_follows_the_switch();
    test_test_lanes_match_the_server();
    test_server_names_have_strings();
    test_new_surfaces_are_translated();
    std::cout << "dashboard_contract: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
