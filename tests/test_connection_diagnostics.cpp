// SPDX-License-Identifier: LGPL-3.0-or-later

// core/connection_diagnostics: the document a Dish asks for about its own
// connection, built from a fixture that holds every value it must never carry
// (other clients' names, ids and addresses, the machine name, keys, secrets),
// and the choice of the log lines that are about the connection.
#include "../src/core/connection_diagnostics.h"

#include <string>
#include <vector>

#include "test_util.h"

using namespace satellite;

namespace {

const char* const SELF_ID = "dish-self-41";
const char* const SELF_NAME = "Thor of Emir";
const char* const SELF_IP = "192.168.1.47";
const char* const CONNECTION_ID = "conn_ab12cd34";

// Every value here is one the document must never carry.
const char* const FORBIDDEN[] = {
    "dish-other-77",                                                    // another client's id
    "Living Room Shield",                                               // another client's name
    "192.168.1.80",                                                     // another client's address
    "dish-paired-9",                                                    // a paired, absent client
    "Grandma's iPad",                                                   // its name
    "EMIR-DESKTOP",                                                     // the machine
    "a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90", // a pairing key
    "c0ffee00c0ffee00c0ffee00c0ffee00",                                 // a secret
    "Thor of Emir",                                                     // the requester's own name
    "192.168.1.47",                                                     // the requester's address
    "dish-self-41",                                                     // the requester's id
};

bool holdsForbidden(const std::string& text) {
    for (const char* value : FORBIDDEN) {
        if (text.find(value) != std::string::npos) {
            std::cerr << "  leaked: " << value << "\n";
            return true;
        }
    }
    return false;
}

bool holdsAnyForbiddenKey(const JsonOut& j) {
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            for (const char* key : DIAGNOSTICS_FORBIDDEN_KEYS) {
                if (it.key() == key) return true;
            }
            if (holdsAnyForbiddenKey(it.value())) return true;
        }
    }
    if (j.is_array()) {
        for (const JsonOut& v : j) {
            if (holdsAnyForbiddenKey(v)) return true;
        }
    }
    return false;
}

ConnectionController padFixture() {
    ConnectionController c;
    c.ctrlIdx = 0;
    c.requestedType = CONTROLLER_TYPE_DUALSENSE;
    c.appliedType = CONTROLLER_TYPE_DUALSENSE;
    c.backend = "hidmaestro";
    c.composite = true;
    c.pluggedIn = true;
    c.caps = CAP_RUMBLE | CAP_LIGHTBAR;
    c.touchpadMode = TOUCHPAD_MODE_DS4;
    c.motionSinkSupportedForType = true;
    c.motionBackendOk = true;
    c.serial = 3;
    c.pluggedAtMs = 5000;
    c.input.frames = 12345;
    c.input.rateHz = 250;
    c.input.lastAtMs = 9000;
    c.output.rumble = {12, 8000};
    c.output.rumbleStrong = 32768;
    c.lastGameOutput = GameOutputKind::Rumble;
    c.lastGameOutputAtMs = 8000;
    return c;
}

ConnectionDiagnosticsInput inputFixture() {
    ConnectionDiagnosticsInput in;
    in.version = "2.3.0";
    in.platform = "windows";
    in.protocolMin = 1;
    in.protocolMax = 4;
    in.uptimeS = 3600;
    ConnectionBackend hm;
    hm.id = "hidmaestro";
    hm.available = true;
    hm.driver = "2.1.0.1716";
    hm.bundled = "2.1.0.1716";
    hm.preferred = true;
    hm.lifecycle = "supported";
    ConnectionBackend vigem;
    vigem.id = "vigem";
    vigem.available = true;
    vigem.driver = "1.22.0";
    vigem.lifecycle = "eol";
    in.backends = {hm, vigem};
    in.controllers = {padFixture()};
    jsonParse(R"({"enabled":true,"mic":false,"speaker":true,"hapticAudio":false,
                  "sharedKey":"c0ffee00c0ffee00c0ffee00c0ffee00"})",
              in.grants);
    jsonParse(R"({"rx":{"input":40,"heartbeat":3},"tx":{"rumble":12},
                  "audio":{"hapticReducedToRumble":4210},
                  "rejected":{"decryptFail":1,"replayDrop":0},
                  "senderIP":"192.168.1.80","token":"c0ffee00c0ffee00c0ffee00c0ffee00",
                  "pin":"482913"})",
              in.counters);
    in.controllerDb.bundledVersion = 1;
    in.controllerDb.activeVersion = 1;
    in.connectionId = CONNECTION_ID;
    in.self = {SELF_ID, SELF_NAME, SELF_IP};
    in.others = {{"dish-other-77", "Living Room Shield", "192.168.1.80"},
                 {"dish-paired-9", "Grandma's iPad", ""}};
    in.machineNames = {"EMIR-DESKTOP"};
    in.secrets = {"a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90",
                  "c0ffee00c0ffee00c0ffee00c0ffee00"};
    in.log = {
        {1000, "INFO", "service",
         std::string("Controller #0 plugged as DualSense (serial 3) for ") + SELF_NAME + " (" +
             CONNECTION_ID + ")"},
        {1100, "INFO", "service",
         "Controller #0 plugged as Xbox (serial 4) for Living Room Shield"},
        {1200, "INFO", "mdns", "Announcing EMIR-DESKTOP._satellite._udp.local."},
        {1300, "INFO", "pairing",
         std::string("Device ") + SELF_ID + " at " + SELF_IP +
             " re-PUT beside Living Room Shield (192.168.1.80) on "
             "EMIR-DESKTOP with key "
             "a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90"},
        {4000, "WARN", "hidmaestro", "Serial 3 fell back to the input-only persona"},
        {6000, "WARN", "hidmaestro", "Serial 3 fell back to the input-only persona"},
        {6100, "INFO", "hidmaestro", "Serial 31 runs on HIDMaestro driver 2.1.0.1716"},
        {6200, "INFO", "service", std::string("Kicked ") + CONNECTION_ID},
        {6300, "INFO", "client", "Pairing request from Grandma's iPad"},
    };
    return in;
}

} // namespace

static void test_envelope() {
    TEST("connection diagnostics: the format, version, platform, protocol range and uptime");
    const JsonOut d = buildConnectionDiagnostics(inputFixture());
    EXPECT_EQ(d.value("format", std::string()), std::string(CONNECTION_DIAGNOSTICS_FORMAT));
    EXPECT_EQ(d["satellite"].value("version", std::string()), std::string("2.3.0"));
    EXPECT_EQ(d["satellite"].value("platform", std::string()), std::string("windows"));
    EXPECT_EQ(d["satellite"]["protocol"].size(), size_t{2});
    EXPECT_EQ(d["satellite"]["protocol"][0].get<int>(), 1);
    EXPECT_EQ(d["satellite"]["protocol"][1].get<int>(), 4);
    EXPECT_EQ(d["satellite"].value("uptimeS", 0), 3600);

    TEST("connection diagnostics: the backends with availability, versions, preference, lifecycle");
    EXPECT_EQ(d["backends"].size(), size_t{2});
    EXPECT_EQ(d["backends"][0].value("id", std::string()), std::string("hidmaestro"));
    EXPECT(d["backends"][0].value("available", false));
    EXPECT_EQ(d["backends"][0].value("driver", std::string()), std::string("2.1.0.1716"));
    EXPECT_EQ(d["backends"][0].value("bundled", std::string()), std::string("2.1.0.1716"));
    EXPECT(d["backends"][0].value("preferred", false));
    EXPECT_EQ(d["backends"][0].value("lifecycle", std::string()), std::string("supported"));
    EXPECT(!d["backends"][1].value("preferred", true));
    EXPECT(d["backends"][1]["bundled"].is_null());

    TEST("connection diagnostics: the controller database status");
    EXPECT_EQ(d["controllerDb"].value("activeVersion", 0), 1);
    EXPECT_EQ(d["controllerDb"].value("source", std::string()), std::string("bundled"));
}

static void test_controllers() {
    TEST("connection diagnostics: each controller with requested and applied type, backend, "
         "composite, plug, caps, touchpad and motion");
    const JsonOut d = buildConnectionDiagnostics(inputFixture());
    EXPECT_EQ(d["controllers"].size(), size_t{1});
    const JsonOut& c = d["controllers"][0];
    EXPECT_EQ(c.value("ctrlIdx", -1), 0);
    EXPECT_EQ(c.value("requestedType", std::string()), std::string("dualsense"));
    EXPECT_EQ(c.value("appliedType", std::string()), std::string("dualsense"));
    EXPECT_EQ(c.value("backend", std::string()), std::string("hidmaestro"));
    EXPECT(c.value("composite", false));
    EXPECT(c.value("pluggedIn", false));
    EXPECT(c["caps"].value("rumble", false));
    EXPECT(!c["caps"].value("mic", true));
    EXPECT_EQ(c.value("touchpadMode", std::string()), std::string("ds4"));
    EXPECT(c["motion"].value("sinkSupportedForType", false));
    EXPECT(c["motion"].value("backendOk", false));
    EXPECT(c.find("serial") == c.end());

    TEST("connection diagnostics: each controller's input and output blocks");
    EXPECT_EQ(c["input"].value("frames", 0), 12345);
    EXPECT_EQ(c["input"].value("rateHz", 0), 250);
    EXPECT_EQ(c["input"].value("lastAtMs", int64_t{0}), int64_t{9000});
    EXPECT_EQ(c["output"]["rumble"].value("count", 0), 12);
    EXPECT_EQ(c["output"]["rumble"]["last"].value("strong", 0), 32768);
    EXPECT(c["output"]["lightbar"]["last"].is_null());
    EXPECT_EQ(c["lastGameOutput"].value("kind", std::string()), std::string("rumble"));

    TEST("connection diagnostics: a failed replug shows the type asked for and the one in force");
    ConnectionDiagnosticsInput failed = inputFixture();
    failed.controllers[0].requestedType = CONTROLLER_TYPE_SWITCHPRO;
    const JsonOut f = buildConnectionDiagnostics(failed)["controllers"][0];
    EXPECT_EQ(f.value("requestedType", std::string()), std::string("switchpro"));
    EXPECT_EQ(f.value("appliedType", std::string()), std::string("dualsense"));

    TEST("connection diagnostics: a pad no backend serves has a null backend");
    ConnectionDiagnosticsInput orphan = inputFixture();
    orphan.controllers[0].backend.clear();
    EXPECT(buildConnectionDiagnostics(orphan)["controllers"][0]["backend"].is_null());
}

static void test_grants_and_counters() {
    TEST("connection diagnostics: the audio grants under controllerAudio");
    const JsonOut d = buildConnectionDiagnostics(inputFixture());
    EXPECT(d["grants"]["controllerAudio"].value("enabled", false));
    EXPECT(d["grants"]["controllerAudio"].value("speaker", false));
    EXPECT(!d["grants"]["controllerAudio"].value("mic", true));

    TEST("connection diagnostics: the counters say they are the process's, not the session's");
    EXPECT_EQ(d["counters"].value("scope", std::string()), std::string("process"));
    EXPECT_EQ(d["counters"]["rx"].value("input", 0), 40);
    EXPECT_EQ(d["counters"]["tx"].value("rumble", 0), 12);
    EXPECT_EQ(d["counters"]["audio"].value("hapticReducedToRumble", 0), 4210);
    EXPECT_EQ(d["counters"]["rejected"].value("decryptFail", 0), 1);
}

static void test_log_scope() {
    TEST("log scope: a line naming the connection's id or its device's id is about it");
    const ConnectionDiagnosticsInput in = inputFixture();
    const ConnectionLogScope scope = connectionLogScope(in);
    EXPECT(logLineConcernsConnection(in.log[0], scope));
    EXPECT(logLineConcernsConnection(in.log[3], scope));
    EXPECT(logLineConcernsConnection(in.log[7], scope));

    TEST("log scope: a line about another client or the machine alone is not");
    EXPECT(!logLineConcernsConnection(in.log[1], scope));
    EXPECT(!logLineConcernsConnection(in.log[2], scope));
    EXPECT(!logLineConcernsConnection(in.log[8], scope));

    TEST("log scope: a line naming a pad's serial counts only from that pad's plug on");
    EXPECT(!logLineConcernsConnection(in.log[4], scope));
    EXPECT(logLineConcernsConnection(in.log[5], scope));

    TEST("log scope: serial 3 is not serial 31");
    EXPECT(!logLineConcernsConnection(in.log[6], scope));
}

// A log fixture where the requester's chosen name occurs inside other
// clients' lines, as a short name or a name another client also carries.
ConnectionDiagnosticsInput nameCanaryFixture(const std::string& selfName) {
    ConnectionDiagnosticsInput in = inputFixture();
    in.self.deviceName = selfName;
    in.others = {{"dish-other-77", "CANARY-TWIN", "192.168.1.80"}};
    in.controllers.clear();
    in.log = {
        {100, "INFO", "service", "Session created for " + selfName + " (" + CONNECTION_ID + ")"},
        {200, "INFO", "service", "Session created for CANARY-TWIN (conn_99ee88dd)"},
        {300, "INFO", "service",
         "Controller #0 plugged as Xbox (serial 4) for CANARY-TWIN (conn_99ee88dd)"},
        {400, "WARN", "client",
         "401 unauthorized PUT /api/connections (BAD_PROOF, deviceId "
         "dish-other-77)"},
        {500, "INFO", "pairing", "Pairing request from CANARY-STRANGER-2 (10.0.0.9)"},
        {600, "INFO", "service", "Link to CANARY-TWIN (conn_99ee88dd) has gone silent"},
        {700, "WARN", "client",
         std::string("device ") + SELF_ID + " speaks protocol v2 (current v4)"},
    };
    return in;
}

std::vector<int64_t> selectedLineTimes(const ConnectionDiagnosticsInput& in) {
    std::vector<int64_t> out;
    const JsonOut doc = buildConnectionDiagnostics(in);
    for (const JsonOut& line : doc["log"]) { out.push_back(line.value("atMs", int64_t{0})); }
    return out;
}

static void test_log_scope_ignores_chosen_names() {
    TEST("log scope: a client named \"e\" gets only its own lines");
    const std::vector<int64_t> own = {100, 700};
    EXPECT((selectedLineTimes(nameCanaryFixture("e")) == own));

    TEST("log scope: a client named like another client gets only its own lines");
    EXPECT((selectedLineTimes(nameCanaryFixture("CANARY-TWIN")) == own));

    TEST("log scope: the connection id is a whole token, not a prefix of another id");
    ConnectionDiagnosticsInput prefix = nameCanaryFixture("Thor");
    prefix.log = {{1, "INFO", "service", std::string("Kicked ") + CONNECTION_ID + "9"}};
    EXPECT(selectedLineTimes(prefix).empty());
}

static void test_redaction() {
    TEST("connection diagnostics: no other client, machine name, address, key or secret appears");
    const JsonOut d = buildConnectionDiagnostics(inputFixture());
    EXPECT(!holdsForbidden(jsonDump(d)));
    EXPECT(!holdsAnyForbiddenKey(d));

    TEST("connection diagnostics: the requester is \"this connection\", the rest are named by "
         "role");
    EXPECT_EQ(d["log"].size(), size_t{4});
    EXPECT_EQ(d["log"][0].value("message", std::string()),
              std::string("Controller #0 plugged as DualSense (serial 3) for this connection "
                          "(conn_ab12cd34)"));
    EXPECT_EQ(d["log"][1].value("message", std::string()),
              std::string("Device this connection at this connection re-PUT beside another "
                          "client (another client) on this PC with key [redacted]"));
    EXPECT_EQ(d["log"][2].value("tag", std::string()), std::string("hidmaestro"));
    EXPECT_EQ(d["log"][2].value("atMs", int64_t{0}), int64_t{6000});
    EXPECT_EQ(d["log"][2].value("level", std::string()), std::string("WARN"));

    TEST("connection diagnostics: an address no client owns is masked");
    ConnectionDiagnosticsInput stranger = inputFixture();
    stranger.log = {{1, "WARN", "client", std::string("401 from 10.1.2.3 for ") + SELF_ID}};
    EXPECT_EQ(buildConnectionDiagnostics(stranger)["log"][0].value("message", std::string()),
              std::string("401 from x.x.x.x for this connection"));
}

static void test_log_tail() {
    TEST("connection diagnostics: only the last 200 lines about the connection, oldest first");
    ConnectionDiagnosticsInput in = inputFixture();
    in.log.clear();
    for (int i = 0; i < 300; i++) {
        in.log.push_back(
            {i, "INFO", "t", std::string("line ") + std::to_string(i) + " " + SELF_ID});
        in.log.push_back({i, "INFO", "t", "unrelated " + std::to_string(i)});
    }
    const JsonOut d = buildConnectionDiagnostics(in);
    EXPECT_EQ(d["log"].size(), CONNECTION_DIAGNOSTICS_LOG_LINES);
    EXPECT_EQ(d["log"][0].value("message", std::string()), std::string("line 100 this connection"));
    EXPECT_EQ(d["log"][199].value("message", std::string()),
              std::string("line 299 this connection"));
}

int main() {
    return satellite::test::runMain("connection_diagnostics", [] {
        test_envelope();
        test_controllers();
        test_grants_and_counters();
        test_log_scope();
        test_log_scope_ignores_chosen_names();
        test_redaction();
        test_log_tail();
    });
}
