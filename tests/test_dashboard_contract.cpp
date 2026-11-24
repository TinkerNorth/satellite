// SPDX-License-Identifier: LGPL-3.0-or-later

// What the dashboard reads of this change, held to where the server writes
// it: the Test button's lanes and caps against core/feedback_test.h and the
// wire cap names, and every string the new surfaces look up against all six
// locales, including the names the server sends (lastGameOutput kinds, check
// results, database sources).
#include "../src/core/controller_db_service.h"
#include "../src/core/descriptor_json.h"
#include "../src/core/diagnostics_bundle.h"
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
    return {(std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()};
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

std::vector<std::string> quotedList(const std::string& body) {
    std::vector<std::string> out;
    const std::regex item("'([A-Za-z]+)'");
    for (std::sregex_iterator it(body.begin(), body.end(), item), end; it != end; ++it) {
        out.push_back((*it)[1].str());
    }
    return out;
}

std::vector<std::string> dashboardReportCategories() {
    const std::string js = web("diagnostics.js");
    const std::regex table("const REPORT_CATEGORIES = \\[([^\\]]*)\\];");
    std::smatch m;
    if (!std::regex_search(js, m, table)) return {};
    return quotedList(m[1].str());
}

std::vector<std::string> formReportCategories() {
    std::vector<std::string> out;
    const std::string html = web("index.html");
    const size_t select = html.find("<select id=\"debug-report-category\"");
    const size_t close = html.find("</select>", select);
    if (select == std::string::npos || close == std::string::npos) return out;
    const std::string body = html.substr(select, close - select);
    const std::regex option("<option value=\"([A-Za-z]+)\"");
    for (std::sregex_iterator it(body.begin(), body.end(), option), end; it != end; ++it) {
        out.push_back((*it)[1].str());
    }
    return out;
}

std::vector<std::string> coreReportCategories() {
    std::vector<std::string> out;
    for (const char* name : REPORT_CATEGORIES) out.push_back(name);
    return out;
}

} // namespace

static void test_report_categories_match_the_contract() {
    TEST("the report's categories are the contract's six, in order, in the script and the form");
    const std::vector<std::string> core = coreReportCategories();
    EXPECT_EQ(core.size(), size_t{6});
    EXPECT(dashboardReportCategories() == core);
    EXPECT(formReportCategories() == core);

    TEST("every report category has its label in all six locales");
    for (const std::string& name : core) expectInEveryLocale("report.category." + name);
    expectInEveryLocale("debug.report.text-placeholder");

    TEST("the support subject keeps its English prefix in every locale, so support can filter on "
         "it");
    for (const char* name : LOCALES) {
        const Json j = locale(name);
        const std::string subject = jsonStr(j, "mail.support-subject");
        EXPECT(subject.rfind("Dish support: %1$s · Satellite %2$s ", 0) == 0);
        EXPECT(subject.find("%3$s") != std::string::npos);
    }

    TEST("the file carries the report as category, text and a null controller, and the mail "
         "starts with the person's words");
    const std::string js = web("diagnostics.js");
    EXPECT(js.find("return { category, text: text ? text.value.trim() : '', controller: null };") !=
           std::string::npos);
    EXPECT(js.find("const words = report.text ? [report.text, ''] : [];") != std::string::npos);
    EXPECT(js.find("t('mail.support-subject',") != std::string::npos);
}

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
        "data-i18n=\"((?:settings\\.controller-db|debug\\.report|report\\.category)[A-Za-z.-]*)\"");
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
    return satellite::test::runMain("dashboard_contract", [] {
        test_check_now_follows_the_switch();
        test_test_lanes_match_the_server();
        test_server_names_have_strings();
        test_new_surfaces_are_translated();
        test_report_categories_match_the_contract();
    });
}
