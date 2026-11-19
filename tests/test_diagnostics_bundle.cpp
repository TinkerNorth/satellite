// SPDX-License-Identifier: LGPL-3.0-or-later

// core/diagnostics_bundle: what the diagnostics file may never hold, removed
// from fixtures that hold each of them, and the envelope it is written in.
#include "../src/core/config_json.h"
#include "../src/core/diagnostics_bundle.h"

#include <string>
#include <vector>

#include "test_util.h"

using namespace satellite;

namespace {

// Every value here is one the file must never carry.
const char* const SECRETS[] = {
    "a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90",
    "dish-7f3a9c",
    "Emir's Pixel 8",
    "192.168.1.47",
    "Kitchen Wi-Fi",
    "482913",
    "c0ffee00c0ffee00c0ffee00c0ffee00",
};

Config configWithEverySecret() {
    Config cfg;
    cfg.networkInterface = SECRETS[4];
    PairedDevice d;
    d.id = SECRETS[1];
    d.name = SECRETS[2];
    d.lastIP = SECRETS[3];
    d.sharedKeyHex = SECRETS[0];
    d.pairedAt = "2026-11-01";
    cfg.pairedDevices.push_back(d);
    PairedDevice e = d;
    e.id = "dish-second";
    cfg.pairedDevices.push_back(e);
    return cfg;
}

// The serialized config plus every forbidden key the table names, nested and
// at the top, so a key a future config grows is covered by the same walk.
Json configFixture() {
    Json j;
    jsonParse(serializeConfig(configWithEverySecret()), j);
    j["pin"] = SECRETS[5];
    j["nested"] = {{"sharedKey", SECRETS[0]},
                   {"list", {{{"senderIP", SECRETS[3]}, {"kept", 1}}}},
                   {"hmacProof", SECRETS[6]}};
    for (const char* key : DIAGNOSTICS_FORBIDDEN_KEYS) j["everyKey"][key] = SECRETS[6];
    return j;
}

bool holdsAnySecret(const std::string& text) {
    for (const char* secret : SECRETS) {
        if (text.find(secret) != std::string::npos) return true;
    }
    return false;
}

bool holdsAnyForbiddenKey(const Json& j) {
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            for (const char* key : DIAGNOSTICS_FORBIDDEN_KEYS) {
                if (it.key() == key) return true;
            }
            if (holdsAnyForbiddenKey(it.value())) return true;
        }
    }
    if (j.is_array()) {
        for (const Json& v : j) {
            if (holdsAnyForbiddenKey(v)) return true;
        }
    }
    return false;
}

DiagnosticsInput inputFixture() {
    DiagnosticsInput in;
    in.nowEpoch = 1763586251;
    in.version = "2.3.0";
    in.build = "2.3.0.0";
    in.flavor = "linux-deb";
    in.deviceModel = "Linux";
    in.os = "Linux";
    in.arch = "x86_64";
    in.controllerDb.bundledVersion = 1;
    in.controllerDb.activeVersion = 3;
    in.controllerDb.source = DbSource::Cache;
    in.controllerDb.checked = true;
    in.controllerDb.lastCheckEpoch = 1763582400;
    in.controllerDb.lastResult = DbCheckResult::UpToDate;
    in.hosts = {{"Host 1", 4, "active"}};
    DiagnosticsController c;
    c.host = "Host 1";
    c.ctrlIdx = 0;
    c.type = "dualsense";
    c.backend = "hidmaestro";
    c.pluggedIn = true;
    c.caps = CAP_RUMBLE | CAP_LIGHTBAR;
    c.lastGameOutput = GameOutputKind::Rumble;
    c.lastGameOutputAtMs = 1763586000000;
    in.controllers = {c};
    jsonParse(R"({"packets":12,"senderIP":"192.168.1.47","tx":{"rumble":3}})", in.debug);
    jsonParse(R"({"id":"hidmaestro","backends":[{"id":"vigem","available":false}]})", in.backends);
    in.config = configFixture();
    in.aliases = aliasHosts({{SECRETS[1], SECRETS[2], SECRETS[3]}}, {});
    in.log = {
        {1, "INFO", "pairing",
         std::string("Paired device via server PIN: ") + SECRETS[1] + " (" + SECRETS[3] + ")"},
        {2, "INFO", "client", std::string("Pairing request from ") + SECRETS[2]}};
    return in;
}

} // namespace

static void test_redact_config() {
    TEST("redactConfigForDiagnostics: no secret, key, paired identity or address survives");
    const Json out = redactConfigForDiagnostics(configFixture());
    EXPECT(!holdsAnySecret(out.dump()));
    EXPECT(!holdsAnyForbiddenKey(out));

    TEST("redactConfigForDiagnostics: the paired list becomes a count, the interface a flag");
    EXPECT_EQ(jsonInt(out, "pairedDeviceCount", -1), 2L);
    EXPECT(jsonBool(out, "networkInterfaceChosen", false));
    Config automatic;
    Json plain;
    jsonParse(serializeConfig(automatic), plain);
    const Json plainOut = redactConfigForDiagnostics(plain);
    EXPECT_EQ(jsonInt(plainOut, "pairedDeviceCount", -1), 0L);
    EXPECT(!jsonBool(plainOut, "networkInterfaceChosen", true));

    TEST("redactConfigForDiagnostics: every other setting is kept as it was");
    EXPECT_EQ(jsonInt(out, "udpPort"), static_cast<long>(DEFAULT_UDP_PORT));
    EXPECT(jsonBool(out, "controllerDbUpdates", false));
    EXPECT_EQ(jsonInt(jsonObject(out, "nested")["list"][0], "kept"), 1L);
}

static void test_redact_any_document() {
    TEST("redactForDiagnostics: the debug counters lose the sender address and keep the rest");
    Json debug;
    jsonParse(R"({"packets":12,"senderIP":"192.168.1.47","tx":{"rumble":3}})", debug);
    const Json out = redactForDiagnostics(debug);
    EXPECT(out.find("senderIP") == out.end());
    EXPECT_EQ(jsonInt(out, "packets"), 12L);
    EXPECT_EQ(jsonInt(jsonObject(out, "tx"), "rumble"), 3L);
}

static void test_alias_hosts() {
    TEST("aliasHosts: connected clients first, then paired ones, numbered from 1");
    const std::vector<HostAlias> aliases =
        aliasHosts({{"dish-a", "Pixel", "10.0.0.2"}}, {{"dish-b", "Tablet", ""}});
    EXPECT_EQ(aliases.size(), size_t{5});
    EXPECT_EQ(hostAlias(0), std::string("Host 1"));
    EXPECT_EQ(hostAlias(1), std::string("Host 2"));
    bool sawB = false;
    for (const HostAlias& a : aliases) {
        if (a.text == "dish-a" || a.text == "Pixel" || a.text == "10.0.0.2") {
            EXPECT_EQ(a.alias, std::string("Host 1"));
        }
        if (a.text == "Tablet") {
            EXPECT_EQ(a.alias, std::string("Host 2"));
            sawB = true;
        }
        EXPECT(!a.text.empty());
    }
    EXPECT(sawB);
}

static void test_scrub_text() {
    TEST("scrubDiagnosticsText: ids and names become their alias, the longest first");
    const std::vector<HostAlias> aliases = {{"Pixel", "Host 2"}, {"Pixel 8", "Host 1"}};
    EXPECT_EQ(scrubDiagnosticsText("from Pixel 8 and Pixel", aliases),
              std::string("from Host 1 and Host 2"));

    TEST("scrubDiagnosticsText: every dotted-quad address is masked, versions are not");
    EXPECT_EQ(scrubDiagnosticsText("peer 10.0.0.25:9876 then 192.168.1.4.", {}),
              std::string("peer x.x.x.x:9876 then x.x.x.x."));
    EXPECT_EQ(scrubDiagnosticsText("Satellite 2.3.0 protocol v4", {}),
              std::string("Satellite 2.3.0 protocol v4"));
    EXPECT_EQ(scrubDiagnosticsText("999.1.1.1 is no address", {}),
              std::string("999.1.1.1 is no address"));
}

static void test_iso_utc() {
    TEST("isoUtc: Unix seconds in UTC, second precision");
    EXPECT_EQ(isoUtc(0), std::string("1970-01-01T00:00:00Z"));
    EXPECT_EQ(isoUtc(1763586251), std::string("2025-11-19T21:04:11Z"));
    EXPECT_EQ(isoUtc(951782400), std::string("2000-02-29T00:00:00Z"));
}

static void test_bundle_envelope() {
    TEST("bundle: the dish-diagnostics.v1 envelope with app.name satellite");
    const JsonOut b = buildDiagnosticsBundle(inputFixture());
    EXPECT_EQ(b.value("bundle", 0), DIAGNOSTICS_BUNDLE_VERSION);
    EXPECT_EQ(b.value("createdAt", std::string()), std::string("2025-11-19T21:04:11Z"));
    EXPECT_EQ(b["app"].value("name", std::string()), std::string("satellite"));
    EXPECT_EQ(b["app"].value("version", std::string()), std::string("2.3.0"));
    EXPECT_EQ(b["app"].value("flavor", std::string()), std::string("linux-deb"));
    EXPECT_EQ(b["device"].value("arch", std::string()), std::string("x86_64"));

    TEST("bundle: controllerDb carries the versions, source and last check");
    EXPECT_EQ(b["controllerDb"].value("bundledVersion", 0), 1);
    EXPECT_EQ(b["controllerDb"].value("activeVersion", 0), 3);
    EXPECT_EQ(b["controllerDb"].value("source", std::string()), std::string("cache"));
    EXPECT_EQ(b["controllerDb"].value("lastCheckAt", std::string()),
              std::string("2025-11-19T20:00:00Z"));
    EXPECT_EQ(b["controllerDb"].value("lastCheckResult", std::string()), std::string("upToDate"));

    TEST("bundle: a database never checked has null for the last check");
    DiagnosticsInput fresh = inputFixture();
    fresh.controllerDb.checked = false;
    const JsonOut f = buildDiagnosticsBundle(fresh);
    EXPECT(f["controllerDb"]["lastCheckAt"].is_null());
    EXPECT(f["controllerDb"]["lastCheckResult"].is_null());

    TEST("bundle: each virtual controller with its host, type, backend, caps and game output");
    EXPECT_EQ(b["controllers"].size(), size_t{1});
    const JsonOut& c = b["controllers"][0];
    EXPECT_EQ(c.value("host", std::string()), std::string("Host 1"));
    EXPECT_EQ(c.value("type", std::string()), std::string("dualsense"));
    EXPECT_EQ(c.value("backend", std::string()), std::string("hidmaestro"));
    EXPECT(c["caps"].value("rumble", false));
    EXPECT(!c["caps"].value("mic", true));
    EXPECT_EQ(c["lastGameOutput"].value("kind", std::string()), std::string("rumble"));
    EXPECT_EQ(c["lastGameOutput"].value("atMs", int64_t{0}), int64_t{1763586000000});

    TEST("bundle: a controller the game never wrote has a null lastGameOutput");
    DiagnosticsInput quiet = inputFixture();
    quiet.controllers[0].lastGameOutput = GameOutputKind::None;
    EXPECT(buildDiagnosticsBundle(quiet)["controllers"][0]["lastGameOutput"].is_null());

    TEST("bundle: hosts are aliases with protocol and link");
    EXPECT_EQ(b["hosts"][0].value("alias", std::string()), std::string("Host 1"));
    EXPECT_EQ(b["hosts"][0].value("protocol", 0), 4);
    EXPECT_EQ(b["hosts"][0].value("link", std::string()), std::string("active"));
    EXPECT_EQ(b["hosts"][0].value("kind", std::string()), std::string("dish"));

    TEST("bundle: debug, backends and config ride along, redacted");
    EXPECT_EQ(b["debug"].value("packets", 0), 12);
    EXPECT(b["debug"].find("senderIP") == b["debug"].end());
    EXPECT_EQ(b["backends"].value("id", std::string()), std::string("hidmaestro"));
    EXPECT_EQ(b["config"].value("pairedDeviceCount", -1), 2);

    TEST("bundle: no secret, address or paired identity appears anywhere in the file");
    EXPECT(!holdsAnySecret(jsonDump(b)));

    TEST("bundle: the log is scrubbed: a client's id, name and address become its alias, any other "
         "address is masked");
    EXPECT_EQ(b["log"].size(), size_t{2});
    EXPECT_EQ(b["log"][0].value("message", std::string()),
              std::string("Paired device via server PIN: Host 1 (Host 1)"));
    DiagnosticsInput stranger = inputFixture();
    stranger.log = {{3, "WARN", "pairing", "Invalid or empty PIN attempt from 10.1.2.3"}};
    EXPECT_EQ(buildDiagnosticsBundle(stranger)["log"][0].value("message", std::string()),
              std::string("Invalid or empty PIN attempt from x.x.x.x"));
    EXPECT_EQ(b["log"][1].value("tag", std::string()), std::string("client"));
    EXPECT_EQ(b["log"][1].value("atMs", int64_t{0}), int64_t{2});
}

static void test_bundle_log_tail() {
    TEST("bundle: only the last 500 log lines are written, oldest first");
    DiagnosticsInput in = inputFixture();
    in.log.clear();
    for (int i = 0; i < 600; i++) in.log.push_back({i, "INFO", "t", "line " + std::to_string(i)});
    const JsonOut b = buildDiagnosticsBundle(in);
    EXPECT_EQ(b["log"].size(), DIAGNOSTICS_LOG_LINES);
    EXPECT_EQ(b["log"][0].value("message", std::string()), std::string("line 100"));
    EXPECT_EQ(b["log"][499].value("message", std::string()), std::string("line 599"));
}

int main() {
    test_redact_config();
    test_redact_any_document();
    test_alias_hosts();
    test_scrub_text();
    test_iso_utc();
    test_bundle_envelope();
    test_bundle_log_tail();
    std::cout << "diagnostics_bundle: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
