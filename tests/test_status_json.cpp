// SPDX-License-Identifier: LGPL-3.0-or-later
#include "../src/net/status_json.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

#include "test_util.h"

using satellite::buildDebugJson;
using satellite::buildSseStatusObject;
using satellite::buildStatusJson;
using satellite::buildUpdateJson;
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
    f.peakLoopUs = 9100;
    f.clientApiListening = true;
    f.connections = 2;
    f.controllers = 3;
    f.maxControllers = 16;
    f.rx = {1007, 60, 900, 4, 120, 500, 2, 1, 8, 9};
    f.tx = {600, 48000, 60, 12, 4, 2, 1, 520, 3, 3, 1, 6, 0, 0, 11};
    f.audio = {500, 2, 1, 498, 1, 1, 520, 40, 0, 3};
    f.authNotPaired = 4;
    f.authBadProof = 2;
    f.sessionsReaped = 5;

    JsonOut backend;
    backend["kind"] = "vigem";
    backend["available"] = true;
    f.backend = backend;
    return f;
}

static UpdateStatusSnapshot makeUpdateSnapshot() {
    UpdateStatusSnapshot s;
    s.state = UpdateState::Downloading;
    s.currentVersion = "2.1.2";
    s.platformId = "windows";
    s.channel = "stable";
    s.autoCheck = true;
    s.autoDownload = false;
    s.autoInstall = false;
    s.lastCheckEpoch = 1759700000;
    s.bytesDownloaded = 4200000;
    s.totalBytes = 12000000;
    s.failedPhase = UpdateState::Idle;
    s.dismissed = false;
    s.info.available = true;
    s.info.version = "2.2.0";
    s.info.channel = "stable";
    s.info.assetName = "SatelliteSetup-2.2.0.exe";
    s.info.assetUrl = "https://example.invalid/never-serialized";
    s.info.assetSize = 12000000;
    s.info.assetSha256 = "ab12";
    s.info.htmlUrl = "https://github.com/TinkerNorth/satellite/releases/tag/2.2.0";
    s.info.publishedAtEpoch = 1759600000;
    s.info.installMethod = InstallMethod::SelfInstall;
    s.info.releaseNotes = "Fixes";
    return s;
}

static void test_update_exact_shape() {
    TEST("buildUpdateJson: exact JSON shape and field order, dismissed right after failedPhase");
    EXPECT_EQ(buildUpdateJson(makeUpdateSnapshot()),
              std::string(
                  R"({"state":"downloading","currentVersion":"2.1.2","platformId":"windows",)"
                  R"("channel":"stable","autoCheck":true,"autoDownload":false,"autoInstall":false,)"
                  R"("lastCheckEpoch":1759700000,"bytesDownloaded":4200000,"totalBytes":12000000,)"
                  R"("message":"","failedPhase":"idle","dismissed":false,"info":{"available":true,)"
                  R"("version":"2.2.0","channel":"stable","assetName":"SatelliteSetup-2.2.0.exe",)"
                  R"("assetSize":12000000,"assetSha256":"ab12",)"
                  R"("htmlUrl":"https://github.com/TinkerNorth/satellite/releases/tag/2.2.0",)"
                  R"("publishedAtEpoch":1759600000,"installMethod":"self","manualInstruction":"",)"
                  R"("releaseNotes":"Fixes"}})"));
}

static void test_update_carries_the_dismissal() {
    TEST("buildUpdateJson: dismissed round-trips both values, after failedPhase");
    UpdateStatusSnapshot s = makeUpdateSnapshot();
    s.state = UpdateState::Downloaded;
    s.dismissed = true;
    EXPECT(buildUpdateJson(s).find(R"("failedPhase":"idle","dismissed":true,"info")") !=
           std::string::npos);
    s.dismissed = false;
    EXPECT(buildUpdateJson(s).find(R"("failedPhase":"idle","dismissed":false,"info")") !=
           std::string::npos);
}

static void test_update_names_the_phase_and_the_install_method() {
    TEST("buildUpdateJson: an error names its failed phase");
    UpdateStatusSnapshot s = makeUpdateSnapshot();
    s.state = UpdateState::Error;
    s.failedPhase = UpdateState::Installing;
    s.message = "declined";
    const std::string failed = buildUpdateJson(s);
    EXPECT(failed.find(R"("state":"error")") != std::string::npos);
    EXPECT(failed.find(R"("message":"declined","failedPhase":"installing")") != std::string::npos);

    TEST("buildUpdateJson: a package-managed release says manual and carries its instruction");
    s.info.installMethod = InstallMethod::Manual;
    s.info.manualInstruction = "sudo apt upgrade satellite";
    const std::string manual = buildUpdateJson(s);
    EXPECT(manual.find(R"("installMethod":"manual")") != std::string::npos);
    EXPECT(manual.find(R"("manualInstruction":"sudo apt upgrade satellite")") != std::string::npos);
}

static void test_status_exact_shape() {
    TEST("buildStatusJson: exact JSON shape and field order");
    std::string s = buildStatusJson(makeFields());
    EXPECT_EQ(
        s, std::string(
               R"({"listening":true,"packets":12345,"senderIP":"192.168.1.42","udpPort":9876,)"
               R"("webPort":9871,"autoStart":true,"discoveryBroadcastEnabled":false,)"
               R"("controllerAudio":false,"controllerAudioMic":true,"controllerAudioSpeaker":true,)"
               R"("controllerAudioHaptics":true,"controllerAudioKeepDefaultDevice":true,)"
               R"("crashReporting":false,"crashReportingActive":false,)"
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

// The two direction switches ride the same surface for the same reason: the
// settings form seeds all three from one /api/status read, and a missing key
// there would render as a toggle that silently disagrees with the server.
static void test_status_carries_the_audio_directions() {
    TEST("buildStatusJson: each direction round-trips independently");
    StatusFields f = makeFields();
    f.controllerAudioMic = true;
    f.controllerAudioSpeaker = false;
    f.controllerAudioHaptics = false;
    std::string s = buildStatusJson(f);
    EXPECT(s.find("\"controllerAudioMic\":true") != std::string::npos);
    EXPECT(s.find("\"controllerAudioSpeaker\":false") != std::string::npos);
    EXPECT(s.find("\"controllerAudioHaptics\":false") != std::string::npos);

    f.controllerAudioMic = false;
    f.controllerAudioSpeaker = true;
    s = buildStatusJson(f);
    EXPECT(s.find("\"controllerAudioMic\":false") != std::string::npos);
    EXPECT(s.find("\"controllerAudioSpeaker\":true") != std::string::npos);

    TEST("the directions are independent of the master switch in the payload");
    f.controllerAudio = false;
    f.controllerAudioMic = true;
    f.controllerAudioSpeaker = true;
    s = buildStatusJson(f);
    EXPECT(s.find("\"controllerAudio\":false") != std::string::npos);
    EXPECT(s.find("\"controllerAudioMic\":true") != std::string::npos);

    TEST("the debug and SSE payloads stay free of them too");
    EXPECT(buildDebugJson(f).find("controllerAudioMic") == std::string::npos);
    EXPECT(buildDebugJson(f).find("controllerAudioSpeaker") == std::string::npos);
    EXPECT(jsonDump(buildSseStatusObject(f)).find("controllerAudioMic") == std::string::npos);
    EXPECT(jsonDump(buildSseStatusObject(f)).find("controllerAudioSpeaker") == std::string::npos);
}

static void test_counterBlocksStayOffTheHotSurfaces() {
    TEST("the rx/tx/audio/auth blocks ride /api/debug only");
    StatusFields f = makeFields();
    const std::string status = buildStatusJson(f);
    const std::string sse = jsonDump(buildSseStatusObject(f));
    for (const char* key : {"\"rx\"", "\"tx\"", "\"audio\"", "\"auth\"", "peakLoopUs",
                            "clientApiListening", "sessionsReaped", "maxControllers"}) {
        EXPECT(status.find(key) == std::string::npos);
        EXPECT(sse.find(key) == std::string::npos);
    }

    TEST("buildDebugJson carries every block");
    const std::string dbg = buildDebugJson(f);
    for (const char* key : {"\"rx\"", "\"tx\"", "\"audio\"", "\"auth\"", "peakLoopUs",
                            "clientApiListening", "sessionsReaped", "maxControllers", "webPort"}) {
        EXPECT(dbg.find(key) != std::string::npos);
    }
}

static void test_debug_exact_shape() {
    TEST("buildDebugJson: exact JSON shape and field order");
    std::string s = buildDebugJson(makeFields());
    EXPECT_EQ(
        s,
        std::string(
            R"({"listening":true,"packets":12345,"submitOk":1000,"submitFail":7,)"
            R"("lastLoopUs":250,"maxLoopUs":9001,"peakLoopUs":9100,"senderIP":"192.168.)"
            R"(1.42","udpPort":9876,"webPort":9871,"decryptFail":3,"replayDrop":5,)"
            R"("backendAvailable":true,"backend":{"kind":"vigem","available":true},)"
            R"("mdnsResponderActive":true,"clientApiListening":true,"connections":2,)"
            R"("controllers":3,"maxControllers":16,"rx":{"input":1007,)"
            R"("heartbeat":60,"motion":900,"battery":4,"pointer":120,"micAudio":500,)"
            R"("malformed":2,"unknownType":1,"runt":8,"unknownToken":9},)"
            R"("tx":{"packets":600,"bytes":48000,"heartbeatAck":60,"rumble":12,)"
            R"("lightbar":4,"triggerEffects":2,"playerLeds":1,"speakerAudio":520,"hapticAudio":3,)"
            R"("micLed":3,"sessionClose":1,"unroutable":6,"encryptFailed":0,)"
            R"("oversize":0,"sendFailed":11},"audio":{"micAccepted":500,)"
            R"("micDropped":2,"micLate":1,"micDecoded":498,"micFecRecovered":1,)"
            R"("micConcealed":1,"speakerSent":520,"speakerSilenceSuppressed":40,)"
            R"("speakerEncodeFailed":0,"speakerLockContended":3,"hapticSent":0,"hapticSilenceSuppressed":0,)"
            R"("hapticEncodeFailed":0,"hapticLockContended":0,"hapticReducedToRumble":0},"auth":{"notPaired":4,)"
            R"("badProof":2},"sessionsReaped":5})"));
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

static void test_senderIpLabel() {
    TEST("senderIpLabel: none until a packet has arrived, then the dotted quad");
    EXPECT_EQ(satellite::senderIpLabel(0), std::string("none"));
    const uint8_t octets[4] = {192, 168, 4, 20};
    uint32_t nbo = 0;
    std::memcpy(&nbo, octets, sizeof(nbo));
    EXPECT_EQ(satellite::senderIpLabel(nbo), std::string("192.168.4.20"));
}

static void test_sseEventFraming() {
    TEST("sseEvent frames one event the way EventSource reads it");
    EXPECT_EQ(satellite::sseEvent("status", "{}"), std::string("event: status\ndata: {}\n\n"));
}

int main() {
    std::cout << "Running status JSON tests...\n\n";
    test_status_exact_shape();
    test_status_carries_controller_audio();
    test_status_carries_the_audio_directions();
    test_debug_exact_shape();
    test_sse_exact_shape();
    test_counterBlocksStayOffTheHotSurfaces();
    test_senderIpLabel();
    test_sseEventFraming();
    test_update_exact_shape();
    test_update_carries_the_dismissal();
    test_update_names_the_phase_and_the_install_method();

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
