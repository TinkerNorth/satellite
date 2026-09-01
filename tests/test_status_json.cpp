// SPDX-License-Identifier: LGPL-3.0-or-later
#include "../src/net/status_json.h"

#include <iostream>
#include <string>

#include "test_util.h"

using satellite::buildDebugJson;
using satellite::buildSseStatusObject;
using satellite::buildStatusJson;
using satellite::jsonDump;
using satellite::JsonOut;
using satellite::StatusFields;

static StatusFields makeFields() {
    StatusFields f;
    f.listening = true;
    f.packets = 12345;
    f.senderIP = "192.168.1.42";
    f.udpPort = 9876;
    f.webPort = 9871;
    f.autoStart = true;
    f.discoveryBroadcastEnabled = false;
    f.controllerAudio = false;
    f.mdnsResponderActive = true;
    f.backendAvailable = true;
    f.submitOk = 1000;
    f.submitFail = 7;
    f.lastLoopUs = 250;
    f.maxLoopUs = 9001;
    f.decryptFail = 3;
    f.replayDrop = 5;
    f.logSeq = 42;

    JsonOut backend;
    backend["kind"] = "vigem";
    backend["available"] = true;
    f.backend = backend;
    return f;
}

static void test_status_exact_shape() {
    TEST("buildStatusJson: exact JSON shape and field order");
    std::string s = buildStatusJson(makeFields());
    EXPECT_EQ(
        s,
        std::string(R"({"listening":true,"packets":12345,"senderIP":"192.168.1.42","udpPort":9876,)"
                    R"("webPort":9871,"autoStart":true,"discoveryBroadcastEnabled":false,)"
                    R"("controllerAudio":false,)"
                    R"("mdnsResponderActive":true,"backendAvailable":true,)"
                    R"("backend":{"kind":"vigem","available":true}})"));
}

// The dashboard seeds its toggle from /api/status and treats an absent key as
// on, so the key has to be there and has to be a real boolean.
static void test_status_carries_controller_audio() {
    TEST("buildStatusJson: controllerAudio round-trips both values");
    StatusFields f = makeFields();
    f.controllerAudio = true;
    EXPECT(buildStatusJson(f).find("\"controllerAudio\":true") != std::string::npos);
    f.controllerAudio = false;
    EXPECT(buildStatusJson(f).find("\"controllerAudio\":false") != std::string::npos);

    TEST("the debug and SSE payloads deliberately stay unchanged");
    // Neither surface drives the settings form, and both are hot: adding a
    // static preference to the SSE tick would ship it 4 times a second.
    EXPECT(buildDebugJson(f).find("controllerAudio") == std::string::npos);
    EXPECT(jsonDump(buildSseStatusObject(f)).find("controllerAudio") == std::string::npos);
}

static void test_debug_exact_shape() {
    TEST("buildDebugJson: exact JSON shape and field order");
    std::string s = buildDebugJson(makeFields());
    EXPECT_EQ(
        s,
        std::string(R"({"listening":true,"packets":12345,"submitOk":1000,"submitFail":7,)"
                    R"("lastLoopUs":250,"maxLoopUs":9001,"senderIP":"192.168.1.42","udpPort":9876,)"
                    R"("decryptFail":3,"replayDrop":5,"backendAvailable":true,)"
                    R"("backend":{"kind":"vigem","available":true}})"));
}

static void test_sse_exact_shape() {
    TEST("buildSseStatusObject: exact JSON shape and field order");
    std::string s = jsonDump(buildSseStatusObject(makeFields()));
    EXPECT_EQ(
        s,
        std::string(R"({"listening":true,"packets":12345,"senderIP":"192.168.1.42","udpPort":9876,)"
                    R"("autoStart":true,"backendAvailable":true,)"
                    R"("backend":{"kind":"vigem","available":true},)"
                    R"("submitOk":1000,"submitFail":7,"lastLoopUs":250,"decryptFail":3,)"
                    R"("replayDrop":5,"logSeq":42})"));
}

int main() {
    std::cout << "Running status JSON tests...\n\n";
    test_status_exact_shape();
    test_status_carries_controller_audio();
    test_debug_exact_shape();
    test_sse_exact_shape();

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
