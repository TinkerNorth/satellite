// SPDX-License-Identifier: LGPL-3.0-or-later

// SessionService under the declarative contract (docs/contract.md): session
// upsert/converge, transactional replug, serial lifecycle (round-robin +
// quarantine), epoch/bitmap reconcile material, close-notify ordering,
// liveness grace + reap, host-feature grants, and the data streams.
// Pure codec coverage (IPv4 util, touchpad/motion wire decode) lives in the
// dedicated test_ipv4_util / test_codecs suites.
#include "../src/core/session_service.h"
#include "../src/core/touchpad_codec.h"
#include "../src/core/gamepad_backend.h"
#include "../src/core/ipv4_util.h"
// The real codec, not a fake: the audio suites decode actual Opus produced by
// the same wrapper the client's encoder mirrors, so a stub that happened to
// agree with a wrong implementation cannot hide behind them.
#include "../src/adapters/audio/opus_codec.h"
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <set>
#include <sstream>
#include <thread>
#include <chrono>
#include <unordered_map>

#include "test_util.h"

// A DS4/DualSense slot has a touchpad surface; Xbox and Switch Pro do not. The
// mock mirrors that so a touchpad routed to a non-touchpad slot reads as dropped.
static bool identityHasTouchpad(GamepadIdentity id) {
    return id == GamepadIdentity::DS4 || id == GamepadIdentity::DualSense;
}

struct MockViGem : IGamepadPort {
    bool busOpen = false;
    bool ensureBusReturnVal = true;
    bool pluginReturnVal = true;
    bool submitReturnVal = true;
    bool unplugReturnVal = true;
    bool supportsRelativeMouseVal = true;

    int ensureBusCalls = 0;
    int closeBusCalls = 0;
    int pluginCalls = 0;
    int pluginDS4Calls = 0;
    int unplugCalls = 0;
    int submitCalls = 0;
    int submitDS4Calls = 0;
    int setRumbleCallbackCalls = 0;
    int submitMotionCalls = 0;
    int submitBatteryCalls = 0;
    int submitTouchpadCalls = 0;
    int submitRelativeMouseCalls = 0;
    int setLightbarCallbackCalls = 0;

    bool submitMotionReturnVal = true;
    bool submitBatteryReturnVal = true;
    bool submitTouchpadReturnVal = true;
    bool submitRelativeMouseReturnVal = true;

    std::vector<uint32_t> pluggedSerials;
    std::vector<uint32_t> unpluggedSerials;
    // Adapter truth for isDevicePlugged, maintained by plugin/unplug; a test can
    // erase entries to simulate a target that vanished driver-side.
    std::set<uint32_t> pluggedSet;
    GamepadReport lastSubmittedReport{};
    uint32_t lastMotionSerial = 0;
    MotionReport lastMotion{};
    uint32_t lastBatterySerial = 0;
    BatteryReport lastBattery{};
    uint32_t lastTouchpadSerial = 0;
    TouchpadReport lastTouchpad{};
    int lastMouseDx = 0;
    int lastMouseDy = 0;
    bool lastMouseButton = false;
    bool lastMouseRight = false;
    bool lastMouseMiddle = false;
    int lastMouseWheel = 0;
    // Tests synthesize "the platform fired a notification" by invoking these via
    // fireRumble / fireLightbar.
    RumbleCallback capturedRumbleCb;
    LightbarCallback capturedLightbarCb;

    // Identity recorded per serial at plug (several tests assert which pad was
    // plugged); lastIdentity is the most recent plug's.
    std::unordered_map<uint32_t, GamepadIdentity> identityBySerial;
    GamepadIdentity lastIdentity = GamepadIdentity::Xbox;
    // Identities this backend refuses (empty = fully capable). A test inserts
    // one to drive the APPLY_ERR_INVALID_TYPE path.
    std::set<GamepadIdentity> unsupportedIdentities;
    GamepadIdentity identityFor(uint32_t serial) const {
        auto it = identityBySerial.find(serial);
        return it != identityBySerial.end() ? it->second : GamepadIdentity::Xbox;
    }

    bool ensureBusOpen() override {
        ensureBusCalls++;
        if (ensureBusReturnVal) busOpen = true;
        return ensureBusReturnVal;
    }
    void closeBus() override {
        closeBusCalls++;
        busOpen = false;
        pluggedSet.clear();
    }
    bool isBusOpen() const override { return busOpen; }
    bool pluginDevice(uint32_t serial, GamepadIdentity identity) override {
        // Split the tally by identity so existing Xbox-vs-DS4 plug-count asserts
        // still read; DualSense/SwitchPro plugs are asserted via identityFor.
        if (identity == GamepadIdentity::DS4)
            pluginDS4Calls++;
        else
            pluginCalls++;
        lastIdentity = identity;
        identityBySerial[serial] = identity;
        pluggedSerials.push_back(serial);
        if (pluginReturnVal) pluggedSet.insert(serial);
        return pluginReturnVal;
    }
    bool supportsIdentity(GamepadIdentity identity) const override {
        return unsupportedIdentities.count(identity) == 0;
    }
    bool unplugDevice(uint32_t serial) override {
        unplugCalls++;
        unpluggedSerials.push_back(serial);
        pluggedSet.erase(serial);
        return unplugReturnVal;
    }
    bool isDevicePlugged(uint32_t serial) const override { return pluggedSet.count(serial) != 0; }
    bool submitReport(uint32_t serial, const GamepadReport& r) override {
        // Route the tally by the serial's plugged identity so "XUSB vs DS4"
        // stays observable under the unified submit.
        if (identityFor(serial) == GamepadIdentity::DS4)
            submitDS4Calls++;
        else
            submitCalls++;
        lastSubmittedReport = r;
        return submitReturnVal;
    }
    void setRumbleCallback(RumbleCallback cb) override {
        setRumbleCallbackCalls++;
        capturedRumbleCb = std::move(cb);
    }
    void fireRumble(uint32_t serial, const RumbleReport& r) {
        if (capturedRumbleCb) capturedRumbleCb(serial, r);
    }
    bool submitMotion(uint32_t serial, const MotionReport& r) override {
        submitMotionCalls++;
        lastMotionSerial = serial;
        lastMotion = r;
        return submitMotionReturnVal;
    }
    bool submitBattery(uint32_t serial, const BatteryReport& r) override {
        submitBatteryCalls++;
        lastBatterySerial = serial;
        lastBattery = r;
        return submitBatteryReturnVal;
    }
    bool submitTouchpad(uint32_t serial, const TouchpadReport& r) override {
        submitTouchpadCalls++;
        lastTouchpadSerial = serial;
        lastTouchpad = r;
        // Real backends land touchpad only on a DS4/DualSense surface; Xbox and
        // Switch Pro slots have no touchpad node and drop it.
        return submitTouchpadReturnVal && identityHasTouchpad(identityFor(serial));
    }
    bool submitRelativeMouse(int dx, int dy, const MouseButtons& buttons, int wheelV) override {
        submitRelativeMouseCalls++;
        lastMouseDx = dx;
        lastMouseDy = dy;
        lastMouseButton = buttons.left;
        lastMouseRight = buttons.right;
        lastMouseMiddle = buttons.middle;
        lastMouseWheel = wheelV;
        return submitRelativeMouseReturnVal;
    }
    bool supportsRelativeMouse() const override { return supportsRelativeMouseVal; }
    void setLightbarCallback(LightbarCallback cb) override {
        setLightbarCallbackCalls++;
        capturedLightbarCb = std::move(cb);
    }
    void fireLightbar(uint32_t serial, uint8_t r, uint8_t g, uint8_t b) {
        if (capturedLightbarCb) capturedLightbarCb(serial, r, g, b);
    }
    void setTriggerEffectsCallback(TriggerEffectsCallback cb) override {
        setTriggerEffectsCallbackCalls++;
        capturedTriggerEffectsCb = std::move(cb);
    }
    void fireTriggerEffects(uint32_t serial, const TriggerEffectsReport& report) {
        if (capturedTriggerEffectsCb) capturedTriggerEffectsCb(serial, report);
    }
    void setPlayerLedsCallback(PlayerLedsCallback cb) override {
        setPlayerLedsCallbackCalls++;
        capturedPlayerLedsCb = std::move(cb);
    }
    void firePlayerLeds(uint32_t serial, uint8_t mask) {
        if (capturedPlayerLedsCb) capturedPlayerLedsCb(serial, mask);
    }
    void setSpeakerAudioCallback(SpeakerAudioCallback cb) override {
        setSpeakerAudioCallbackCalls++;
        capturedSpeakerAudioCb = std::move(cb);
    }
    void fireSpeakerAudio(uint32_t serial, const int16_t* pcm, size_t frames) {
        if (capturedSpeakerAudioCb) capturedSpeakerAudioCb(serial, pcm, frames);
    }
    void setMicLedCallback(MicLedCallback cb) override {
        setMicLedCallbackCalls++;
        capturedMicLedCb = std::move(cb);
    }
    void fireMicLed(uint32_t serial, uint8_t state) {
        if (capturedMicLedCb) capturedMicLedCb(serial, state);
    }
    bool submitMicAudioPcm(uint32_t serial, const int16_t* mono48k, size_t samples) override {
        submitMicAudioCalls++;
        lastMicAudioSerial = serial;
        lastMicAudioSamples = samples;
        lastMicAudioPcm.assign(mono48k, mono48k + samples);
        // Per submission, because one packet can flush several frames at once
        // (a concealed gap plus the packets behind it) and only the tail would
        // otherwise be observable.
        micAudioEnergies.push_back(meanSquare(mono48k, samples));
        return submitMicAudioReturnVal;
    }

    static double meanSquare(const int16_t* pcm, size_t samples) {
        if (samples == 0) return 0.0;
        double sum = 0.0;
        for (size_t i = 0; i < samples; i++) sum += static_cast<double>(pcm[i]) * pcm[i];
        return sum / static_cast<double>(samples);
    }

    // Mean square of the last PCM handed over. Distinguishes "the decoder ran"
    // from "a zeroed buffer was submitted", which a call count cannot.
    double lastMicAudioEnergy() const {
        return meanSquare(lastMicAudioPcm.data(), lastMicAudioPcm.size());
    }

    int setTriggerEffectsCallbackCalls = 0;
    int setPlayerLedsCallbackCalls = 0;
    int setSpeakerAudioCallbackCalls = 0;
    int setMicLedCallbackCalls = 0;
    int submitMicAudioCalls = 0;
    uint32_t lastMicAudioSerial = 0;
    size_t lastMicAudioSamples = 0;
    std::vector<int16_t> lastMicAudioPcm;
    std::vector<double> micAudioEnergies;
    bool submitMicAudioReturnVal = true;
    TriggerEffectsCallback capturedTriggerEffectsCb;
    PlayerLedsCallback capturedPlayerLedsCb;
    SpeakerAudioCallback capturedSpeakerAudioCb;
    MicLedCallback capturedMicLedCb;

    // Default mirrors real Windows/Linux: the motion-capable types
    // (controllerTypeHasMotion) have an IMU sink, Xbox does not.
    std::unordered_map<uint8_t, bool> supportsMotionForTypeMap{
        {CONTROLLER_TYPE_XBOX, false},
        {CONTROLLER_TYPE_PLAYSTATION, true},
        {CONTROLLER_TYPE_DUALSENSE, true},
        {CONTROLLER_TYPE_SWITCHPRO, true},
    };
    bool supportsMotionForType(uint8_t controllerType) const override {
        auto it = supportsMotionForTypeMap.find(controllerType);
        return it != supportsMotionForTypeMap.end() ? it->second : false;
    }

    // Default true; tests flip a serial's entry to exercise the kernel-rejected path.
    std::unordered_map<uint32_t, bool> motionBackendOkMap;
    bool motionBackendOk(uint32_t serial) const override {
        auto it = motionBackendOkMap.find(serial);
        return it != motionBackendOkMap.end() ? it->second : true;
    }
};

struct MockClient : IClientPort {
    int updateAddrCalls = 0;
    int updateAddrV4Calls = 0;
    uint32_t lastV4Token = 0;
    uint32_t lastV4IPv4Nbo = 0;
    uint16_t lastV4Port = 0;
    int removeAddrCalls = 0;

    int heartbeatAckCalls = 0;
    bool lastAckBackendAvailable = false;
    uint8_t lastAckTotalControllers = 0;
    uint16_t lastAckEpoch = 0;
    uint16_t lastAckBitmap = 0;

    int sessionCloseCalls = 0;
    std::vector<std::pair<uint32_t, uint8_t>> closeNotifies; // (token, reason)
    // Optional ordering probe, fired inside sendSessionClose (e.g. to assert
    // the notify precedes teardown's unplugs).
    std::function<void()> onSessionClose;

    int rumbleCalls = 0;
    int lightbarCalls = 0;

    uint32_t lastRumbleConnToken = 0;
    uint8_t lastRumbleCtrlIdx = 0;
    RumbleReport lastRumble{};

    uint32_t lastLightbarConnToken = 0;
    uint8_t lastLightbarCtrlIdx = 0;
    uint8_t lastLightbarR = 0;
    uint8_t lastLightbarG = 0;
    uint8_t lastLightbarB = 0;

    void updateClientAddr(uint32_t, const std::string&, uint16_t) override { updateAddrCalls++; }
    void updateClientAddrV4(uint32_t token, uint32_t ipv4Nbo, uint16_t port) override {
        updateAddrV4Calls++;
        lastV4Token = token;
        lastV4IPv4Nbo = ipv4Nbo;
        lastV4Port = port;
    }
    void removeClientAddr(uint32_t) override { removeAddrCalls++; }
    void sendHeartbeatAck(const Connection&, bool backendAvailable, uint8_t totalActiveControllers,
                          uint16_t epoch, uint16_t activeBitmap) override {
        heartbeatAckCalls++;
        lastAckBackendAvailable = backendAvailable;
        lastAckTotalControllers = totalActiveControllers;
        lastAckEpoch = epoch;
        lastAckBitmap = activeBitmap;
    }
    void sendSessionClose(const Connection& conn, uint8_t reason) override {
        sessionCloseCalls++;
        closeNotifies.push_back({conn.token, reason});
        if (onSessionClose) onSessionClose();
    }
    void sendRumble(const Connection& conn, uint8_t ctrlIdx, const RumbleReport& report) override {
        rumbleCalls++;
        lastRumbleConnToken = conn.token;
        lastRumbleCtrlIdx = ctrlIdx;
        lastRumble = report;
    }
    void sendLightbar(const Connection& conn, uint8_t ctrlIdx, uint8_t r, uint8_t g,
                      uint8_t b) override {
        lightbarCalls++;
        lastLightbarConnToken = conn.token;
        lastLightbarCtrlIdx = ctrlIdx;
        lastLightbarR = r;
        lastLightbarG = g;
        lastLightbarB = b;
    }
    void sendTriggerEffects(const Connection& conn, uint8_t ctrlIdx,
                            const TriggerEffectsReport& report) override {
        triggerEffectsCalls++;
        lastTriggerEffectsConnToken = conn.token;
        lastTriggerEffectsCtrlIdx = ctrlIdx;
        lastTriggerEffects = report;
    }
    void sendPlayerLeds(const Connection& conn, uint8_t ctrlIdx, uint8_t ledMask) override {
        playerLedsCalls++;
        lastPlayerLedsConnToken = conn.token;
        lastPlayerLedsCtrlIdx = ctrlIdx;
        lastPlayerLeds = ledMask;
    }
    void sendSpeakerAudio(const Connection& conn, uint8_t ctrlIdx, uint16_t seq,
                          const uint8_t* opus, size_t opusLen) override {
        speakerAudioCalls++;
        lastSpeakerAudioConnToken = conn.token;
        lastSpeakerAudioCtrlIdx = ctrlIdx;
        lastSpeakerAudioSeq = seq;
        lastSpeakerAudioOpus.assign(opus, opus + opusLen);
    }
    void sendMicLed(const Connection& conn, uint8_t ctrlIdx, uint8_t state) override {
        micLedCalls++;
        lastMicLedConnToken = conn.token;
        lastMicLedCtrlIdx = ctrlIdx;
        lastMicLedState = state;
    }

    int speakerAudioCalls = 0;
    uint32_t lastSpeakerAudioConnToken = 0;
    uint8_t lastSpeakerAudioCtrlIdx = 0;
    uint16_t lastSpeakerAudioSeq = 0;
    std::vector<uint8_t> lastSpeakerAudioOpus;

    int micLedCalls = 0;
    uint32_t lastMicLedConnToken = 0;
    uint8_t lastMicLedCtrlIdx = 0;
    uint8_t lastMicLedState = 0;

    int triggerEffectsCalls = 0;
    uint32_t lastTriggerEffectsConnToken = 0;
    uint8_t lastTriggerEffectsCtrlIdx = 0;
    TriggerEffectsReport lastTriggerEffects{};

    int playerLedsCalls = 0;
    uint32_t lastPlayerLedsConnToken = 0;
    uint8_t lastPlayerLedsCtrlIdx = 0;
    uint8_t lastPlayerLeds = 0;
};

struct MockLog : ILogPort {
    int logCalls = 0;
    std::vector<std::string> messages;

    void logMsg(LogLevel, const std::string&, const std::string& msg) override {
        logCalls++;
        messages.push_back(msg);
    }
    bool contains(const std::string& needle) const {
        for (auto& m : messages) {
            if (m.find(needle) != std::string::npos) return true;
        }
        return false;
    }
    // For the once-per-session-per-cause drop lines: presence is not enough,
    // the point is that a repeated cause does NOT repeat the line.
    int countContaining(const std::string& needle) const {
        int n = 0;
        for (auto& m : messages) {
            if (m.find(needle) != std::string::npos) n++;
        }
        return n;
    }
};

static const uint8_t TEST_KEY[CRYPTO_KEY_SIZE] = {1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11,
                                                  12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22,
                                                  23, 24, 25, 26, 27, 28, 29, 30, 31, 32};

static ControllerDescriptor makeDesc(uint8_t idx, uint8_t type = CONTROLLER_TYPE_XBOX,
                                     uint16_t caps = 0, uint8_t touchpadMode = TOUCHPAD_MODE_OFF) {
    ControllerDescriptor d;
    d.ctrlIdx = idx;
    d.type = type;
    d.caps = caps;
    d.touchpadMode = touchpadMode;
    return d;
}

static SessionUpsertResult
upsert(SessionService& svc, const std::vector<ControllerDescriptor>& descriptors = {},
       const std::string& devId = "dev1", const std::string& devName = "TestDevice",
       bool mouseControl = false, int protocolVersion = PROTOCOL_VERSION) {
    return svc.upsertSession(devId, devName, "192.168.1.100", TEST_KEY, descriptors, mouseControl,
                             protocolVersion);
}

static void test_upsert_recordsProtocolVersion() {
    TEST("upsert: the negotiated protocol version rides the result and the snapshot");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r1 = upsert(svc, {makeDesc(0)}, "dev1", "D", false, 1);
    EXPECT_EQ(r1.protocolVersion, 1);
    auto snap1 = svc.getConnectionsSnapshot();
    EXPECT_EQ(snap1.connections.size(), size_t{1});
    if (!snap1.connections.empty()) EXPECT_EQ(snap1.connections[0].protocolVersion, 1);

    // A re-PUT from an updated client raises the session in place.
    auto r2 = upsert(svc, {makeDesc(0)}, "dev1", "D", false, PROTOCOL_VERSION);
    EXPECT_EQ(r2.protocolVersion, PROTOCOL_VERSION);
    auto snap2 = svc.getConnectionsSnapshot();
    if (!snap2.connections.empty()) {
        EXPECT_EQ(snap2.connections[0].protocolVersion, PROTOCOL_VERSION);
    }
}

static void test_upsert_createsSession() {
    TEST("upsert: creates a session row");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = upsert(svc);
    EXPECT(r.ok);
    EXPECT(r.token != 0);
    EXPECT_EQ(r.connectionId.substr(0, 5), std::string("conn_"));
    EXPECT_EQ(r.maxControllers, 16);
    EXPECT_EQ((int)r.controllers.size(), 0);
    EXPECT(svc.isDeviceConnected("dev1"));
    EXPECT(log.logCalls > 0);
}

static void test_upsert_zeroControllerSessionIsValid() {
    TEST("upsert: zero-controller session is valid and persists");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = upsert(svc, {});
    EXPECT(r.ok);
    EXPECT_EQ(vigem.pluginCalls + vigem.pluginDS4Calls, 0);
    auto snap = svc.getConnectionsSnapshot();
    EXPECT_EQ((int)snap.connections.size(), 1);
    EXPECT_EQ(snap.connections[0].activeControllerCount, 0);
}

static void test_upsert_idempotent_stableConnectionId_rotatingToken() {
    TEST("upsert: same device twice: same connectionId, token rotates, no pad churn");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r1 = upsert(svc, {makeDesc(0)});
    EXPECT_EQ(vigem.pluginCalls, 1);
    auto r2 = upsert(svc, {makeDesc(0)});
    EXPECT(r2.ok);
    EXPECT_EQ(r2.connectionId, r1.connectionId);
    EXPECT(r2.token != r1.token);
    // The converge matched the existing slot: no unplug, no second plug.
    EXPECT_EQ(vigem.pluginCalls, 1);
    EXPECT_EQ(vigem.unplugCalls, 0);
    // Exactly one row remains.
    auto snap = svc.getConnectionsSnapshot();
    EXPECT_EQ((int)snap.connections.size(), 1);
    // The old token is dead, the new one routes.
    uint8_t key[CRYPTO_KEY_SIZE];
    uint32_t ctr;
    EXPECT(!svc.getDecryptInfo(r1.token, key, ctr));
    EXPECT(svc.getDecryptInfo(r2.token, key, ctr));
}

static void test_upsert_rotation_notifiesOldTokenReplaced() {
    TEST("upsert: rotation sends close-notify(replaced) on the OLD token");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r1 = upsert(svc);
    auto r2 = upsert(svc);
    EXPECT_EQ(client.sessionCloseCalls, 1);
    EXPECT_EQ(client.closeNotifies[0].first, r1.token);
    EXPECT_EQ((int)client.closeNotifies[0].second, (int)CLOSE_REASON_REPLACED);
    (void)r2;
}

static void test_upsert_saltAndCounterRotate() {
    TEST("upsert: fresh salt + counter reset on every PUT");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r1 = upsert(svc);
    // Advance the counter as packets would.
    svc.updatePostDecryptV4(r1.token, 42, 0x0100007f, 5555);
    auto r2 = upsert(svc);
    uint8_t key[CRYPTO_KEY_SIZE];
    uint32_t ctr = 999;
    EXPECT(svc.getDecryptInfo(r2.token, key, ctr));
    EXPECT_EQ(ctr, (uint32_t)0); // replay window restarts with the fresh key
    EXPECT(std::memcmp(r1.sessionSalt, r2.sessionSalt, SESSION_SALT_SIZE) != 0);
}

static void test_upsert_keyDeriverIsUsed() {
    TEST("upsert: injected KeyDeriver output becomes the session key");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(
        vigem, client, log,
        [](const uint8_t pairingKey[CRYPTO_KEY_SIZE], const uint8_t salt[SESSION_SALT_SIZE],
           uint32_t token, uint8_t out[CRYPTO_KEY_SIZE]) {
            for (int i = 0; i < CRYPTO_KEY_SIZE; i++) {
                out[i] = static_cast<uint8_t>(pairingKey[i] ^ salt[i % 8] ^ (uint8_t)token);
            }
        });

    auto r = upsert(svc);
    uint8_t key[CRYPTO_KEY_SIZE];
    uint32_t ctr;
    EXPECT(svc.getDecryptInfo(r.token, key, ctr));
    uint8_t expected[CRYPTO_KEY_SIZE];
    for (int i = 0; i < CRYPTO_KEY_SIZE; i++) {
        expected[i] = static_cast<uint8_t>(TEST_KEY[i] ^ r.sessionSalt[i % 8] ^ (uint8_t)r.token);
    }
    EXPECT(std::memcmp(key, expected, CRYPTO_KEY_SIZE) == 0);
    EXPECT(std::memcmp(key, TEST_KEY, CRYPTO_KEY_SIZE) != 0);
}

static void test_upsert_defaultDeriverCopiesPairingKey() {
    TEST("upsert: without a deriver the pairing key is copied (test mode)");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = upsert(svc);
    uint8_t key[CRYPTO_KEY_SIZE];
    uint32_t ctr;
    EXPECT(svc.getDecryptInfo(r.token, key, ctr));
    EXPECT(std::memcmp(key, TEST_KEY, CRYPTO_KEY_SIZE) == 0);
}

static void test_upsert_keyCopiedNotAliased() {
    TEST("upsert: pairing key is copied by value (caller buffer can die)");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    uint8_t volatileKey[CRYPTO_KEY_SIZE];
    std::memcpy(volatileKey, TEST_KEY, CRYPTO_KEY_SIZE);
    auto r = svc.upsertSession("dev1", "D", "1.1.1.1", volatileKey, {}, false);
    std::memset(volatileKey, 0xEE, CRYPTO_KEY_SIZE);
    uint8_t key[CRYPTO_KEY_SIZE];
    uint32_t ctr;
    EXPECT(svc.getDecryptInfo(r.token, key, ctr));
    EXPECT(std::memcmp(key, TEST_KEY, CRYPTO_KEY_SIZE) == 0);
}

static void test_upsert_firstPlugCarriesFinalType() {
    TEST("upsert: DS4 descriptor plugs DS4 on the FIRST try (no Xbox-default phase)");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_PLAYSTATION)});
    EXPECT_EQ(vigem.pluginDS4Calls, 1);
    EXPECT_EQ(vigem.pluginCalls, 0); // never plugged as Xbox
    EXPECT_EQ(vigem.unplugCalls, 0); // and never corrected via replug
    EXPECT_EQ((int)r.controllers.size(), 1);
    EXPECT_EQ((int)r.controllers[0].result, (int)APPLY_OK);
    EXPECT_EQ((int)r.controllers[0].appliedType, (int)CONTROLLER_TYPE_PLAYSTATION);
}

static void test_upsert_convergeRemovesAbsentSlots() {
    TEST("upsert: slots absent from the desired set are unplugged");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    upsert(svc, {makeDesc(0), makeDesc(1)});
    EXPECT_EQ(svc.totalActiveControllers(), 2);

    auto r = upsert(svc, {makeDesc(0)});
    EXPECT_EQ(svc.totalActiveControllers(), 1);
    EXPECT_EQ(vigem.unplugCalls, 1);
    EXPECT_EQ((int)r.controllers.size(), 1);
}

static void test_upsert_convergeToZeroKeepsSession() {
    TEST("upsert: converge to zero controllers keeps the session (user in menus)");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r1 = upsert(svc, {makeDesc(0)});
    auto r2 = upsert(svc, {});
    EXPECT_EQ(svc.totalActiveControllers(), 0);
    EXPECT_EQ(r2.connectionId, r1.connectionId);
    EXPECT(svc.isDeviceConnected("dev1"));
    // Bus idles closed once the last pad is gone.
    EXPECT(!vigem.busOpen);
}

static void test_upsert_addsNewSlots() {
    TEST("upsert: new descriptor indexes are plugged");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    upsert(svc, {makeDesc(0)});
    auto r = upsert(svc, {makeDesc(0), makeDesc(2, CONTROLLER_TYPE_PLAYSTATION)});
    EXPECT_EQ(svc.totalActiveControllers(), 2);
    EXPECT_EQ(vigem.pluginDS4Calls, 1);
    EXPECT_EQ((int)r.controllers.size(), 2);
}

static void test_upsert_duplicateIdxLastWins() {
    TEST("upsert: duplicate ctrlIdx in one body: last write wins, one result");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r =
        upsert(svc, {makeDesc(0, CONTROLLER_TYPE_XBOX), makeDesc(0, CONTROLLER_TYPE_PLAYSTATION)});
    EXPECT_EQ((int)r.controllers.size(), 1);
    EXPECT_EQ((int)r.controllers[0].appliedType, (int)CONTROLLER_TYPE_PLAYSTATION);
    EXPECT_EQ(vigem.pluginDS4Calls, 1);
    EXPECT_EQ(vigem.pluginCalls, 0);
}

static void test_upsert_invalidTypeReported() {
    TEST("upsert: unknown type id → invalidType, nothing plugged");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = upsert(svc, {makeDesc(0, 99)});
    EXPECT_EQ((int)r.controllers[0].result, (int)APPLY_ERR_INVALID_TYPE);
    EXPECT_EQ(vigem.pluginCalls + vigem.pluginDS4Calls, 0);
    EXPECT_EQ(svc.totalActiveControllers(), 0);
}

static void test_upsert_invalidIndexReported() {
    TEST("upsert: ctrlIdx >= 16 → invalidIndex, nothing plugged");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = upsert(svc, {makeDesc(16)});
    EXPECT_EQ((int)r.controllers[0].result, (int)APPLY_ERR_INVALID_INDEX);
    EXPECT_EQ(vigem.pluginCalls + vigem.pluginDS4Calls, 0);
}

static void test_upsert_backendUnavailable() {
    TEST("upsert: bus won't open → backendUnavailable per controller, session still ok");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    vigem.ensureBusReturnVal = false;
    SessionService svc(vigem, client, log);

    auto r = upsert(svc, {makeDesc(0)});
    EXPECT(r.ok);
    EXPECT_EQ((int)r.controllers[0].result, (int)APPLY_ERR_BACKEND_UNAVAIL);
    EXPECT(svc.isDeviceConnected("dev1"));
}

static void test_upsert_pluginFailReleasesSerial() {
    TEST("upsert: plug failure → pluginFailed, serial released");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    vigem.pluginReturnVal = false;
    SessionService svc(vigem, client, log);

    auto r = upsert(svc, {makeDesc(0)});
    EXPECT_EQ((int)r.controllers[0].result, (int)APPLY_ERR_PLUGIN_FAIL);
    EXPECT_EQ(svc.availableSlots(), 16);
}

static void test_upsert_noSlots() {
    TEST("upsert: 17th controller across sessions → noSlots");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    for (int i = 0; i < 16; i++) {
        svc.upsertSession("dev" + std::to_string(i), "D", "1.1.1.1", TEST_KEY, {makeDesc(0)},
                          false);
    }
    EXPECT_EQ(svc.availableSlots(), 0);
    auto r = svc.upsertSession("devX", "DX", "1.1.1.1", TEST_KEY, {makeDesc(0)}, false);
    EXPECT_EQ((int)r.controllers[0].result, (int)APPLY_ERR_NO_SLOTS);
}

static void test_upsert_partialSuccess() {
    TEST("upsert: one good + one bad descriptor: per-controller results, no abort");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = upsert(svc, {makeDesc(0), makeDesc(1, 99)});
    EXPECT(r.ok);
    EXPECT_EQ((int)r.controllers.size(), 2);
    EXPECT_EQ((int)r.controllers[0].result, (int)APPLY_OK);
    EXPECT_EQ((int)r.controllers[1].result, (int)APPLY_ERR_INVALID_TYPE);
    EXPECT_EQ(svc.totalActiveControllers(), 1);
}

static void test_converge_typeFamilyChange_transactionalReplug() {
    TEST("converge: family change plugs NEW serial first, then retires the old");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    upsert(svc, {makeDesc(0, CONTROLLER_TYPE_XBOX)});
    uint32_t oldSerial = vigem.pluggedSerials.back();

    auto r = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_PLAYSTATION)});
    EXPECT_EQ((int)r.controllers[0].result, (int)APPLY_OK);
    EXPECT_EQ((int)r.controllers[0].appliedType, (int)CONTROLLER_TYPE_PLAYSTATION);
    EXPECT_EQ(vigem.pluginDS4Calls, 1);
    EXPECT_EQ(vigem.unplugCalls, 1);
    // The new target landed on a FRESH serial; the old was retired after.
    uint32_t newSerial = 0;
    {
        auto snap = svc.getConnectionsSnapshot();
        newSerial = snap.connections[0].controllers[0].serial;
    }
    EXPECT(newSerial != oldSerial);
    EXPECT_EQ(vigem.unpluggedSerials.back(), oldSerial);
    EXPECT_EQ(vigem.pluggedSerials.back(), newSerial);
}

static void test_converge_replugFailure_keepsOldPad() {
    TEST("converge: failed replug leaves the old pad untouched and reports replugFailed");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    upsert(svc, {makeDesc(0, CONTROLLER_TYPE_XBOX)});
    uint32_t oldSerial = vigem.pluggedSerials.back();

    vigem.pluginReturnVal = false;
    auto r = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_PLAYSTATION)});
    EXPECT_EQ((int)r.controllers[0].result, (int)APPLY_ERR_REPLUG_FAIL);
    EXPECT_EQ((int)r.controllers[0].appliedType, (int)CONTROLLER_TYPE_XBOX);
    EXPECT_EQ(vigem.unplugCalls, 0); // old pad untouched
    EXPECT_EQ(svc.totalActiveControllers(), 1);
    auto snap = svc.getConnectionsSnapshot();
    EXPECT_EQ((int)snap.connections[0].controllers[0].controllerType, (int)CONTROLLER_TYPE_XBOX);
    EXPECT_EQ(snap.connections[0].controllers[0].serial, oldSerial);
    EXPECT(log.contains("Failed to replug"));
    // Retrying the switch is NOT a silent no-op: when the driver recovers,
    // the next converge succeeds.
    vigem.pluginReturnVal = true;
    auto r2 = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_PLAYSTATION)});
    EXPECT_EQ((int)r2.controllers[0].result, (int)APPLY_OK);
    EXPECT_EQ((int)r2.controllers[0].appliedType, (int)CONTROLLER_TYPE_PLAYSTATION);
}

static void test_converge_fullBus_fallsBackToUnplugFirst() {
    TEST("converge: 16/16 serials: family change falls back to unplug-first, same serial");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    std::vector<ControllerDescriptor> sixteen;
    for (int i = 0; i < 16; i++) sixteen.push_back(makeDesc((uint8_t)i, CONTROLLER_TYPE_XBOX));
    upsert(svc, sixteen);
    EXPECT_EQ(svc.availableSlots(), 0);
    uint32_t serial0 = 0;
    {
        auto snap = svc.getConnectionsSnapshot();
        for (auto& c : snap.connections[0].controllers) {
            if (c.index == 0) serial0 = c.serial;
        }
    }

    sixteen[0] = makeDesc(0, CONTROLLER_TYPE_PLAYSTATION);
    auto r = upsert(svc, sixteen);
    EXPECT_EQ((int)r.controllers[0].result, (int)APPLY_OK);
    EXPECT_EQ(vigem.unpluggedSerials.back(), serial0);
    EXPECT_EQ(vigem.pluggedSerials.back(), serial0); // same serial reused
    EXPECT_EQ(svc.totalActiveControllers(), 16);
}

static void test_converge_capsAndModeChange_noReplug() {
    TEST("converge: caps/touchpadMode change, same family → no replug, no epoch bump");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r1 =
        upsert(svc, {makeDesc(0, CONTROLLER_TYPE_PLAYSTATION, CAP_RUMBLE, TOUCHPAD_MODE_OFF)});
    int plugsAfterFirst = vigem.pluginDS4Calls;
    auto r2 = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_PLAYSTATION,
                                    CAP_RUMBLE | CAP_MOTION | CAP_LIGHTBAR, TOUCHPAD_MODE_DS4)});
    EXPECT_EQ(vigem.pluginDS4Calls, plugsAfterFirst); // no replug
    EXPECT_EQ(vigem.unplugCalls, 0);
    EXPECT_EQ((int)r2.controllers[0].result, (int)APPLY_OK);
    EXPECT_EQ(r2.epoch, r1.epoch);
    auto view = svc.getSessionView(r2.connectionId, "dev1");
    EXPECT_EQ((int)view.controllers[0].touchpadMode, (int)TOUCHPAD_MODE_DS4);
    EXPECT_EQ((int)view.controllers[0].caps, (int)(CAP_RUMBLE | CAP_MOTION | CAP_LIGHTBAR));
}

static void test_converge_sameDescriptorIsNoOp() {
    TEST("converge: re-PUT of the identical descriptor set: no churn, same epoch");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r1 = upsert(svc, {makeDesc(0), makeDesc(1, CONTROLLER_TYPE_PLAYSTATION)});
    int plugs = vigem.pluginCalls + vigem.pluginDS4Calls;
    auto r2 = upsert(svc, {makeDesc(0), makeDesc(1, CONTROLLER_TYPE_PLAYSTATION)});
    EXPECT_EQ(vigem.pluginCalls + vigem.pluginDS4Calls, plugs);
    EXPECT_EQ(vigem.unplugCalls, 0);
    EXPECT_EQ(r2.epoch, r1.epoch);
}

// N-way controller-type coverage: the identity model plugs each family under
// its own materialization identity and gates motion/touchpad on the type.

static void test_upsert_dualSensePlugsWithDualSenseIdentity() {
    TEST("upsert: DualSense type plugs the DualSense identity; motion sink supported");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_DUALSENSE, CAP_MOTION)});
    EXPECT_EQ((int)r.controllers.size(), 1);
    EXPECT_EQ((int)r.controllers[0].result, (int)APPLY_OK);
    EXPECT_EQ((int)r.controllers[0].appliedType, (int)CONTROLLER_TYPE_DUALSENSE);
    uint32_t serial = vigem.pluggedSerials.back();
    EXPECT(vigem.identityFor(serial) == GamepadIdentity::DualSense);
    EXPECT(vigem.lastIdentity == GamepadIdentity::DualSense);
    EXPECT(r.controllers[0].motionSinkSupportedForType);
}

static void test_upsert_switchProPlugsMotionNoTouchpad() {
    TEST("upsert: Switch Pro plugs the SwitchPro identity; motion supported, touchpad not landed");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_SWITCHPRO, CAP_MOTION, TOUCHPAD_MODE_DS4)});
    EXPECT_EQ((int)r.controllers[0].result, (int)APPLY_OK);
    EXPECT_EQ((int)r.controllers[0].appliedType, (int)CONTROLLER_TYPE_SWITCHPRO);
    uint32_t serial = vigem.pluggedSerials.back();
    EXPECT(vigem.identityFor(serial) == GamepadIdentity::SwitchPro);
    EXPECT(r.controllers[0].motionSinkSupportedForType); // Switch Pro has an IMU
    EXPECT(!controllerTypeHasTouchpad(CONTROLLER_TYPE_SWITCHPRO));

    // A DS4-mode touchpad sample is offered to the backend but the SwitchPro
    // identity has no touchpad surface, so it isn't landed (still cached).
    TouchpadReport tr{};
    tr.finger0.active = true;
    EXPECT(!svc.handleTouchpadData(r.token, 0, tr));
    auto snap = svc.getConnectionsSnapshot();
    EXPECT(snap.connections[0].controllers[0].touchpadActive);
}

static void test_converge_identityChangeReplugs_capsOnlyDoesNot() {
    TEST("converge: DS4→DualSense replugs + bumps epoch; DS4 caps-only change does not");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    // Live DS4 (PlayStation) pad.
    auto r1 = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_PLAYSTATION, CAP_RUMBLE)});
    uint32_t ds4Serial = vigem.pluggedSerials.back();
    EXPECT(vigem.identityFor(ds4Serial) == GamepadIdentity::DS4);
    size_t plugsAfterFirst = vigem.pluggedSerials.size();

    // Caps-only change within the DS4 identity: converge in place, no replug/bump.
    auto r2 = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_PLAYSTATION, CAP_RUMBLE | CAP_MOTION)});
    EXPECT_EQ(vigem.pluggedSerials.size(), plugsAfterFirst);
    EXPECT_EQ(vigem.unplugCalls, 0);
    EXPECT_EQ(r2.epoch, r1.epoch);

    // DS4 → DualSense: identity changes → transactional replug onto a FRESH
    // serial + epoch bump; the old DS4 target is retired after.
    auto r3 = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_DUALSENSE, CAP_RUMBLE | CAP_MOTION)});
    EXPECT_EQ((int)r3.controllers[0].result, (int)APPLY_OK);
    EXPECT_EQ((int)r3.controllers[0].appliedType, (int)CONTROLLER_TYPE_DUALSENSE);
    EXPECT(r3.epoch != r2.epoch);
    EXPECT_EQ(vigem.pluggedSerials.size(), plugsAfterFirst + 1);
    uint32_t dualSenseSerial = vigem.pluggedSerials.back();
    EXPECT(dualSenseSerial != ds4Serial);
    EXPECT(vigem.identityFor(dualSenseSerial) == GamepadIdentity::DualSense);
    EXPECT_EQ(vigem.unplugCalls, 1);
    EXPECT_EQ(vigem.unpluggedSerials.back(), ds4Serial);
}

static void test_apply_unsupportedIdentityRejectedPriorPadIntact() {
    TEST("apply: backend that can't materialize DualSense → invalidType, prior pad intact");
    MockViGem vigem;
    vigem.unsupportedIdentities.insert(GamepadIdentity::DualSense);
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    // A live DS4 pad (the DS4 identity IS supported).
    auto r1 = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_PLAYSTATION)});
    EXPECT_EQ((int)r1.controllers[0].result, (int)APPLY_OK);
    uint32_t ds4Serial = vigem.pluggedSerials.back();
    size_t plugsBefore = vigem.pluggedSerials.size();
    int unplugsBefore = vigem.unplugCalls;

    // Asking for DualSense on this backend fails per-controller and leaves the
    // existing pad untouched (no plug, no unplug, type/serial unchanged).
    auto r2 = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_DUALSENSE)});
    EXPECT_EQ((int)r2.controllers[0].result, (int)APPLY_ERR_INVALID_TYPE);
    EXPECT_EQ((int)r2.controllers[0].appliedType, (int)CONTROLLER_TYPE_PLAYSTATION);
    EXPECT_EQ(vigem.pluggedSerials.size(), plugsBefore);
    EXPECT_EQ(vigem.unplugCalls, unplugsBefore);
    EXPECT_EQ(svc.totalActiveControllers(), 1);
    auto snap = svc.getConnectionsSnapshot();
    EXPECT_EQ((int)snap.connections[0].controllers[0].controllerType,
              (int)CONTROLLER_TYPE_PLAYSTATION);
    EXPECT_EQ(snap.connections[0].controllers[0].serial, ds4Serial);
    EXPECT(vigem.identityFor(ds4Serial) == GamepadIdentity::DS4);
}

static void test_epoch_bumpsOnTopologyChanges() {
    TEST("epoch: bumps on add/remove/replug, not on no-ops");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r1 = upsert(svc, {makeDesc(0)});
    auto r2 = upsert(svc, {makeDesc(0), makeDesc(1)}); // add → bump
    EXPECT(r2.epoch != r1.epoch);
    auto r3 = upsert(svc, {makeDesc(0)}); // remove → bump
    EXPECT(r3.epoch != r2.epoch);
    auto r4 = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_PLAYSTATION)}); // replug → bump
    EXPECT(r4.epoch != r3.epoch);
    auto r5 = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_PLAYSTATION)}); // identical → no bump
    EXPECT_EQ(r5.epoch, r4.epoch);
}

static void test_epoch_bumpsOnFailedReplug() {
    TEST("epoch: failed replug bumps so a lost PUT response still reconciles");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r1 = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_XBOX)});
    vigem.pluginReturnVal = false;
    auto r2 = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_PLAYSTATION)});
    EXPECT(r2.epoch != r1.epoch);
}

static void test_heartbeat_enrichedAck() {
    TEST("heartbeat: ack carries backend status + epoch + active bitmap");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = upsert(svc, {makeDesc(0), makeDesc(2)});
    svc.handleHeartbeat(r.token);
    EXPECT_EQ(client.heartbeatAckCalls, 1);
    EXPECT(client.lastAckBackendAvailable);
    EXPECT_EQ((int)client.lastAckTotalControllers, 2);
    EXPECT_EQ(client.lastAckEpoch, r.epoch);
    EXPECT_EQ(client.lastAckBitmap, (uint16_t)0b101);
}

static void test_heartbeat_invalidTokenIsNoOp() {
    TEST("heartbeat: invalid token is a no-op");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    svc.handleHeartbeat(99999);
    EXPECT_EQ(client.heartbeatAckCalls, 0);
}

static void test_heartbeat_reflectsInvoluntaryLoss() {
    TEST("heartbeat: slot removed server-side shows up as epoch+bitmap change");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = upsert(svc, {makeDesc(0), makeDesc(1)});
    svc.handleHeartbeat(r.token);
    uint16_t epochBefore = client.lastAckEpoch;
    uint16_t bitmapBefore = client.lastAckBitmap;

    // Standalone DELETE of slot 1 stands in for any server-side loss.
    uint16_t epochOut = 0;
    svc.removeController(r.connectionId, "dev1", 1, epochOut);
    svc.handleHeartbeat(r.token);
    EXPECT(client.lastAckEpoch != epochBefore);
    EXPECT(client.lastAckBitmap != bitmapBefore);
    EXPECT_EQ(client.lastAckBitmap, (uint16_t)0b1);
}

static void test_applyController_standalone() {
    TEST("applyController: standalone descriptor upsert on a live session");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = upsert(svc);
    ControllerApplyResult ar;
    uint16_t epoch = 0;
    EXPECT(svc.applyController(r.connectionId, "dev1", makeDesc(3, CONTROLLER_TYPE_PLAYSTATION), ar,
                               epoch));
    EXPECT_EQ((int)ar.result, (int)APPLY_OK);
    EXPECT_EQ(svc.totalActiveControllers(), 1);
    EXPECT(epoch != r.epoch);
}

static void test_applyController_scopedToOwner() {
    TEST("applyController: another device's connectionId is not found");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = upsert(svc);
    ControllerApplyResult ar;
    uint16_t epoch = 0;
    EXPECT(!svc.applyController(r.connectionId, "devEvil", makeDesc(0), ar, epoch));
    EXPECT(!svc.applyController("conn_ffffffff", "dev1", makeDesc(0), ar, epoch));
}

static void test_removeController_slotOnly() {
    TEST("removeController: removes the SLOT; the session lives on");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = upsert(svc, {makeDesc(0)});
    uint16_t epoch = 0;
    EXPECT(svc.removeController(r.connectionId, "dev1", 0, epoch));
    EXPECT_EQ(vigem.unplugCalls, 1);
    EXPECT_EQ(svc.totalActiveControllers(), 0);
    EXPECT(svc.isDeviceConnected("dev1"));
    EXPECT(epoch != r.epoch);
    // Idempotent: removing the now-empty slot still succeeds, epoch unchanged.
    uint16_t epoch2 = 0;
    EXPECT(svc.removeController(r.connectionId, "dev1", 0, epoch2));
    EXPECT_EQ(epoch2, epoch);
    EXPECT(!svc.removeController(r.connectionId, "devEvil", 0, epoch2));
}

static void test_sessionView_scopedAndComplete() {
    TEST("getSessionView: applied state, scoped to the owning device");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r =
        upsert(svc, {makeDesc(1, CONTROLLER_TYPE_PLAYSTATION, CAP_MOTION, TOUCHPAD_MODE_MOUSE)},
               "dev1", "TestDevice", true);
    auto view = svc.getSessionView(r.connectionId, "dev1");
    EXPECT(view.found);
    EXPECT_EQ(view.connectionId, r.connectionId);
    EXPECT_EQ(view.epoch, r.epoch);
    EXPECT(view.mouseControlGranted);
    EXPECT_EQ((int)view.controllers.size(), 1);
    EXPECT_EQ((int)view.controllers[0].ctrlIdx, 1);
    EXPECT_EQ((int)view.controllers[0].appliedType, (int)CONTROLLER_TYPE_PLAYSTATION);
    EXPECT(view.controllers[0].motionSinkSupportedForType);

    EXPECT(!svc.getSessionView(r.connectionId, "devEvil").found);
    EXPECT(!svc.getSessionView("conn_nope", "dev1").found);
    // Admin scope (empty deviceId) sees any session.
    EXPECT(svc.getSessionView(r.connectionId, "").found);
}

static void test_mouseControl_grantedWhenSupported() {
    TEST("hostFeatures: mouseControl granted when requested and supported");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = upsert(svc, {}, "dev1", "D", true);
    EXPECT(r.mouseControlGranted);
    EXPECT(r.mouseControlDenyReason.empty());
}

static void test_mouseControl_deniedWhenUnsupported() {
    TEST("hostFeatures: mouseControl denied with notSupported on an inert backend");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    vigem.supportsRelativeMouseVal = false;
    SessionService svc(vigem, client, log);

    auto r = upsert(svc, {}, "dev1", "D", true);
    EXPECT(!r.mouseControlGranted);
    EXPECT_EQ(r.mouseControlDenyReason, std::string(HOST_FEATURE_DENY_NOT_SUPPORTED));
}

static void test_mouseControl_notRequestedNotGranted() {
    TEST("hostFeatures: mouseControl not requested → not granted, no reason");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = upsert(svc, {}, "dev1", "D", false);
    EXPECT(!r.mouseControlGranted);
    EXPECT(r.mouseControlDenyReason.empty());
}

static void test_touchpad_mouseStreamDroppedWithoutGrant() {
    TEST("hostFeatures: mouse-mode touchpad stream is dropped without the grant");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_XBOX, 0, TOUCHPAD_MODE_MOUSE)}, "dev1", "D",
                    /*mouseControl=*/false);
    TouchpadReport tr{};
    tr.finger0.active = true;
    tr.finger0.x = 100;
    tr.finger0.y = 100;
    tr.eventTimeMs = 1000;
    EXPECT(!svc.handleTouchpadData(r.token, 0, tr));
    EXPECT_EQ(vigem.submitRelativeMouseCalls, 0);
    // Still cached for the debug pane.
    auto snap = svc.getConnectionsSnapshot();
    EXPECT(snap.connections[0].controllers[0].touchpadActive);
}

static void test_mouseControl_rePutWithoutRequestRevokes() {
    TEST("hostFeatures: grants are per-PUT: a re-PUT without the request revokes");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r1 = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_XBOX, 0, TOUCHPAD_MODE_MOUSE)}, "dev1", "D",
                     /*mouseControl=*/true);
    EXPECT(r1.mouseControlGranted);
    TouchpadReport tr{};
    tr.finger0.active = true;
    tr.eventTimeMs = 1000;
    EXPECT(svc.handleTouchpadData(r1.token, 0, tr));
    EXPECT_EQ(vigem.submitRelativeMouseCalls, 1);

    auto r2 = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_XBOX, 0, TOUCHPAD_MODE_MOUSE)}, "dev1", "D",
                     /*mouseControl=*/false);
    EXPECT(!r2.mouseControlGranted);
    EXPECT(!svc.getSessionView(r2.connectionId, "dev1").mouseControlGranted);
    tr.eventTimeMs = 1004;
    EXPECT(!svc.handleTouchpadData(r2.token, 0, tr)); // dropped, not routed
    EXPECT_EQ(vigem.submitRelativeMouseCalls, 1);
}

static void test_serials_roundRobinAvoidsInstantReuse() {
    TEST("serials: a just-freed serial is not the next one allocated");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    upsert(svc, {makeDesc(0)});
    uint32_t first = vigem.pluggedSerials.back();
    // Free it, then plug a fresh slot: the scan starts past the freed serial.
    upsert(svc, {});
    upsert(svc, {makeDesc(1)});
    uint32_t second = vigem.pluggedSerials.back();
    EXPECT(second != first);
}

static void test_serials_quarantineOnUnplugFailure() {
    TEST("serials: unconfirmed unplug quarantines the serial until the bus closes");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    upsert(svc, {makeDesc(0)});
    EXPECT_EQ(svc.availableSlots(), 15);

    vigem.unplugReturnVal = false;
    // Keep a second pad alive so the bus doesn't idle-close (which would
    // legitimately clear the quarantine).
    upsert(svc, {makeDesc(1)}); // slot 0 removed (quarantined), slot 1 plugged
    EXPECT(log.contains("quarantined"));
    // 16 - 1 in use - 1 quarantined = 14.
    EXPECT_EQ(svc.availableSlots(), 14);

    // Quarantined serials are never reallocated: 14 free + the live pad fills
    // exactly 15 slots with zero noSlots failures.
    vigem.unplugReturnVal = true;
    std::vector<ControllerDescriptor> many;
    for (int i = 1; i <= 15; i++) many.push_back(makeDesc((uint8_t)i));
    auto r = upsert(svc, many);
    int noSlots = 0;
    for (auto& c : r.controllers) {
        if (c.result == APPLY_ERR_NO_SLOTS) noSlots++;
    }
    EXPECT_EQ(noSlots, 0);
    EXPECT_EQ(svc.availableSlots(), 0);

    // Closing the bus (idle) clears the quarantine.
    upsert(svc, {});
    EXPECT(!vigem.busOpen);
    EXPECT_EQ(svc.availableSlots(), 16);
}

static void test_closeById_kick_notifiesBeforeTeardown() {
    TEST("closeSessionById: kick sends close-notify BEFORE any unplug");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = upsert(svc, {makeDesc(0)});
    bool notifiedBeforeUnplug = false;
    client.onSessionClose = [&] { notifiedBeforeUnplug = (vigem.unplugCalls == 0); };
    int removed = svc.closeSessionById(r.connectionId, "", CLOSE_REASON_KICKED, /*notify=*/true);
    EXPECT_EQ(removed, 1);
    EXPECT(notifiedBeforeUnplug);
    EXPECT_EQ((int)client.closeNotifies.back().second, (int)CLOSE_REASON_KICKED);
    EXPECT(!svc.isDeviceConnected("dev1"));
}

static void test_closeById_clientCloseNoNotify() {
    TEST("closeSessionById: client's own close sends no notify");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = upsert(svc, {makeDesc(0)});
    int removed = svc.closeSessionById(r.connectionId, "dev1", CLOSE_REASON_REPLACED,
                                       /*notify=*/false);
    EXPECT_EQ(removed, 1);
    EXPECT_EQ(client.sessionCloseCalls, 0);
}

static void test_closeById_scopeAndNotFound() {
    TEST("closeSessionById: wrong owner or unknown id → -1");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = upsert(svc);
    EXPECT_EQ(svc.closeSessionById(r.connectionId, "devEvil", CLOSE_REASON_KICKED, true), -1);
    EXPECT_EQ(svc.closeSessionById("conn_nope", "", CLOSE_REASON_KICKED, true), -1);
    EXPECT(svc.isDeviceConnected("dev1"));
}

static void test_closeForDevice_unpairClosesLiveSession() {
    TEST("closeSessionsForDevice: unpair closes the live session with reason=unpaired");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    upsert(svc, {makeDesc(0)});
    bool notifiedBeforeUnplug = false;
    client.onSessionClose = [&] { notifiedBeforeUnplug = (vigem.unplugCalls == 0); };
    int closed = svc.closeSessionsForDevice("dev1", CLOSE_REASON_UNPAIRED);
    EXPECT_EQ(closed, 1);
    EXPECT(notifiedBeforeUnplug);
    EXPECT_EQ((int)client.closeNotifies.back().second, (int)CLOSE_REASON_UNPAIRED);
    EXPECT_EQ(vigem.unplugCalls, 1);
    EXPECT(!svc.isDeviceConnected("dev1"));
    EXPECT_EQ(svc.closeSessionsForDevice("dev1", CLOSE_REASON_UNPAIRED), 0);
}

static void test_closeAll_broadcastsShutdownFirst() {
    TEST("closeAllSessions: broadcasts close-notify(shutdown) before teardown");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    svc.upsertSession("d1", "D1", "1.1.1.1", TEST_KEY, {makeDesc(0)}, false);
    svc.upsertSession("d2", "D2", "1.1.1.2", TEST_KEY, {makeDesc(0)}, false);
    int unplugsWhenLastNotifyFired = -1;
    client.onSessionClose = [&] { unplugsWhenLastNotifyFired = vigem.unplugCalls; };
    svc.closeAllSessions();
    EXPECT_EQ(client.sessionCloseCalls, 2);
    EXPECT_EQ(unplugsWhenLastNotifyFired, 0); // every notify preceded every unplug
    for (auto& n : client.closeNotifies) { EXPECT_EQ((int)n.second, (int)CLOSE_REASON_SHUTDOWN); }
    EXPECT_EQ(svc.totalActiveControllers(), 0);
    EXPECT_EQ(svc.availableSlots(), 16);
    auto snap = svc.getConnectionsSnapshot();
    EXPECT_EQ((int)snap.connections.size(), 0);
}

static void test_reap_honoursRestGraceWindow() {
    TEST("reap: a stale-but-in-grace session is NOT reaped (half-open protection)");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = upsert(svc, {makeDesc(0)});
    // No packets ever arrived; lastPacketTime is way past the reap timeout,
    // but the REST-open grace is still in the future.
    svc.backdateForTest(r.token, /*lastPacketSecondsAgo=*/60, /*graceSecondsAgo=*/-60);
    EXPECT_EQ(svc.reapTimedOut(), 0);
    EXPECT(svc.isDeviceConnected("dev1"));
}

static void test_reap_firesAfterGraceExpires() {
    TEST("reap: grace lapsed + no packets → reaped, pads unplugged");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = upsert(svc, {makeDesc(0)});
    svc.backdateForTest(r.token, 60, 1);
    EXPECT_EQ(svc.reapTimedOut(), 1);
    EXPECT(!svc.isDeviceConnected("dev1"));
    EXPECT_EQ(vigem.unplugCalls, 1);
    EXPECT_EQ(svc.availableSlots(), 16);
    EXPECT(log.contains("Reaper"));
}

static void test_reap_recentPacketsKeepAlive() {
    TEST("reap: fresh packets keep the session alive after grace");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = upsert(svc, {makeDesc(0)});
    svc.backdateForTest(r.token, 60, 1);
    svc.updatePostDecryptV4(r.token, 1, 0x0100007f, 5555); // a packet lands
    EXPECT_EQ(svc.reapTimedOut(), 0);
    EXPECT(svc.isDeviceConnected("dev1"));
}

static void test_linkState_graceCountsAsActive() {
    TEST("linkState: in-grace session reads Active, not NotResponding");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = upsert(svc);
    EXPECT(svc.linkStateForDevice("dev1") == DeviceLinkState::Active);
    // Stale but still in grace.
    svc.backdateForTest(r.token, 60, -60);
    EXPECT(svc.linkStateForDevice("dev1") == DeviceLinkState::Active);
    // Stale and out of grace.
    svc.backdateForTest(r.token, 60, 1);
    EXPECT(svc.linkStateForDevice("dev1") == DeviceLinkState::NotResponding);
    EXPECT(svc.linkStateForDevice("devUnknown") == DeviceLinkState::Paired);
}

static void test_snapshot_notRespondingState() {
    TEST("snapshot: stale post-grace connection tagged notResponding");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = upsert(svc, {makeDesc(0)});
    svc.backdateForTest(r.token, 60, 1);
    auto snap = svc.getConnectionsSnapshot();
    EXPECT(snap.connections[0].linkState == DeviceLinkState::NotResponding);
}

static void test_snapshot_pluggedInFromAdapterTruth() {
    TEST("snapshot: pluggedIn reflects the adapter, not serialNo > 0");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    upsert(svc, {makeDesc(0)});
    auto snap1 = svc.getConnectionsSnapshot();
    EXPECT(snap1.connections[0].controllers[0].pluggedIn);
    EXPECT(snap1.connections[0].controllers[0].serial > 0);

    // The target vanishes driver-side (zombie) while the serial stays held:
    // the dashboard must now say unplugged.
    vigem.pluggedSet.clear();
    auto snap2 = svc.getConnectionsSnapshot();
    EXPECT(!snap2.connections[0].controllers[0].pluggedIn);
    EXPECT(snap2.connections[0].controllers[0].serial > 0);
}

static void test_snapshot_carriesIdentityAndEpoch() {
    TEST("snapshot: carries connectionId, epoch, grants, per-controller mode");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_PLAYSTATION, 0, TOUCHPAD_MODE_DS4)}, "dev1",
                    "TestDevice", true);
    auto snap = svc.getConnectionsSnapshot();
    EXPECT_EQ(snap.connections[0].connectionId, r.connectionId);
    EXPECT_EQ(snap.connections[0].epoch, r.epoch);
    EXPECT(snap.connections[0].mouseControlGranted);
    EXPECT_EQ((int)snap.connections[0].controllers[0].touchpadMode, (int)TOUCHPAD_MODE_DS4);
    EXPECT_EQ(snap.connections[0].deviceName, std::string("TestDevice"));
}

// Data streams (unchanged plane, regression coverage).

static void test_gamepadData_routesByType() {
    TEST("gamepadData: XUSB for Xbox slots, DS4 for PlayStation slots");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r =
        upsert(svc, {makeDesc(0, CONTROLLER_TYPE_XBOX), makeDesc(1, CONTROLLER_TYPE_PLAYSTATION)});
    GamepadReport rpt{};
    rpt.wButtons = 0x1234;
    EXPECT(svc.handleGamepadData(r.token, 0, rpt));
    EXPECT_EQ(vigem.submitCalls, 1);
    EXPECT_EQ(vigem.submitDS4Calls, 0);
    EXPECT(svc.handleGamepadData(r.token, 1, rpt));
    EXPECT_EQ(vigem.submitDS4Calls, 1);
    EXPECT_EQ(vigem.lastSubmittedReport.wButtons, (uint16_t)0x1234);
}

static void test_gamepadData_guards() {
    TEST("gamepadData: invalid token / inactive slot / out-of-range idx");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = upsert(svc);
    GamepadReport rpt{};
    EXPECT(!svc.handleGamepadData(99999, 0, rpt));
    EXPECT(!svc.handleGamepadData(r.token, 0, rpt));
    EXPECT(!svc.handleGamepadData(r.token, 20, rpt));
}

static void test_fusedHotPath() {
    TEST("handleGamepadDataAndUpdate: fused path updates counter+addr and submits");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = upsert(svc, {makeDesc(0)});
    GamepadReport rpt{};
    rpt.sThumbLX = 7777;
    EXPECT(svc.handleGamepadDataAndUpdate(r.token, 5, 0x0100007f, 4242, 0, rpt));
    EXPECT_EQ(client.updateAddrV4Calls, 1);
    EXPECT_EQ(client.lastV4Token, r.token);
    EXPECT_EQ(client.lastV4Port, (uint16_t)4242);
    uint8_t key[CRYPTO_KEY_SIZE];
    uint32_t ctr;
    svc.getDecryptInfo(r.token, key, ctr);
    EXPECT_EQ(ctr, (uint32_t)5);
    EXPECT_EQ(vigem.lastSubmittedReport.sThumbLX, (int16_t)7777);
}

static void test_fusedHotPath_guards() {
    TEST("handleGamepadDataAndUpdate: guards mirror the unfused path");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = upsert(svc);
    GamepadReport rpt{};
    EXPECT(!svc.handleGamepadDataAndUpdate(99999, 1, 0x0100007f, 1, 0, rpt));
    // Inactive controller: the packet still refreshes counter/addr (it
    // authenticated), only the submit is refused.
    EXPECT(!svc.handleGamepadDataAndUpdate(r.token, 7, 0x0100007f, 1, 0, rpt));
    EXPECT_EQ(client.updateAddrV4Calls, 1);
    uint8_t key[CRYPTO_KEY_SIZE];
    uint32_t ctr;
    svc.getDecryptInfo(r.token, key, ctr);
    EXPECT_EQ(ctr, (uint32_t)7);
    EXPECT(!svc.handleGamepadDataAndUpdate(r.token, 8, 0x0100007f, 1, 20, rpt));
}

static void test_motionData_cachedAndForwarded() {
    TEST("motion: cached for UI and forwarded to the backend sink");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_PLAYSTATION, CAP_MOTION)});
    MotionReport m{};
    m.gyroX = 123;
    EXPECT(svc.handleMotionData(r.token, 0, m));
    EXPECT_EQ(vigem.submitMotionCalls, 1);
    EXPECT_EQ(vigem.lastMotion.gyroX, (int16_t)123);
    auto snap = svc.getConnectionsSnapshot();
    EXPECT(snap.connections[0].controllers[0].motionActive);
    EXPECT(snap.connections[0].controllers[0].motionSink);
}

static void test_motionData_acceptedWithoutCapAndCachedOnDecline() {
    TEST("motion: best-effort: accepted without CAP_MOTION, cached when backend declines");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = upsert(svc, {makeDesc(0)}); // no CAP_MOTION advertised
    vigem.submitMotionReturnVal = false;
    MotionReport m{};
    EXPECT(!svc.handleMotionData(r.token, 0, m)); // false = not delivered, not an error
    EXPECT_EQ(vigem.submitMotionCalls, 1);        // still offered to the backend
    auto snap = svc.getConnectionsSnapshot();
    EXPECT(snap.connections[0].controllers[0].motionActive);
    EXPECT(!snap.connections[0].controllers[0].motionSink);
}

static void test_battery_validation() {
    TEST("battery: valid levels cached, malformed rejected");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = upsert(svc, {makeDesc(0)});
    BatteryReport b{};
    b.level = 80;
    b.status = BATTERY_STATUS_CHARGING;
    EXPECT(svc.handleBatteryUpdate(r.token, 0, b));
    EXPECT_EQ(vigem.submitBatteryCalls, 1);

    b.level = 150; // 101..254 is malformed
    EXPECT(!svc.handleBatteryUpdate(r.token, 0, b));
    b.level = BATTERY_LEVEL_UNKNOWN;
    b.status = BATTERY_STATUS_COUNT; // out of range
    EXPECT(!svc.handleBatteryUpdate(r.token, 0, b));
    b.status = BATTERY_STATUS_FULL;
    EXPECT(svc.handleBatteryUpdate(r.token, 0, b)); // unknown level sentinel is valid

    auto snap = svc.getConnectionsSnapshot();
    EXPECT(snap.connections[0].controllers[0].batteryKnown);
}

static void test_touchpad_ds4ModeRoutesToPad() {
    TEST("touchpad: DS4 mode routes to submitTouchpad");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_PLAYSTATION, 0, TOUCHPAD_MODE_DS4)});
    TouchpadReport tr{};
    tr.finger0.active = true;
    EXPECT(svc.handleTouchpadData(r.token, 0, tr));
    EXPECT_EQ(vigem.submitTouchpadCalls, 1);
}

static void test_touchpad_offModeCachesOnly() {
    TEST("touchpad: OFF mode caches but routes nowhere");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_XBOX, 0, TOUCHPAD_MODE_OFF)});
    TouchpadReport tr{};
    tr.finger0.active = true;
    EXPECT(!svc.handleTouchpadData(r.token, 0, tr));
    EXPECT_EQ(vigem.submitTouchpadCalls, 0);
    EXPECT_EQ(vigem.submitRelativeMouseCalls, 0);
    auto snap = svc.getConnectionsSnapshot();
    EXPECT(snap.connections[0].controllers[0].touchpadActive);
}

static TouchpadReport mkTouch(bool f0, uint8_t id, int16_t x, int16_t y, uint32_t timeMs,
                              bool button = false) {
    TouchpadReport tr{};
    tr.finger0.active = f0;
    tr.finger0.trackingId = id;
    tr.finger0.x = x;
    tr.finger0.y = y;
    tr.buttonPressed = button;
    tr.eventTimeMs = timeMs;
    return tr;
}

static void test_touchpad_mouseMode_deltaAndAnchoring() {
    TEST("touchpad: mouse mode: continuous delta, re-anchor on new contact");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_XBOX, 0, TOUCHPAD_MODE_MOUSE)}, "dev1", "D",
                    /*mouseControl=*/true);

    // First sample: no previous frame → no motion, but injected (anchored).
    EXPECT(svc.handleTouchpadData(r.token, 0, mkTouch(true, 1, 0, 0, 1000)));
    EXPECT_EQ(vigem.lastMouseDx, 0);
    EXPECT_EQ(vigem.lastMouseDy, 0);

    // Second sample, same trackingId, dt at the reference spacing → motion.
    EXPECT(svc.handleTouchpadData(r.token, 0,
                                  mkTouch(true, 1, 1000, 500, 1000 + TOUCHPAD_MOUSE_REFERENCE_MS)));
    EXPECT(vigem.lastMouseDx > 0);
    EXPECT(vigem.lastMouseDy > 0);

    // New trackingId (finger lifted, survivor compacted) → re-anchor, no jump.
    EXPECT(svc.handleTouchpadData(r.token, 0, mkTouch(true, 2, -5000, -5000, 1012)));
    EXPECT_EQ(vigem.lastMouseDx, 0);
    EXPECT_EQ(vigem.lastMouseDy, 0);
}

static void test_touchpad_mouseMode_dtScaling() {
    TEST("touchpad: mouse mode: doubled dt halves the emitted delta");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r =
        upsert(svc, {makeDesc(0, CONTROLLER_TYPE_XBOX, 0, TOUCHPAD_MODE_MOUSE)}, "dev1", "D", true);
    svc.handleTouchpadData(r.token, 0, mkTouch(true, 1, 0, 0, 1000));
    svc.handleTouchpadData(r.token, 0,
                           mkTouch(true, 1, 4000, 0, 1000 + TOUCHPAD_MOUSE_REFERENCE_MS));
    int dxReference = vigem.lastMouseDx;

    // Same spatial step, twice the dt → roughly half the cursor delta.
    svc.handleTouchpadData(r.token, 0,
                           mkTouch(true, 1, 8000, 0, 1000 + 3 * TOUCHPAD_MOUSE_REFERENCE_MS));
    int dxDoubleDt = vigem.lastMouseDx;
    EXPECT(dxReference > 0);
    EXPECT(dxDoubleDt > 0);
    EXPECT(dxDoubleDt < dxReference);
}

static void test_touchpad_mouseMode_duplicateTimestampNoMotion() {
    TEST("touchpad: mouse mode: dt <= 0 (resend) emits no motion");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r =
        upsert(svc, {makeDesc(0, CONTROLLER_TYPE_XBOX, 0, TOUCHPAD_MODE_MOUSE)}, "dev1", "D", true);
    svc.handleTouchpadData(r.token, 0, mkTouch(true, 1, 0, 0, 1000));
    svc.handleTouchpadData(r.token, 0, mkTouch(true, 1, 9000, 9000, 1000)); // same eventTime
    EXPECT_EQ(vigem.lastMouseDx, 0);
    EXPECT_EQ(vigem.lastMouseDy, 0);
}

static void test_touchpad_mouseMode_gapReanchors() {
    TEST("touchpad: mouse mode: dt gap > max re-anchors instead of flinging");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r =
        upsert(svc, {makeDesc(0, CONTROLLER_TYPE_XBOX, 0, TOUCHPAD_MODE_MOUSE)}, "dev1", "D", true);
    svc.handleTouchpadData(r.token, 0, mkTouch(true, 1, 0, 0, 1000));
    svc.handleTouchpadData(r.token, 0,
                           mkTouch(true, 1, 8000, 8000, 1000 + TOUCHPAD_MOUSE_MAX_GAP_MS + 50));
    EXPECT_EQ(vigem.lastMouseDx, 0);
    EXPECT_EQ(vigem.lastMouseDy, 0);
}

static void test_touchpad_mouseMode_buttonLevel() {
    TEST("touchpad: mouse mode forwards the button level");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r =
        upsert(svc, {makeDesc(0, CONTROLLER_TYPE_XBOX, 0, TOUCHPAD_MODE_MOUSE)}, "dev1", "D", true);
    svc.handleTouchpadData(r.token, 0, mkTouch(true, 1, 0, 0, 1000, /*button=*/true));
    EXPECT(vigem.lastMouseButton);
    svc.handleTouchpadData(r.token, 0, mkTouch(true, 1, 0, 0, 1004, /*button=*/false));
    EXPECT(!vigem.lastMouseButton);
}

static void test_touchpad_mouseMode_clickWithoutTouch() {
    TEST("touchpad: mouse mode: clicky button with no finger contact still forwards");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r =
        upsert(svc, {makeDesc(0, CONTROLLER_TYPE_XBOX, 0, TOUCHPAD_MODE_MOUSE)}, "dev1", "D", true);
    EXPECT(svc.handleTouchpadData(r.token, 0, mkTouch(false, 0, 0, 0, 1000, /*button=*/true)));
    EXPECT_EQ(vigem.submitRelativeMouseCalls, 1);
    EXPECT(vigem.lastMouseButton);
    EXPECT_EQ(vigem.lastMouseDx, 0);
    EXPECT_EQ(vigem.lastMouseDy, 0);
}

static void test_touchpad_mouseMode_extendedButtonsAndWheel() {
    TEST("touchpad: mouse mode forwards right/middle levels and the wheel event");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r =
        upsert(svc, {makeDesc(0, CONTROLLER_TYPE_XBOX, 0, TOUCHPAD_MODE_MOUSE)}, "dev1", "D", true);
    TouchpadReport tr = mkTouch(false, 0, 0, 0, 1000);
    tr.rightPressed = true;
    tr.middlePressed = true;
    tr.scrollV = 240;
    EXPECT(svc.handleTouchpadData(r.token, 0, tr));
    EXPECT(!vigem.lastMouseButton);
    EXPECT(vigem.lastMouseRight);
    EXPECT(vigem.lastMouseMiddle);
    EXPECT_EQ(vigem.lastMouseWheel, 240);

    TouchpadReport up = mkTouch(false, 0, 0, 0, 1004);
    EXPECT(svc.handleTouchpadData(r.token, 0, up));
    EXPECT(!vigem.lastMouseRight);
    EXPECT(!vigem.lastMouseMiddle);
    EXPECT_EQ(vigem.lastMouseWheel, 0);
}

static void test_touchpad_mouseMode_wheelDuplicateSuppressed() {
    TEST("touchpad: mouse mode: a duplicated frame must not scroll twice");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r =
        upsert(svc, {makeDesc(0, CONTROLLER_TYPE_XBOX, 0, TOUCHPAD_MODE_MOUSE)}, "dev1", "D", true);
    TouchpadReport tr = mkTouch(false, 0, 0, 0, 1000);
    tr.scrollV = 120;
    EXPECT(svc.handleTouchpadData(r.token, 0, tr));
    EXPECT_EQ(vigem.lastMouseWheel, 120);
    // Byte-identical UDP duplicate: same eventTimeMs, same scroll. Neutralized.
    EXPECT(svc.handleTouchpadData(r.token, 0, tr));
    EXPECT_EQ(vigem.lastMouseWheel, 0);
    // A fresh frame with a newer stamp scrolls again.
    TouchpadReport next = mkTouch(false, 0, 0, 0, 1008);
    next.scrollV = -120;
    EXPECT(svc.handleTouchpadData(r.token, 0, next));
    EXPECT_EQ(vigem.lastMouseWheel, -120);
}

static void test_touchpad_mouseMode_subPixelRemainder() {
    TEST("touchpad: mouse mode: sub-pixel remainder accumulates; resends keep it; gaps drop it");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r =
        upsert(svc, {makeDesc(0, CONTROLLER_TYPE_XBOX, 0, TOUCHPAD_MODE_MOUSE)}, "dev1", "D", true);
    // 10 wire units / 4 ms ≈ 0.42 px per sample: truncation alone would never
    // move the cursor; the carried remainder must cross 1 px on the 3rd step.
    const uint32_t dt = TOUCHPAD_MOUSE_REFERENCE_MS;
    svc.handleTouchpadData(r.token, 0, mkTouch(true, 1, 0, 0, 1000)); // anchor
    svc.handleTouchpadData(r.token, 0, mkTouch(true, 1, 10, -10, 1000 + dt));
    EXPECT_EQ(vigem.lastMouseDx, 0);
    EXPECT_EQ(vigem.lastMouseDy, 0);
    svc.handleTouchpadData(r.token, 0, mkTouch(true, 1, 20, -20, 1000 + 2 * dt));
    EXPECT_EQ(vigem.lastMouseDx, 0);
    // A dt <= 0 resend emits no motion but must NOT drop the remainder…
    svc.handleTouchpadData(r.token, 0, mkTouch(true, 1, 20, -20, 1000 + 2 * dt));
    EXPECT_EQ(vigem.lastMouseDx, 0);
    // …so the next real step crosses ±1 px (0.42 × 3 ≈ 1.26).
    svc.handleTouchpadData(r.token, 0, mkTouch(true, 1, 30, -30, 1000 + 3 * dt));
    EXPECT_EQ(vigem.lastMouseDx, 1);
    EXPECT_EQ(vigem.lastMouseDy, -1);

    // A MAX_GAP re-anchor DOES drop the remainder: grow it to ~0.68, gap, then
    // step; from a clean slate the step stays sub-pixel (0.42), but a wrongly
    // kept remainder would cross 1 px (1.10).
    svc.handleTouchpadData(r.token, 0, mkTouch(true, 1, 40, -40, 1000 + 4 * dt));
    EXPECT_EQ(vigem.lastMouseDx, 0); // remainder now ≈ 0.68
    const uint32_t tGap = 1000 + 4 * dt + TOUCHPAD_MOUSE_MAX_GAP_MS + 1;
    svc.handleTouchpadData(r.token, 0, mkTouch(true, 1, 50, -50, tGap));
    EXPECT_EQ(vigem.lastMouseDx, 0); // re-anchored
    svc.handleTouchpadData(r.token, 0, mkTouch(true, 1, 60, -60, tGap + dt));
    EXPECT_EQ(vigem.lastMouseDx, 0);
    EXPECT_EQ(vigem.lastMouseDy, 0);
}

static void test_replug_resetsStreamCaches() {
    TEST("replug: type change clears cached stream state (no phantom samples)");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_PLAYSTATION, CAP_MOTION)});
    MotionReport m{};
    m.gyroX = 5;
    svc.handleMotionData(r.token, 0, m);
    BatteryReport b{};
    b.level = 50;
    b.status = BATTERY_STATUS_DISCHARGING;
    svc.handleBatteryUpdate(r.token, 0, b);
    {
        auto snap = svc.getConnectionsSnapshot();
        EXPECT(snap.connections[0].controllers[0].motionActive);
        EXPECT(snap.connections[0].controllers[0].batteryKnown);
    }
    upsert(svc, {makeDesc(0, CONTROLLER_TYPE_XBOX, CAP_MOTION)});
    {
        auto snap = svc.getConnectionsSnapshot();
        EXPECT(!snap.connections[0].controllers[0].motionActive);
        EXPECT(!snap.connections[0].controllers[0].batteryKnown);
    }
}

static void test_rumble_forwardsAndCoalesces() {
    TEST("rumble: backend event forwarded once, identical repeats coalesced");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_XBOX, CAP_RUMBLE)});
    uint32_t serial = vigem.pluggedSerials.back();
    RumbleReport rr{};
    rr.strongMagnitude = 30000;
    rr.weakMagnitude = 10000;
    vigem.fireRumble(serial, rr);
    EXPECT_EQ(client.rumbleCalls, 1);
    EXPECT_EQ(client.lastRumbleConnToken, r.token);
    EXPECT_EQ(client.lastRumble.durationMs, (uint16_t)500); // stamped
    vigem.fireRumble(serial, rr);                           // identical → coalesced
    EXPECT_EQ(client.rumbleCalls, 1);
    rr.strongMagnitude = 0;
    rr.weakMagnitude = 0;
    vigem.fireRumble(serial, rr); // change → forwarded
    EXPECT_EQ(client.rumbleCalls, 2);
    vigem.fireRumble(9999, rr); // stray serial → dropped
    EXPECT_EQ(client.rumbleCalls, 2);
}

static void test_lightbar_gatedOnCap() {
    TEST("lightbar: forwarded only to CAP_LIGHTBAR senders, always cached");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    upsert(svc, {makeDesc(0, CONTROLLER_TYPE_PLAYSTATION, CAP_LIGHTBAR),
                 makeDesc(1, CONTROLLER_TYPE_PLAYSTATION, 0)});
    uint32_t serial0 = 0, serial1 = 0;
    {
        auto snap = svc.getConnectionsSnapshot();
        for (auto& c : snap.connections[0].controllers) {
            if (c.index == 0) serial0 = c.serial;
            if (c.index == 1) serial1 = c.serial;
        }
    }
    vigem.fireLightbar(serial0, 10, 20, 30);
    EXPECT_EQ(client.lightbarCalls, 1);
    EXPECT_EQ((int)client.lastLightbarR, 10);
    vigem.fireLightbar(serial0, 10, 20, 30); // identical → coalesced
    EXPECT_EQ(client.lightbarCalls, 1);
    vigem.fireLightbar(serial1, 1, 2, 3); // no cap → cached, not sent
    EXPECT_EQ(client.lightbarCalls, 1);
    auto snap = svc.getConnectionsSnapshot();
    for (auto& c : snap.connections[0].controllers) {
        if (c.index == 1) {
            EXPECT(c.lightbarKnown);
            EXPECT_EQ((int)c.lightbarR, 1);
        }
    }
}

static TriggerEffectsReport makeTriggerEffects(uint8_t leftMode, uint8_t rightMode) {
    TriggerEffectsReport t{};
    t.left[0] = leftMode;
    t.left[1] = 0x11;
    t.left[10] = 0xAA;
    t.right[0] = rightMode;
    t.right[1] = 0x22;
    t.right[10] = 0xBB;
    return t;
}

static void test_triggerEffects_gatedOnCap() {
    TEST("trigger effects: forwarded only to CAP_TRIGGER_EFFECTS senders, coalesced");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    EXPECT_EQ(vigem.setTriggerEffectsCallbackCalls, 1);

    upsert(svc, {makeDesc(0, CONTROLLER_TYPE_DUALSENSE, CAP_TRIGGER_EFFECTS),
                 makeDesc(1, CONTROLLER_TYPE_DUALSENSE, 0)});
    uint32_t serial0 = 0, serial1 = 0;
    {
        auto snap = svc.getConnectionsSnapshot();
        for (auto& c : snap.connections[0].controllers) {
            if (c.index == 0) serial0 = c.serial;
            if (c.index == 1) serial1 = c.serial;
        }
    }
    const TriggerEffectsReport fx = makeTriggerEffects(0x21, 0x26);
    vigem.fireTriggerEffects(serial0, fx);
    EXPECT_EQ(client.triggerEffectsCalls, 1);
    EXPECT_EQ((int)client.lastTriggerEffectsCtrlIdx, 0);
    EXPECT((client.lastTriggerEffects == fx));
    vigem.fireTriggerEffects(serial0, fx); // identical → coalesced
    EXPECT_EQ(client.triggerEffectsCalls, 1);
    TriggerEffectsReport fx2 = fx;
    fx2.right[0] = 0x01; // one changed byte → forwarded
    vigem.fireTriggerEffects(serial0, fx2);
    EXPECT_EQ(client.triggerEffectsCalls, 2);
    vigem.fireTriggerEffects(serial1, fx); // no cap → dropped from the wire
    EXPECT_EQ(client.triggerEffectsCalls, 2);
    vigem.fireTriggerEffects(9999, fx); // stray serial → dropped
    EXPECT_EQ(client.triggerEffectsCalls, 2);
}

static void test_playerLeds_gatedOnCap() {
    TEST("player LEDs: forwarded only to CAP_PLAYER_LEDS senders, coalesced");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    EXPECT_EQ(vigem.setPlayerLedsCallbackCalls, 1);

    upsert(svc, {makeDesc(0, CONTROLLER_TYPE_DUALSENSE, CAP_PLAYER_LEDS),
                 makeDesc(1, CONTROLLER_TYPE_DUALSENSE, 0)});
    uint32_t serial0 = 0, serial1 = 0;
    {
        auto snap = svc.getConnectionsSnapshot();
        for (auto& c : snap.connections[0].controllers) {
            if (c.index == 0) serial0 = c.serial;
            if (c.index == 1) serial1 = c.serial;
        }
    }
    vigem.firePlayerLeds(serial0, 0x01);
    EXPECT_EQ(client.playerLedsCalls, 1);
    EXPECT_EQ((int)client.lastPlayerLeds, 0x01);
    vigem.firePlayerLeds(serial0, 0x01); // identical → coalesced
    EXPECT_EQ(client.playerLedsCalls, 1);
    vigem.firePlayerLeds(serial0, 0x03);
    EXPECT_EQ(client.playerLedsCalls, 2);
    EXPECT_EQ((int)client.lastPlayerLeds, 0x03);
    vigem.firePlayerLeds(serial1, 0x01); // no cap → dropped
    EXPECT_EQ(client.playerLedsCalls, 2);
    vigem.firePlayerLeds(9999, 0x01); // stray serial → dropped
    EXPECT_EQ(client.playerLedsCalls, 2);
}

// ---- controller audio ------------------------------------------------------

// An Opus-shaped payload. SAT-1's gates never look inside a packet, so only
// the length is load-bearing here.
static std::vector<uint8_t> opusFrame(size_t bytes) { return std::vector<uint8_t>(bytes, 0x5A); }

// Serials of the slots this connection plugged, by controller index.
static uint32_t serialOfSlot(SessionService& svc, uint8_t index) {
    auto snap = svc.getConnectionsSnapshot();
    if (snap.connections.empty()) return 0;
    for (auto& c : snap.connections[0].controllers) {
        if (c.index == index) return c.serial;
    }
    return 0;
}

static void test_micAudio_gatedOnCap() {
    TEST("mic audio: accepted only from slots that advertised CAP_MIC");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_DUALSENSE, CAP_MIC),
                          makeDesc(1, CONTROLLER_TYPE_DUALSENSE, CAP_SPEAKER)});
    const auto opus = opusFrame(64);
    EXPECT(svc.handleMicAudio(r.token, 0, 1, opus.data(), opus.size()));

    // CAP_SPEAKER is the other direction and grants nothing here: caps
    // advertise the client's own actuator/source, one bit per direction.
    EXPECT(!svc.handleMicAudio(r.token, 1, 1, opus.data(), opus.size()));
    EXPECT(log.contains("never advertised the mic cap"));

    // Malformed packets are refused whatever the caps say.
    EXPECT(!svc.handleMicAudio(r.token, 0, 2, opus.data(), 0));
    EXPECT(!svc.handleMicAudio(r.token, 0, 2, nullptr, opus.size()));
}

static void test_micAudio_unknownControllerAndToken() {
    TEST("mic audio: unbound slot, out-of-range slot and unknown token all drop");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_DUALSENSE, CAP_MIC)});
    const auto opus = opusFrame(32);

    EXPECT(!svc.handleMicAudio(r.token, 3, 0, opus.data(), opus.size()));   // never bound
    EXPECT(!svc.handleMicAudio(r.token, 200, 0, opus.data(), opus.size())); // past the slot cap
    EXPECT(log.contains("no such bound controller"));

    // An unknown token has no session to log against; a stale or spoofed
    // datagram must not be able to write log lines.
    const int before = log.logCalls;
    EXPECT(!svc.handleMicAudio(r.token ^ 0xFFFFFFFFu, 0, 0, opus.data(), opus.size()));
    EXPECT_EQ(log.logCalls, before);
}

static void test_micAudio_rateLimitIsPerController() {
    TEST("mic audio: the packets/s ceiling is per controller, not per session");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_DUALSENSE, CAP_MIC),
                          makeDesc(1, CONTROLLER_TYPE_DUALSENSE, CAP_MIC)});
    const auto opus = opusFrame(48);

    // The window is a whole second, so a tight loop cannot outrun it: exactly
    // MIC_AUDIO_MAX_PACKETS_PER_SEC get through and the overflow is dropped.
    int accepted = 0;
    for (int i = 0; i < MIC_AUDIO_MAX_PACKETS_PER_SEC + 25; ++i) {
        if (svc.handleMicAudio(r.token, 0, (uint16_t)i, opus.data(), opus.size())) accepted++;
    }
    EXPECT_EQ(accepted, MIC_AUDIO_MAX_PACKETS_PER_SEC);
    EXPECT(log.contains("packets/s"));

    // A flooding slot must not starve its neighbour.
    EXPECT(svc.handleMicAudio(r.token, 1, 0, opus.data(), opus.size()));
}

static void test_micAudio_replugRestoresAllowance() {
    TEST("mic audio: a replugged pad gets a fresh rate-limit window");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_DUALSENSE, CAP_MIC)});
    const auto opus = opusFrame(48);
    for (int i = 0; i <= MIC_AUDIO_MAX_PACKETS_PER_SEC; ++i) {
        (void)svc.handleMicAudio(r.token, 0, (uint16_t)i, opus.data(), opus.size());
    }
    EXPECT(!svc.handleMicAudio(r.token, 0, 999, opus.data(), opus.size()));

    // Identity change forces a replug. Carrying the spent window onto the fresh
    // actuator would swallow the first second of the new stream.
    upsert(svc, {makeDesc(0, CONTROLLER_TYPE_XBOX, CAP_MIC)});
    auto back = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_DUALSENSE, CAP_MIC)});
    EXPECT(svc.handleMicAudio(back.token, 0, 0, opus.data(), opus.size()));
}

static void test_micAudio_dropLoggedOncePerSessionPerCause() {
    TEST("mic audio: each drop cause logs once per session, and a re-PUT rearms it");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto r = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_DUALSENSE, 0)});
    const auto opus = opusFrame(32);

    // 50 frames a second of the same mistake must not become 50 log lines.
    for (int i = 0; i < 20; ++i) {
        (void)svc.handleMicAudio(r.token, 0, (uint16_t)i, opus.data(), opus.size());
    }
    EXPECT_EQ(log.countContaining("never advertised the mic cap"), 1);

    // A different cause is worth its own line, once.
    for (int i = 0; i < 20; ++i) {
        (void)svc.handleMicAudio(r.token, 4, (uint16_t)i, opus.data(), opus.size());
    }
    EXPECT_EQ(log.countContaining("no such bound controller"), 1);
    EXPECT_EQ(log.countContaining("never advertised the mic cap"), 1);

    // A re-PUT rotates token/salt/key, so it is a new session: a repeat mistake
    // is diagnosable again instead of silently inheriting the old flag.
    auto r2 = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_DUALSENSE, 0)});
    (void)svc.handleMicAudio(r2.token, 0, 0, opus.data(), opus.size());
    EXPECT_EQ(log.countContaining("never advertised the mic cap"), 2);
}

static void test_micLed_gatedOnCapAndCoalesced() {
    TEST("mic LED: forwarded only to CAP_MIC senders, coalesced, bad states dropped");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);
    EXPECT_EQ(vigem.setMicLedCallbackCalls, 1);

    upsert(svc, {makeDesc(0, CONTROLLER_TYPE_DUALSENSE, CAP_MIC),
                 makeDesc(1, CONTROLLER_TYPE_DUALSENSE, CAP_SPEAKER)});
    const uint32_t serial0 = serialOfSlot(svc, 0);
    const uint32_t serial1 = serialOfSlot(svc, 1);

    vigem.fireMicLed(serial0, MIC_LED_STATE_ON);
    EXPECT_EQ(client.micLedCalls, 1);
    EXPECT_EQ((int)client.lastMicLedCtrlIdx, 0);
    EXPECT_EQ((int)client.lastMicLedState, (int)MIC_LED_STATE_ON);

    vigem.fireMicLed(serial0, MIC_LED_STATE_ON); // identical → coalesced
    EXPECT_EQ(client.micLedCalls, 1);

    vigem.fireMicLed(serial0, MIC_LED_STATE_PULSE);
    EXPECT_EQ(client.micLedCalls, 2);
    EXPECT_EQ((int)client.lastMicLedState, (int)MIC_LED_STATE_PULSE);
    vigem.fireMicLed(serial0, MIC_LED_STATE_OFF);
    EXPECT_EQ(client.micLedCalls, 3);

    // CAP_SPEAKER does not open this path: the lamp reports a microphone's
    // state, so it rides CAP_MIC or not at all.
    vigem.fireMicLed(serial1, MIC_LED_STATE_ON);
    EXPECT_EQ(client.micLedCalls, 3);

    vigem.fireMicLed(9999, MIC_LED_STATE_ON); // stray serial → dropped
    EXPECT_EQ(client.micLedCalls, 3);

    // Out-of-vocabulary state bytes are a decode bug or a hostile report;
    // dropped rather than forwarded for the client to guess at.
    vigem.fireMicLed(serial0, MIC_LED_STATE_COUNT);
    vigem.fireMicLed(serial0, 0xFF);
    EXPECT_EQ(client.micLedCalls, 3);
}

// Interleaved stereo the speaker path can encode. Non-silent so a wrong frame
// count is visible as a wrong packet count rather than as identical silence,
// and so the exact-silence suppression in sendSpeakerFrameLocked does not eat
// frames a test meant to send.
static std::vector<int16_t> speakerPcm(size_t frames, int frameIndex = 0) {
    std::vector<int16_t> pcm(frames * AUDIO_SPEAKER_CHANNELS);
    for (size_t i = 0; i < frames; i++) {
        const double t =
            (frameIndex * (double)AUDIO_FRAME_SAMPLES + (double)i) / AUDIO_SAMPLE_RATE_HZ;
        pcm[i * 2 + 0] = (int16_t)(9000.0 * std::sin(2.0 * 3.14159265358979 * 330.0 * t));
        pcm[i * 2 + 1] = (int16_t)(4000.0 * std::sin(2.0 * 3.14159265358979 * 550.0 * t));
    }
    return pcm;
}

static void test_speakerAudio_gatedOnCap() {
    TEST("speaker audio: backend frames gated on CAP_SPEAKER, strays and empties dropped");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    satellite::audio::OpusCodecFactory codecs;
    SessionService svc(vigem, client, log, {}, &codecs);
    EXPECT_EQ(vigem.setSpeakerAudioCallbackCalls, 1);

    upsert(svc, {makeDesc(0, CONTROLLER_TYPE_DUALSENSE, CAP_SPEAKER),
                 makeDesc(1, CONTROLLER_TYPE_DUALSENSE, CAP_MIC)});
    const uint32_t serial0 = serialOfSlot(svc, 0);
    const uint32_t serial1 = serialOfSlot(svc, 1);

    const auto pcm = speakerPcm(AUDIO_FRAME_SAMPLES, 0);
    EXPECT(svc.handleSpeakerAudioFromBackend(serial0, pcm.data(), AUDIO_FRAME_SAMPLES));

    // CAP_MIC is the other direction: encoding for a client that would drop the
    // frame is pure wasted CPU, so the gate is before the encoder.
    EXPECT(!svc.handleSpeakerAudioFromBackend(serial1, pcm.data(), AUDIO_FRAME_SAMPLES));
    EXPECT(!svc.handleSpeakerAudioFromBackend(9999, pcm.data(), AUDIO_FRAME_SAMPLES));
    EXPECT(!svc.handleSpeakerAudioFromBackend(serial0, nullptr, AUDIO_FRAME_SAMPLES));
    EXPECT(!svc.handleSpeakerAudioFromBackend(serial0, pcm.data(), 0));

    // The registered backend callback lands on the same path.
    vigem.fireSpeakerAudio(serial0, pcm.data(), AUDIO_FRAME_SAMPLES);

    // Two whole windows got through the gate (the direct call and the callback)
    // and each became exactly one wire packet; everything the gate refused
    // produced nothing, which is what makes this an encoder-side gate rather
    // than a send-side one.
    EXPECT_EQ(client.speakerAudioCalls, 2);
    EXPECT_EQ((int)client.lastSpeakerAudioCtrlIdx, 0);
    EXPECT(!client.lastSpeakerAudioOpus.empty());
}

// ---- controller audio: exact-silence suppression ----------------------------

// Windows renders digital zeros into the pad's endpoint whenever nothing is
// playing to it. Encoding those cost ~28 kbps of Opus and ~52 kbps on the wire
// once framing is counted, for a stream carrying nothing.
static void test_speakerAudio_exactSilenceIsNotSent() {
    TEST("speaker audio: an all-zero window produces no packet at all");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    satellite::audio::OpusCodecFactory codecs;
    SessionService svc(vigem, client, log, {}, &codecs);

    upsert(svc, {makeDesc(0, CONTROLLER_TYPE_DUALSENSE, CAP_SPEAKER)});
    const uint32_t serial0 = serialOfSlot(svc, 0);

    const std::vector<int16_t> silence(AUDIO_FRAME_SAMPLES * AUDIO_SPEAKER_CHANNELS, 0);
    for (int i = 0; i < 50; i++) {
        EXPECT(svc.handleSpeakerAudioFromBackend(serial0, silence.data(), AUDIO_FRAME_SAMPLES));
    }
    EXPECT_EQ(client.speakerAudioCalls, 0);

    TEST("speaker audio: suppression does not burn sequence numbers");
    // A suppressed window is not a hole, so it must not advance seq: a client
    // that saw a jump would run PLC over a gap with no signal to guess at, and
    // Opus would invent noise where the game wrote none.
    const auto tone = speakerPcm(AUDIO_FRAME_SAMPLES, 0);
    EXPECT(svc.handleSpeakerAudioFromBackend(serial0, tone.data(), AUDIO_FRAME_SAMPLES));
    EXPECT_EQ(client.speakerAudioCalls, 1);
    EXPECT_EQ((int)client.lastSpeakerAudioSeq, 0);

    TEST("speaker audio: seq stays contiguous across a silent stretch");
    for (int i = 0; i < 25; i++) {
        EXPECT(svc.handleSpeakerAudioFromBackend(serial0, silence.data(), AUDIO_FRAME_SAMPLES));
    }
    EXPECT_EQ(client.speakerAudioCalls, 1);
    EXPECT(svc.handleSpeakerAudioFromBackend(serial0, tone.data(), AUDIO_FRAME_SAMPLES));
    EXPECT_EQ(client.speakerAudioCalls, 2);
    EXPECT_EQ((int)client.lastSpeakerAudioSeq, 1);
}

static void test_speakerAudio_anythingAudibleStillGoes() {
    TEST("speaker audio: one non-zero sample in a window is enough to send it");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    satellite::audio::OpusCodecFactory codecs;
    SessionService svc(vigem, client, log, {}, &codecs);

    upsert(svc, {makeDesc(0, CONTROLLER_TYPE_DUALSENSE, CAP_SPEAKER)});
    const uint32_t serial0 = serialOfSlot(svc, 0);

    const size_t samples = (size_t)AUDIO_FRAME_SAMPLES * AUDIO_SPEAKER_CHANNELS;
    for (size_t at : {(size_t)0, samples / 2, samples - 1}) {
        std::vector<int16_t> nearly(samples, 0);
        nearly[at] = 1; // the quietest representable sample
        const int before = client.speakerAudioCalls;
        EXPECT(svc.handleSpeakerAudioFromBackend(serial0, nearly.data(), AUDIO_FRAME_SAMPLES));
        EXPECT_EQ(client.speakerAudioCalls, before + 1);
    }

    TEST("speaker audio: a half-silent window is still audible content");
    std::vector<int16_t> half(samples, 0);
    const auto tone = speakerPcm(AUDIO_FRAME_SAMPLES, 1);
    for (size_t i = samples / 2; i < samples; i++) half[i] = tone[i];
    const int before = client.speakerAudioCalls;
    EXPECT(svc.handleSpeakerAudioFromBackend(serial0, half.data(), AUDIO_FRAME_SAMPLES));
    EXPECT_EQ(client.speakerAudioCalls, before + 1);
}

// ---- controller audio: the host's per-direction switches --------------------

static void test_audioPolicy_defaultsToBothOn() {
    TEST("audio policy: a service with no policy wired streams both directions");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    satellite::audio::OpusCodecFactory codecs;
    SessionService svc(vigem, client, log, {}, &codecs);

    auto r = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_DUALSENSE, CAP_MIC | CAP_SPEAKER)});
    const uint32_t serial0 = serialOfSlot(svc, 0);

    const auto tone = speakerPcm(AUDIO_FRAME_SAMPLES, 0);
    EXPECT(svc.handleSpeakerAudioFromBackend(serial0, tone.data(), AUDIO_FRAME_SAMPLES));
    EXPECT_EQ(client.speakerAudioCalls, 1);

    const auto opus = opusFrame(32);
    EXPECT(svc.handleMicAudio(r.token, 0, 0, opus.data(), opus.size()));
}

static void test_audioPolicy_speakerOffStopsTheStream() {
    TEST("audio policy: speaker off sends nothing, even for audible content");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    satellite::audio::OpusCodecFactory codecs;
    SessionService svc(vigem, client, log, {}, &codecs);

    bool speakerOn = false;
    svc.setAudioPolicy([&] { return ControllerAudioPolicy{true, speakerOn}; });

    upsert(svc, {makeDesc(0, CONTROLLER_TYPE_DUALSENSE, CAP_SPEAKER)});
    const uint32_t serial0 = serialOfSlot(svc, 0);
    const auto tone = speakerPcm(AUDIO_FRAME_SAMPLES, 0);

    for (int i = 0; i < 10; i++) {
        EXPECT(!svc.handleSpeakerAudioFromBackend(serial0, tone.data(), AUDIO_FRAME_SAMPLES));
    }
    EXPECT_EQ(client.speakerAudioCalls, 0);

    TEST("audio policy: the speaker gate is read per frame, not cached at plug");
    // The whole point of gating the wire rather than the persona: the pad keeps
    // its Windows endpoints, so the switch has to reach a stream already
    // playing instead of waiting for a replug.
    speakerOn = true;
    EXPECT(svc.handleSpeakerAudioFromBackend(serial0, tone.data(), AUDIO_FRAME_SAMPLES));
    EXPECT_EQ(client.speakerAudioCalls, 1);
    EXPECT_EQ((int)client.lastSpeakerAudioSeq, 0);

    speakerOn = false;
    EXPECT(!svc.handleSpeakerAudioFromBackend(serial0, tone.data(), AUDIO_FRAME_SAMPLES));
    EXPECT_EQ(client.speakerAudioCalls, 1);

    TEST("audio policy: a gated speaker frame burns no sequence number");
    speakerOn = true;
    EXPECT(svc.handleSpeakerAudioFromBackend(serial0, tone.data(), AUDIO_FRAME_SAMPLES));
    EXPECT_EQ((int)client.lastSpeakerAudioSeq, 1);
}

static void test_audioPolicy_micOffDropsWithAReason() {
    TEST("audio policy: mic off drops the frame before it reaches the backend");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    satellite::audio::OpusCodecFactory codecs;
    SessionService svc(vigem, client, log, {}, &codecs);

    bool micOn = false;
    svc.setAudioPolicy([&] { return ControllerAudioPolicy{micOn, true}; });

    auto r = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_DUALSENSE, CAP_MIC)});
    const auto opus = opusFrame(32);
    for (int i = 0; i < 20; i++) {
        EXPECT(!svc.handleMicAudio(r.token, 0, (uint16_t)i, opus.data(), opus.size()));
    }
    EXPECT_EQ(vigem.submitMicAudioCalls, 0);

    TEST("audio policy: the host-off drop logs once per session, with its own cause");
    // The client cannot see this setting, so a mic going nowhere has to leave
    // something behind to diagnose it with -- but only one line, not 50 a second.
    EXPECT_EQ(log.countContaining("controller microphone is off on the host"), 1);

    TEST("audio policy: a capless sender is still told about its own mistake first");
    auto r2 = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_DUALSENSE, 0)}, "devB", "B");
    (void)svc.handleMicAudio(r2.token, 0, 0, opus.data(), opus.size());
    EXPECT_EQ(log.countContaining("never advertised the mic cap"), 1);

    TEST("audio policy: the mic gate is read per frame too");
    micOn = true;
    EXPECT(svc.handleMicAudio(r.token, 0, 100, opus.data(), opus.size()));

    TEST("audio policy: a host-disabled mic frame consumes no rate-limit budget");
    // Checked before the window so a direction that is off cannot be used to
    // exhaust the budget a re-enabled one would need.
    micOn = false;
    for (int i = 0; i < MIC_AUDIO_MAX_PACKETS_PER_SEC * 2; i++) {
        (void)svc.handleMicAudio(r.token, 0, (uint16_t)(200 + i), opus.data(), opus.size());
    }
    micOn = true;
    EXPECT(svc.handleMicAudio(r.token, 0, 900, opus.data(), opus.size()));
    EXPECT_EQ(log.countContaining("packets/s"), 0);
}

static void test_audioPolicy_directionsAreIndependent() {
    TEST("audio policy: turning one direction off leaves the other alone");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    satellite::audio::OpusCodecFactory codecs;
    SessionService svc(vigem, client, log, {}, &codecs);

    svc.setAudioPolicy([] { return ControllerAudioPolicy{true, false}; });

    auto r = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_DUALSENSE, CAP_MIC | CAP_SPEAKER)});
    const uint32_t serial0 = serialOfSlot(svc, 0);

    const auto tone = speakerPcm(AUDIO_FRAME_SAMPLES, 0);
    EXPECT(!svc.handleSpeakerAudioFromBackend(serial0, tone.data(), AUDIO_FRAME_SAMPLES));
    EXPECT_EQ(client.speakerAudioCalls, 0);

    // Acceptance, not backend submission: opusFrame is a synthetic gate
    // fixture the real decoder rejects, and the decode path has its own tests.
    // What matters here is that the mic direction was never gated.
    const auto opus = opusFrame(32);
    for (uint16_t seq = 0; seq < 4; seq++) {
        EXPECT(svc.handleMicAudio(r.token, 0, seq, opus.data(), opus.size()));
    }
    EXPECT_EQ(log.countContaining("controller microphone is off on the host"), 0);

    TEST("audio policy: mic LED still rides CAP_MIC, not the host's mic switch");
    // The lamp reports what the game asked for. Muting the host's intake does
    // not make the game's request untrue, and the client renders it against its
    // own mute state anyway.
    vigem.fireMicLed(serial0, MIC_LED_STATE_ON);
    EXPECT_EQ(client.micLedCalls, 1);
}

// ---- controller audio: the codec paths --------------------------------------

// Real 20 ms mic packets from the production encoder, so the service decodes
// what a client would actually send rather than a hand-rolled byte string.
struct MicPacketSource {
    std::unique_ptr<IAudioEncoder> enc =
        satellite::audio::OpusStreamEncoder::create(satellite::audio::Stream::Mic);
    int frameIndex = 0;

    std::vector<uint8_t> next() {
        std::vector<int16_t> src(AUDIO_FRAME_SAMPLES);
        for (int i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
            const double t = (frameIndex * (double)AUDIO_FRAME_SAMPLES + i) / AUDIO_SAMPLE_RATE_HZ;
            src[i] = (int16_t)(7000.0 * std::sin(2.0 * 3.14159265358979 * 220.0 * t) +
                               3000.0 * std::sin(2.0 * 3.14159265358979 * 660.0 * t));
        }
        frameIndex++;
        uint8_t buf[1024];
        const size_t n = enc->encode(src.data(), AUDIO_FRAME_SAMPLES, buf, sizeof(buf));
        return std::vector<uint8_t>(buf, buf + n);
    }
};

static void test_micAudio_decodesRealPacketsToTheBackend() {
    TEST("mic audio: real Opus frames decode to 20 ms of PCM on the pad's mic endpoint");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    satellite::audio::OpusCodecFactory codecs;
    SessionService svc(vigem, client, log, {}, &codecs);

    auto r = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_DUALSENSE, CAP_MIC)});
    const uint32_t serial0 = serialOfSlot(svc, 0);
    MicPacketSource mic;

    for (int i = 0; i < 10; i++) {
        const auto pkt = mic.next();
        EXPECT(svc.handleMicAudio(r.token, 0, (uint16_t)i, pkt.data(), pkt.size()));
        EXPECT_EQ(vigem.submitMicAudioCalls, i + 1);
    }
    // One wire frame is one 20 ms window, mono, routed to the pad this slot
    // actually plugged.
    EXPECT_EQ(vigem.lastMicAudioSamples, (size_t)AUDIO_FRAME_SAMPLES);
    EXPECT_EQ(vigem.lastMicAudioSerial, serial0);
    // Real audio came out, not a zeroed buffer: a service that counted frames
    // without decoding them would pass every assertion above.
    EXPECT(vigem.lastMicAudioEnergy() > 10000.0);

    // A backend with no mic endpoint on this serial says so, and that is not an
    // error: senders keep streaming and the service keeps accepting.
    vigem.submitMicAudioReturnVal = false;
    const auto pkt = mic.next();
    EXPECT(svc.handleMicAudio(r.token, 0, 10, pkt.data(), pkt.size()));
    EXPECT_EQ(vigem.submitMicAudioCalls, 11);
    vigem.submitMicAudioReturnVal = true;

    // A packet claiming twice the wire's window (Opus TOC frame-count code 1
    // over a duplicated body) decodes to nothing rather than overrunning the
    // fixed 20 ms buffer the decode path hands the codec. Accepted at the gate,
    // dropped at the decoder, no PCM.
    const auto base = mic.next();
    std::vector<uint8_t> twoFrames;
    twoFrames.push_back((uint8_t)((base[0] & 0xFC) | 0x01));
    twoFrames.insert(twoFrames.end(), base.begin() + 1, base.end());
    twoFrames.insert(twoFrames.end(), base.begin() + 1, base.end());
    const int before = vigem.submitMicAudioCalls;
    EXPECT(svc.handleMicAudio(r.token, 0, 11, twoFrames.data(), twoFrames.size()));
    EXPECT_EQ(vigem.submitMicAudioCalls, before);
}

static void test_micAudio_withoutCodecAcceptsAndDrops() {
    TEST("mic audio: with no codec wired the gates still run and the frame is dropped");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log); // no factory

    auto r = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_DUALSENSE, CAP_MIC)});
    MicPacketSource mic;
    for (int i = 0; i < 4; i++) {
        const auto pkt = mic.next();
        EXPECT(svc.handleMicAudio(r.token, 0, (uint16_t)i, pkt.data(), pkt.size()));
    }
    // Accepted (it passed every gate) but nowhere to go. This is the pre-SAT-2
    // behaviour, kept deliberately so a build without a codec degrades to
    // "no audio" rather than to a broken session.
    EXPECT_EQ(vigem.submitMicAudioCalls, 0);
}

static void test_micAudio_gapConcealedAndReorderHealed() {
    TEST("mic audio: a skipped seq is concealed; a swapped pair is reordered, not concealed");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    satellite::audio::OpusCodecFactory codecs;
    SessionService svc(vigem, client, log, {}, &codecs);

    auto r = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_DUALSENSE, CAP_MIC)});
    MicPacketSource mic;
    std::vector<std::vector<uint8_t>> pkts;
    for (int i = 0; i < 12; i++) pkts.push_back(mic.next());

    for (int i = 0; i < 4; i++) {
        EXPECT(svc.handleMicAudio(r.token, 0, (uint16_t)i, pkts[i].data(), pkts[i].size()));
    }
    EXPECT_EQ(vigem.submitMicAudioCalls, 4);

    // Frame 4 is lost. Packet 5 alone proves nothing (it could be a reorder),
    // so nothing is submitted yet: the 40 ms window is exactly the price of not
    // concealing a frame that was merely out of order.
    EXPECT(svc.handleMicAudio(r.token, 0, 5, pkts[5].data(), pkts[5].size()));
    EXPECT_EQ(vigem.submitMicAudioCalls, 4);

    // Packet 6 lands 2 ahead of the missing one, which settles it: the gap is
    // filled (from packet 5's in-band FEC copy of frame 4), then 5 and 6 play.
    EXPECT(svc.handleMicAudio(r.token, 0, 6, pkts[6].data(), pkts[6].size()));
    EXPECT_EQ(vigem.submitMicAudioCalls, 7);
    EXPECT_EQ(vigem.lastMicAudioSamples, (size_t)AUDIO_FRAME_SAMPLES);
    // The concealed frame is audio in its own right, not a hole padded with
    // zeros: submission 4 is the one the decoder invented for the lost packet.
    EXPECT_EQ((int)vigem.micAudioEnergies.size(), 7);
    EXPECT(vigem.micAudioEnergies[4] > 10000.0);
    EXPECT(vigem.micAudioEnergies[5] > 10000.0);
    EXPECT(vigem.lastMicAudioEnergy() > 10000.0);

    // A one-frame swap costs nothing: 8 before 7 waits, then both go together.
    EXPECT(svc.handleMicAudio(r.token, 0, 8, pkts[8].data(), pkts[8].size()));
    EXPECT_EQ(vigem.submitMicAudioCalls, 7);
    EXPECT(svc.handleMicAudio(r.token, 0, 7, pkts[7].data(), pkts[7].size()));
    EXPECT_EQ(vigem.submitMicAudioCalls, 9);

    // And the frame that finally turns up after its slot was concealed is
    // refused rather than played out of order.
    EXPECT(!svc.handleMicAudio(r.token, 0, 4, pkts[4].data(), pkts[4].size()));
    EXPECT_EQ(vigem.submitMicAudioCalls, 9);
}

static void test_micAudio_streamsPastTheRateWindow() {
    TEST("mic audio: a stream longer than one rate window keeps decoding");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    satellite::audio::OpusCodecFactory codecs;
    SessionService svc(vigem, client, log, {}, &codecs);

    auto r = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_DUALSENSE, CAP_MIC)});
    MicPacketSource mic;
    // Four seconds of speech at the wire's 50 packets/s. The limiter counts
    // wall-clock seconds and a test loop outruns them, so the window is
    // reopened on the cadence a real second would (see resetMicRateWindowForTest).
    const int frames = 200;
    for (int i = 0; i < frames; i++) {
        if (i % MIC_AUDIO_MAX_PACKETS_PER_SEC == 0) svc.resetMicRateWindowForTest(r.token, 0);
        const auto pkt = mic.next();
        EXPECT(svc.handleMicAudio(r.token, 0, (uint16_t)i, pkt.data(), pkt.size()));
    }
    EXPECT_EQ(vigem.submitMicAudioCalls, frames);
    EXPECT(vigem.lastMicAudioEnergy() > 10000.0);
}

static void test_micAudio_seqWrapsMidStream() {
    TEST("mic audio: the u16 seq wrapping past 0xFFFF is not a 65k-frame gap");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    satellite::audio::OpusCodecFactory codecs;
    SessionService svc(vigem, client, log, {}, &codecs);

    auto r = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_DUALSENSE, CAP_MIC)});
    MicPacketSource mic;
    // Straddle the wrap: a client that has been talking for 22 minutes hits it,
    // and reading 0x0000 as ancient would silence the stream from there on.
    for (int i = 0; i < 6; i++) {
        svc.resetMicRateWindowForTest(r.token, 0);
        const uint16_t seq = (uint16_t)(0xFFFD + i);
        const auto pkt = mic.next();
        EXPECT(svc.handleMicAudio(r.token, 0, seq, pkt.data(), pkt.size()));
        EXPECT_EQ(vigem.submitMicAudioCalls, i + 1);
    }
}

static void test_micAudio_codecStateIsPerControllerAndDiesWithThePad() {
    TEST("mic audio: each pad decodes its own stream, and a replug starts a fresh one");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    satellite::audio::OpusCodecFactory codecs;
    SessionService svc(vigem, client, log, {}, &codecs);

    auto r = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_DUALSENSE, CAP_MIC),
                          makeDesc(1, CONTROLLER_TYPE_DUALSENSE, CAP_MIC)});
    const uint32_t serial0 = serialOfSlot(svc, 0);
    const uint32_t serial1 = serialOfSlot(svc, 1);
    MicPacketSource mic;

    // Slot 0 gets to seq 40; slot 1 then starts its own stream at 0. A shared
    // window would read that as 40 frames in the past and drop it.
    for (int i = 0; i < 41; i++) {
        const auto pkt = mic.next();
        EXPECT(svc.handleMicAudio(r.token, 0, (uint16_t)i, pkt.data(), pkt.size()));
    }
    EXPECT_EQ(vigem.submitMicAudioCalls, 41);
    EXPECT_EQ(vigem.lastMicAudioSerial, serial0);

    const auto slot1 = mic.next();
    EXPECT(svc.handleMicAudio(r.token, 1, 0, slot1.data(), slot1.size()));
    EXPECT_EQ(vigem.submitMicAudioCalls, 42);
    EXPECT_EQ(vigem.lastMicAudioSerial, serial1);

    // Identity change forces a replug. The new pad is a new stream: its decoder
    // must not carry the old pad's filter state, and its window must not treat
    // a restarted seq as 40 frames of history.
    upsert(svc, {makeDesc(0, CONTROLLER_TYPE_XBOX, CAP_MIC),
                 makeDesc(1, CONTROLLER_TYPE_DUALSENSE, CAP_MIC)});
    // A re-PUT rotates the token, so the stream continues under the new one.
    auto again = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_DUALSENSE, CAP_MIC),
                              makeDesc(1, CONTROLLER_TYPE_DUALSENSE, CAP_MIC)});
    const int before = vigem.submitMicAudioCalls;
    const uint32_t serial0b = serialOfSlot(svc, 0);
    EXPECT(serial0b != serial0);

    MicPacketSource fresh;
    const auto restart = fresh.next();
    EXPECT(svc.handleMicAudio(again.token, 0, 0, restart.data(), restart.size()));
    EXPECT_EQ(vigem.submitMicAudioCalls, before + 1);
    EXPECT_EQ(vigem.lastMicAudioSerial, serial0b);
    EXPECT(vigem.lastMicAudioEnergy() > 10000.0);
}

static void test_micAudio_unboundSlotSubmitsNothing() {
    TEST("mic audio: an unbound slot decodes nothing, and rebinding starts clean");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    satellite::audio::OpusCodecFactory codecs;
    SessionService svc(vigem, client, log, {}, &codecs);

    auto r = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_DUALSENSE, CAP_MIC)});
    MicPacketSource mic;
    for (int i = 0; i < 5; i++) {
        const auto pkt = mic.next();
        EXPECT(svc.handleMicAudio(r.token, 0, (uint16_t)i, pkt.data(), pkt.size()));
    }
    EXPECT_EQ(vigem.submitMicAudioCalls, 5);

    // Unbind the slot. Frames that were already in flight must find nothing to
    // decode into: the codec state went with the pad.
    auto gone = upsert(svc, {});
    const auto strayPkt = mic.next();
    EXPECT(!svc.handleMicAudio(gone.token, 0, 5, strayPkt.data(), strayPkt.size()));
    EXPECT_EQ(vigem.submitMicAudioCalls, 5);

    // Rebinding gives a fresh stream, seq and all.
    auto back = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_DUALSENSE, CAP_MIC)});
    MicPacketSource fresh;
    const auto pkt = fresh.next();
    EXPECT(svc.handleMicAudio(back.token, 0, 0, pkt.data(), pkt.size()));
    EXPECT_EQ(vigem.submitMicAudioCalls, 6);
}

static void test_speakerAudio_oneWindowBecomesOneWirePacket() {
    TEST("speaker audio: a 20 ms window becomes one decodable Opus packet on the wire");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    satellite::audio::OpusCodecFactory codecs;
    SessionService svc(vigem, client, log, {}, &codecs);

    auto r = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_DUALSENSE, CAP_SPEAKER)});
    (void)r;
    const uint32_t serial0 = serialOfSlot(svc, 0);

    const auto pcm = speakerPcm(AUDIO_FRAME_SAMPLES, 0);
    EXPECT(svc.handleSpeakerAudioFromBackend(serial0, pcm.data(), AUDIO_FRAME_SAMPLES));
    EXPECT_EQ(client.speakerAudioCalls, 1);
    EXPECT_EQ((int)client.lastSpeakerAudioCtrlIdx, 0);
    EXPECT_EQ((int)client.lastSpeakerAudioSeq, 0);
    EXPECT(!client.lastSpeakerAudioOpus.empty());
    // Fits a datagram with room to spare, which is why the 1500-byte ceiling
    // absorbs a VBR spike instead of being approached.
    EXPECT(client.lastSpeakerAudioOpus.size() <
           (size_t)(MAX_INNER_PAYLOAD_BYTES - AUDIO_WIRE_HEADER_BYTES));

    // What went out is a real stereo 20 ms packet, not a length-shaped blob:
    // decoding it with the client's half of the codec gives the window back.
    auto dec = satellite::audio::OpusStreamDecoder::create(satellite::audio::Stream::Speaker);
    EXPECT(dec != nullptr);
    if (!dec) return;
    std::vector<int16_t> out(AUDIO_FRAME_SAMPLES * AUDIO_SPEAKER_CHANNELS, 0);
    EXPECT_EQ(dec->decode(client.lastSpeakerAudioOpus.data(), client.lastSpeakerAudioOpus.size(),
                          out.data(), AUDIO_FRAME_SAMPLES),
              (size_t)AUDIO_FRAME_SAMPLES);
}

static void test_speakerAudio_partialBatchesBuffer() {
    TEST("speaker audio: partial and multi-window batches are re-framed to exact 20 ms packets");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    satellite::audio::OpusCodecFactory codecs;
    SessionService svc(vigem, client, log, {}, &codecs);

    auto r = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_DUALSENSE, CAP_SPEAKER)});
    (void)r;
    const uint32_t serial0 = serialOfSlot(svc, 0);

    // A backend hands over whatever its ring held, which is a batch boundary,
    // not a codec boundary. Half a window is not a packet.
    const size_t half = AUDIO_FRAME_SAMPLES / 2;
    const auto firstHalf = speakerPcm(half, 0);
    EXPECT(svc.handleSpeakerAudioFromBackend(serial0, firstHalf.data(), half));
    EXPECT_EQ(client.speakerAudioCalls, 0);

    // The second half completes it: exactly one packet, not two short ones.
    const auto secondHalf = speakerPcm(half, 1);
    EXPECT(svc.handleSpeakerAudioFromBackend(serial0, secondHalf.data(), half));
    EXPECT_EQ(client.speakerAudioCalls, 1);
    EXPECT_EQ((int)client.lastSpeakerAudioSeq, 0);

    // Two and a half windows: two packets out, half a window carried forward.
    const size_t batch = AUDIO_FRAME_SAMPLES * 2 + half;
    const auto big = speakerPcm(batch, 2);
    EXPECT(svc.handleSpeakerAudioFromBackend(serial0, big.data(), batch));
    EXPECT_EQ(client.speakerAudioCalls, 3);
    EXPECT_EQ((int)client.lastSpeakerAudioSeq, 2);

    // ...and the carried half joins the next batch rather than being dropped.
    EXPECT(svc.handleSpeakerAudioFromBackend(serial0, secondHalf.data(), half));
    EXPECT_EQ(client.speakerAudioCalls, 4);
    EXPECT_EQ((int)client.lastSpeakerAudioSeq, 3);

    // A one-frame dribble is buffered, never emitted as a runt packet.
    const auto dribble = speakerPcm(1, 5);
    for (int i = 0; i < 16; i++) {
        EXPECT(svc.handleSpeakerAudioFromBackend(serial0, dribble.data(), 1));
    }
    EXPECT_EQ(client.speakerAudioCalls, 4);
}

static void test_speakerAudio_seqIsPerControllerAndWraps() {
    TEST("speaker audio: seq counts per controller and wraps through 0xFFFF");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    satellite::audio::OpusCodecFactory codecs;
    SessionService svc(vigem, client, log, {}, &codecs);

    auto r = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_DUALSENSE, CAP_SPEAKER),
                          makeDesc(1, CONTROLLER_TYPE_DUALSENSE, CAP_SPEAKER)});
    const uint32_t serial0 = serialOfSlot(svc, 0);
    const uint32_t serial1 = serialOfSlot(svc, 1);
    const auto pcm = speakerPcm(AUDIO_FRAME_SAMPLES, 0);

    for (int i = 0; i < 3; i++) {
        EXPECT(svc.handleSpeakerAudioFromBackend(serial0, pcm.data(), AUDIO_FRAME_SAMPLES));
        EXPECT_EQ((int)client.lastSpeakerAudioSeq, i);
    }
    // Slot 1 numbers its own stream: the client demuxes by ctrlIdx, so a shared
    // counter would make one pad's gaps look like the other's.
    EXPECT(svc.handleSpeakerAudioFromBackend(serial1, pcm.data(), AUDIO_FRAME_SAMPLES));
    EXPECT_EQ((int)client.lastSpeakerAudioCtrlIdx, 1);
    EXPECT_EQ((int)client.lastSpeakerAudioSeq, 0);
    EXPECT(svc.handleSpeakerAudioFromBackend(serial0, pcm.data(), AUDIO_FRAME_SAMPLES));
    EXPECT_EQ((int)client.lastSpeakerAudioCtrlIdx, 0);
    EXPECT_EQ((int)client.lastSpeakerAudioSeq, 3);

    // The wrap is 22 minutes of audio away, so it is reached through the test
    // seam rather than by encoding 65536 frames to get there.
    svc.setSpeakerSeqForTest(r.token, 0, 0xFFFE);
    const uint16_t expected[] = {0xFFFE, 0xFFFF, 0x0000, 0x0001};
    for (uint16_t want : expected) {
        EXPECT(svc.handleSpeakerAudioFromBackend(serial0, pcm.data(), AUDIO_FRAME_SAMPLES));
        EXPECT_EQ((int)client.lastSpeakerAudioSeq, (int)want);
    }
}

static void test_speakerAudio_encoderStateDiesWithThePad() {
    TEST("speaker audio: a replug resets the encoder, the seq and the buffered remainder");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    satellite::audio::OpusCodecFactory codecs;
    SessionService svc(vigem, client, log, {}, &codecs);

    upsert(svc, {makeDesc(0, CONTROLLER_TYPE_DUALSENSE, CAP_SPEAKER)});
    const uint32_t serial0 = serialOfSlot(svc, 0);
    const auto whole = speakerPcm(AUDIO_FRAME_SAMPLES, 0);
    for (int i = 0; i < 3; i++) {
        EXPECT(svc.handleSpeakerAudioFromBackend(serial0, whole.data(), AUDIO_FRAME_SAMPLES));
    }
    EXPECT_EQ(client.speakerAudioCalls, 3);
    EXPECT_EQ((int)client.lastSpeakerAudioSeq, 2);

    // Leave half a window buffered, then replug.
    const size_t half = AUDIO_FRAME_SAMPLES / 2;
    const auto tail = speakerPcm(half, 3);
    EXPECT(svc.handleSpeakerAudioFromBackend(serial0, tail.data(), half));
    EXPECT_EQ(client.speakerAudioCalls, 3);

    upsert(svc, {makeDesc(0, CONTROLLER_TYPE_XBOX, CAP_SPEAKER)});
    upsert(svc, {makeDesc(0, CONTROLLER_TYPE_DUALSENSE, CAP_SPEAKER)});
    const uint32_t serial0b = serialOfSlot(svc, 0);
    EXPECT(serial0b != serial0);

    // Half a window from the NEW pad must not be completed by half a window
    // from the old one: that would splice two different streams into one packet.
    EXPECT(svc.handleSpeakerAudioFromBackend(serial0b, tail.data(), half));
    EXPECT_EQ(client.speakerAudioCalls, 3);
    EXPECT(svc.handleSpeakerAudioFromBackend(serial0b, tail.data(), half));
    EXPECT_EQ(client.speakerAudioCalls, 4);
    // Fresh pad, fresh numbering: seq back to 0 is the observable proof that
    // the whole audio state (encoder included) went with the old pad.
    EXPECT_EQ((int)client.lastSpeakerAudioSeq, 0);
}

static void test_speakerAudio_withoutCodecSendsNothing() {
    TEST("speaker audio: with no codec wired the gate still runs and nothing is sent");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log); // no factory

    upsert(svc, {makeDesc(0, CONTROLLER_TYPE_DUALSENSE, CAP_SPEAKER)});
    const uint32_t serial0 = serialOfSlot(svc, 0);
    const auto pcm = speakerPcm(AUDIO_FRAME_SAMPLES, 0);
    EXPECT(svc.handleSpeakerAudioFromBackend(serial0, pcm.data(), AUDIO_FRAME_SAMPLES));
    EXPECT_EQ(client.speakerAudioCalls, 0);
}

static void test_audioBackendCallbacks_dropNotBlock_whenLockHeld() {
    TEST("mic LED / speaker: backend callbacks drop (never block) while mtx_ is held");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto rA =
        upsert(svc, {makeDesc(0, CONTROLLER_TYPE_DUALSENSE, CAP_MIC | CAP_SPEAKER)}, "devA", "A");
    (void)rA;
    const uint32_t serialA = vigem.pluggedSerials.back();
    auto rB = upsert(svc, {}, "devB", "B");

    std::vector<int16_t> pcm(AUDIO_FRAME_SAMPLES * AUDIO_SPEAKER_CHANNELS, 0);
    // Same production bug the rumble/lightbar test pins: unplug joins the
    // notification worker while holding mtx_, so a blocking acquire in a
    // backend callback deadlocks this join.
    client.onSessionClose = [&] {
        std::thread worker([&] {
            vigem.fireMicLed(serialA, MIC_LED_STATE_ON);
            vigem.fireSpeakerAudio(serialA, pcm.data(), AUDIO_FRAME_SAMPLES);
        });
        worker.join();
    };
    svc.closeSessionById(rB.connectionId, "", CLOSE_REASON_KICKED, /*notify=*/true);
    client.onSessionClose = nullptr;
    EXPECT_EQ(client.micLedCalls, 0); // dropped, not delivered

    // The drop didn't poison the coalesce cache: the same value re-notified
    // with the lock free goes out (self-heal).
    vigem.fireMicLed(serialA, MIC_LED_STATE_ON);
    EXPECT_EQ(client.micLedCalls, 1);
}

static void test_feedback_replugResetsCoalesce() {
    TEST("replug: trigger/LED coalesce state resets, so the same value re-forwards");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    const uint16_t caps = CAP_TRIGGER_EFFECTS | CAP_PLAYER_LEDS | CAP_MIC;
    auto r = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_DUALSENSE, caps)});
    (void)r;
    uint32_t serial = vigem.pluggedSerials.back();
    const TriggerEffectsReport fx = makeTriggerEffects(0x21, 0x26);
    vigem.fireTriggerEffects(serial, fx);
    vigem.firePlayerLeds(serial, 0x1F);
    vigem.fireMicLed(serial, MIC_LED_STATE_ON);
    EXPECT_EQ(client.triggerEffectsCalls, 1);
    EXPECT_EQ(client.playerLedsCalls, 1);
    EXPECT_EQ(client.micLedCalls, 1);

    // Identity change forces a replug; the fresh pad must not have the old
    // values suppressed as "same as last".
    upsert(svc, {makeDesc(0, CONTROLLER_TYPE_XBOX, caps)});
    upsert(svc, {makeDesc(0, CONTROLLER_TYPE_DUALSENSE, caps)});
    uint32_t serial2 = vigem.pluggedSerials.back();
    vigem.fireTriggerEffects(serial2, fx);
    vigem.firePlayerLeds(serial2, 0x1F);
    vigem.fireMicLed(serial2, MIC_LED_STATE_ON);
    EXPECT_EQ(client.triggerEffectsCalls, 2);
    EXPECT_EQ(client.playerLedsCalls, 2);
    EXPECT_EQ(client.micLedCalls, 2);
}

static void test_backendCallbacks_dropNotBlock_whenLockHeld() {
    TEST("rumble/lightbar: backend callbacks drop (never block) while mtx_ is held");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    auto rA = upsert(svc, {makeDesc(0, CONTROLLER_TYPE_PLAYSTATION, CAP_RUMBLE | CAP_LIGHTBAR)},
                     "devA", "A");
    uint32_t serialA = vigem.pluggedSerials.back();
    auto rB = upsert(svc, {}, "devB", "B");
    (void)rA;

    RumbleReport rr{};
    rr.strongMagnitude = 1000;
    // onSessionClose fires while the closing thread holds mtx_. A worker
    // thread invoking the backend callbacks then MUST fail its try_lock and
    // return; a blocking acquire would deadlock this join (the production bug:
    // unplug joins the notification worker while holding mtx_).
    client.onSessionClose = [&] {
        std::thread worker([&] {
            vigem.fireRumble(serialA, rr);
            vigem.fireLightbar(serialA, 1, 2, 3);
        });
        worker.join();
    };
    svc.closeSessionById(rB.connectionId, "", CLOSE_REASON_KICKED, /*notify=*/true);
    client.onSessionClose = nullptr;
    EXPECT_EQ(client.rumbleCalls, 0); // dropped, not delivered
    EXPECT_EQ(client.lightbarCalls, 0);

    // The drop didn't poison the coalesce caches: the same values re-notified
    // with the lock free go out (self-heal).
    vigem.fireRumble(serialA, rr);
    vigem.fireLightbar(serialA, 1, 2, 3);
    EXPECT_EQ(client.rumbleCalls, 1);
    EXPECT_EQ(client.lightbarCalls, 1);
}

static void test_concurrent_upsertCloseSnapshot() {
    TEST("concurrency: upsert vs unpair-close vs snapshot races don't corrupt state");
    MockViGem vigem;
    MockClient client;
    MockLog log;
    SessionService svc(vigem, client, log);

    std::thread t1([&] {
        for (int i = 0; i < 200; i++) {
            svc.upsertSession("devA", "A", "1.1.1.1", TEST_KEY, {makeDesc(0)}, false);
        }
    });
    std::thread t2([&] {
        for (int i = 0; i < 200; i++) { svc.closeSessionsForDevice("devA", CLOSE_REASON_UNPAIRED); }
    });
    std::thread t3([&] {
        for (int i = 0; i < 200; i++) { (void)svc.getConnectionsSnapshot(); }
    });
    t1.join();
    t2.join();
    t3.join();
    svc.closeAllSessions();
    EXPECT_EQ(svc.totalActiveControllers(), 0);
    EXPECT_EQ(svc.availableSlots(), 16);
}

static void test_controllerTypeHelpers() {
    TEST("controller type helpers: names, labels, identities, feature surfaces");
    EXPECT_EQ((int)CONTROLLER_TYPE_COUNT, 4);

    EXPECT_EQ(std::string(controllerTypeName(CONTROLLER_TYPE_XBOX)), std::string("xbox"));
    EXPECT_EQ(std::string(controllerTypeName(CONTROLLER_TYPE_PLAYSTATION)),
              std::string("playstation"));
    EXPECT_EQ(std::string(controllerTypeName(CONTROLLER_TYPE_DUALSENSE)), std::string("dualsense"));
    EXPECT_EQ(std::string(controllerTypeName(CONTROLLER_TYPE_SWITCHPRO)), std::string("switchpro"));
    EXPECT_EQ(std::string(controllerTypeName(255)), std::string("xbox"));

    EXPECT_EQ(std::string(controllerTypeLabel(CONTROLLER_TYPE_XBOX)), std::string("Xbox"));
    EXPECT_EQ(std::string(controllerTypeLabel(CONTROLLER_TYPE_PLAYSTATION)),
              std::string("PlayStation"));
    EXPECT_EQ(std::string(controllerTypeLabel(CONTROLLER_TYPE_DUALSENSE)),
              std::string("DualSense"));
    EXPECT_EQ(std::string(controllerTypeLabel(CONTROLLER_TYPE_SWITCHPRO)),
              std::string("Switch Pro"));
    EXPECT_EQ(std::string(controllerTypeLabel(255)), std::string("Xbox"));

    // Materialization identity per type (drives plug/submit selection + the
    // replug-on-change gate). Unknown ids clamp to Xbox.
    EXPECT(controllerIdentity(CONTROLLER_TYPE_XBOX) == GamepadIdentity::Xbox);
    EXPECT(controllerIdentity(CONTROLLER_TYPE_PLAYSTATION) == GamepadIdentity::DS4);
    EXPECT(controllerIdentity(CONTROLLER_TYPE_DUALSENSE) == GamepadIdentity::DualSense);
    EXPECT(controllerIdentity(CONTROLLER_TYPE_SWITCHPRO) == GamepadIdentity::SwitchPro);
    EXPECT(controllerIdentity(255) == GamepadIdentity::Xbox);

    // Feature surfaces, independent of identity: Switch Pro has an IMU but no
    // touchpad; Xbox has neither.
    EXPECT(!controllerTypeHasMotion(CONTROLLER_TYPE_XBOX));
    EXPECT(controllerTypeHasMotion(CONTROLLER_TYPE_PLAYSTATION));
    EXPECT(controllerTypeHasMotion(CONTROLLER_TYPE_DUALSENSE));
    EXPECT(controllerTypeHasMotion(CONTROLLER_TYPE_SWITCHPRO));
    EXPECT(controllerTypeHasTouchpad(CONTROLLER_TYPE_PLAYSTATION));
    EXPECT(controllerTypeHasTouchpad(CONTROLLER_TYPE_DUALSENSE));
    EXPECT(!controllerTypeHasTouchpad(CONTROLLER_TYPE_XBOX));
    EXPECT(!controllerTypeHasTouchpad(CONTROLLER_TYPE_SWITCHPRO));
}

static void test_applyResultNames() {
    TEST("apply result names: protocol constants, machine-readable");
    EXPECT_EQ(std::string(applyResultName(APPLY_OK)), std::string("ok"));
    EXPECT_EQ(std::string(applyResultName(APPLY_ERR_NO_SLOTS)), std::string("noSlots"));
    EXPECT_EQ(std::string(applyResultName(APPLY_ERR_PLUGIN_FAIL)), std::string("pluginFailed"));
    EXPECT_EQ(std::string(applyResultName(APPLY_ERR_REPLUG_FAIL)), std::string("replugFailed"));
    EXPECT_EQ(std::string(applyResultName(APPLY_ERR_BACKEND_UNAVAIL)),
              std::string("backendUnavailable"));
    EXPECT_EQ(std::string(applyResultName(APPLY_ERR_INVALID_TYPE)), std::string("invalidType"));
    EXPECT_EQ(std::string(applyResultName(APPLY_ERR_INVALID_INDEX)), std::string("invalidIndex"));
}

static void test_closeReasonNames() {
    TEST("close reason names: protocol constants");
    EXPECT_EQ(std::string(closeReasonName(CLOSE_REASON_SHUTDOWN)), std::string("shutdown"));
    EXPECT_EQ(std::string(closeReasonName(CLOSE_REASON_KICKED)), std::string("kicked"));
    EXPECT_EQ(std::string(closeReasonName(CLOSE_REASON_REPLACED)), std::string("replaced"));
    EXPECT_EQ(std::string(closeReasonName(CLOSE_REASON_UNPAIRED)), std::string("unpaired"));
}

static void test_capabilityBits() {
    TEST("capability bits: distinct powers of two; the audio accessors read their own bit");
    // A reused or overlapping bit would silently grant a stream the sender
    // never advertised, so pin the whole word, not just the new entries.
    const uint16_t all[] = {CAP_ANALOG_TRIGGERS, CAP_RUMBLE,      CAP_MOTION, CAP_LIGHTBAR,
                            CAP_TRIGGER_EFFECTS, CAP_PLAYER_LEDS, CAP_MIC,    CAP_SPEAKER};
    uint16_t seen = 0;
    for (uint16_t bit : all) {
        EXPECT((bit & (uint16_t)(bit - 1)) == 0); // exactly one bit set
        EXPECT((seen & bit) == 0);                // never reused
        seen |= bit;
    }
    EXPECT_EQ((int)CAP_MIC, 0x0040);
    EXPECT_EQ((int)CAP_SPEAKER, 0x0080);

    Controller c;
    c.caps = CAP_MIC;
    EXPECT(c.micCapable());
    EXPECT(!c.speakerCapable());
    c.caps = CAP_SPEAKER;
    EXPECT(!c.micCapable());
    EXPECT(c.speakerCapable());
    c.caps = CAP_MIC | CAP_SPEAKER;
    EXPECT(c.micCapable());
    EXPECT(c.speakerCapable());
    // Neighbouring caps must not bleed into the audio accessors.
    c.caps = CAP_LIGHTBAR | CAP_TRIGGER_EFFECTS | CAP_PLAYER_LEDS;
    EXPECT(!c.micCapable());
    EXPECT(!c.speakerCapable());

    // The mic-mute button is the one free bit in the XINPUT-shaped word; it
    // must not collide with Guide (0x0400), which the identity encoders map.
    // A compile-time invariant, so it fails the build rather than a test run.
    static_assert((WBUTTON_MIC_MUTE & 0x0400) == 0,
                  "WBUTTON_MIC_MUTE must not collide with the XUSB Guide bit");
    EXPECT_EQ((int)WBUTTON_MIC_MUTE, 0x0800);
    GamepadReport g;
    g.wButtons = WBUTTON_MIC_MUTE;
    EXPECT_EQ((int)g.wButtons, 0x0800);
}

static void test_wireConstants() {
    TEST("wire constants: remaining opcodes + close reasons + protocol version");
    EXPECT_EQ((int)MSG_GAMEPAD_DATA, 0x0001);
    EXPECT_EQ((int)MSG_HEARTBEAT_PING, 0x0002);
    EXPECT_EQ((int)MSG_HEARTBEAT_ACK, 0x0003);
    EXPECT_EQ((int)MSG_RUMBLE, 0x0009);
    EXPECT_EQ((int)MSG_MOTION, 0x000A);
    EXPECT_EQ((int)MSG_BATTERY, 0x000B);
    EXPECT_EQ((int)MSG_TOUCHPAD, 0x000C);
    EXPECT_EQ((int)MSG_LIGHTBAR, 0x000D);
    EXPECT_EQ((int)MSG_SESSION_CLOSE, 0x000F);
    EXPECT_EQ((int)MSG_TRIGGER_EFFECTS, 0x0010);
    EXPECT_EQ((int)MSG_PLAYER_LEDS, 0x0011);
    EXPECT_EQ((int)MSG_MIC_AUDIO, 0x0012);
    EXPECT_EQ((int)MSG_SPEAKER_AUDIO, 0x0013);
    EXPECT_EQ((int)MSG_MIC_LED, 0x0014);
    // Protocol 2 is extended in place: the audio messages must NOT move it.
    EXPECT_EQ(PROTOCOL_VERSION, 2);
    EXPECT_EQ(PROTOCOL_VERSION_MIN, 1);
    EXPECT_EQ(SESSION_SALT_SIZE, 8);
    EXPECT_EQ((int)CRYPTO_DIR_CLIENT_TO_SERVER, 0);
    EXPECT_EQ((int)CRYPTO_DIR_SERVER_TO_CLIENT, 1);
}

int main() {
    test_upsert_createsSession();
    test_upsert_recordsProtocolVersion();
    test_upsert_zeroControllerSessionIsValid();
    test_upsert_idempotent_stableConnectionId_rotatingToken();
    test_upsert_rotation_notifiesOldTokenReplaced();
    test_upsert_saltAndCounterRotate();
    test_upsert_keyDeriverIsUsed();
    test_upsert_defaultDeriverCopiesPairingKey();
    test_upsert_keyCopiedNotAliased();
    test_upsert_firstPlugCarriesFinalType();
    test_upsert_convergeRemovesAbsentSlots();
    test_upsert_convergeToZeroKeepsSession();
    test_upsert_addsNewSlots();
    test_upsert_duplicateIdxLastWins();
    test_upsert_invalidTypeReported();
    test_upsert_invalidIndexReported();
    test_upsert_backendUnavailable();
    test_upsert_pluginFailReleasesSerial();
    test_upsert_noSlots();
    test_upsert_partialSuccess();

    test_converge_typeFamilyChange_transactionalReplug();
    test_converge_replugFailure_keepsOldPad();
    test_converge_fullBus_fallsBackToUnplugFirst();
    test_converge_capsAndModeChange_noReplug();
    test_converge_sameDescriptorIsNoOp();

    test_upsert_dualSensePlugsWithDualSenseIdentity();
    test_upsert_switchProPlugsMotionNoTouchpad();
    test_converge_identityChangeReplugs_capsOnlyDoesNot();
    test_apply_unsupportedIdentityRejectedPriorPadIntact();

    test_epoch_bumpsOnTopologyChanges();
    test_epoch_bumpsOnFailedReplug();
    test_heartbeat_enrichedAck();
    test_heartbeat_invalidTokenIsNoOp();
    test_heartbeat_reflectsInvoluntaryLoss();

    test_applyController_standalone();
    test_applyController_scopedToOwner();
    test_removeController_slotOnly();
    test_sessionView_scopedAndComplete();

    test_mouseControl_grantedWhenSupported();
    test_mouseControl_deniedWhenUnsupported();
    test_mouseControl_notRequestedNotGranted();
    test_touchpad_mouseStreamDroppedWithoutGrant();
    test_mouseControl_rePutWithoutRequestRevokes();

    test_serials_roundRobinAvoidsInstantReuse();
    test_serials_quarantineOnUnplugFailure();

    test_closeById_kick_notifiesBeforeTeardown();
    test_closeById_clientCloseNoNotify();
    test_closeById_scopeAndNotFound();
    test_closeForDevice_unpairClosesLiveSession();
    test_closeAll_broadcastsShutdownFirst();

    test_reap_honoursRestGraceWindow();
    test_reap_firesAfterGraceExpires();
    test_reap_recentPacketsKeepAlive();
    test_linkState_graceCountsAsActive();
    test_snapshot_notRespondingState();

    test_snapshot_pluggedInFromAdapterTruth();
    test_snapshot_carriesIdentityAndEpoch();

    test_gamepadData_routesByType();
    test_gamepadData_guards();
    test_fusedHotPath();
    test_fusedHotPath_guards();
    test_motionData_cachedAndForwarded();
    test_motionData_acceptedWithoutCapAndCachedOnDecline();
    test_battery_validation();
    test_touchpad_ds4ModeRoutesToPad();
    test_touchpad_offModeCachesOnly();
    test_touchpad_mouseMode_deltaAndAnchoring();
    test_touchpad_mouseMode_dtScaling();
    test_touchpad_mouseMode_duplicateTimestampNoMotion();
    test_touchpad_mouseMode_gapReanchors();
    test_touchpad_mouseMode_buttonLevel();
    test_touchpad_mouseMode_clickWithoutTouch();
    test_touchpad_mouseMode_extendedButtonsAndWheel();
    test_touchpad_mouseMode_wheelDuplicateSuppressed();
    test_touchpad_mouseMode_subPixelRemainder();
    test_replug_resetsStreamCaches();
    test_rumble_forwardsAndCoalesces();
    test_lightbar_gatedOnCap();
    test_triggerEffects_gatedOnCap();
    test_playerLeds_gatedOnCap();
    test_micAudio_gatedOnCap();
    test_micAudio_unknownControllerAndToken();
    test_micAudio_rateLimitIsPerController();
    test_micAudio_replugRestoresAllowance();
    test_micAudio_dropLoggedOncePerSessionPerCause();
    test_micLed_gatedOnCapAndCoalesced();
    test_speakerAudio_gatedOnCap();
    test_speakerAudio_exactSilenceIsNotSent();
    test_speakerAudio_anythingAudibleStillGoes();
    test_audioPolicy_defaultsToBothOn();
    test_audioPolicy_speakerOffStopsTheStream();
    test_audioPolicy_micOffDropsWithAReason();
    test_audioPolicy_directionsAreIndependent();
    test_micAudio_decodesRealPacketsToTheBackend();
    test_micAudio_withoutCodecAcceptsAndDrops();
    test_micAudio_gapConcealedAndReorderHealed();
    test_micAudio_streamsPastTheRateWindow();
    test_micAudio_seqWrapsMidStream();
    test_micAudio_codecStateIsPerControllerAndDiesWithThePad();
    test_micAudio_unboundSlotSubmitsNothing();
    test_speakerAudio_oneWindowBecomesOneWirePacket();
    test_speakerAudio_partialBatchesBuffer();
    test_speakerAudio_seqIsPerControllerAndWraps();
    test_speakerAudio_encoderStateDiesWithThePad();
    test_speakerAudio_withoutCodecSendsNothing();
    test_audioBackendCallbacks_dropNotBlock_whenLockHeld();
    test_feedback_replugResetsCoalesce();
    test_backendCallbacks_dropNotBlock_whenLockHeld();

    test_concurrent_upsertCloseSnapshot();

    test_controllerTypeHelpers();
    test_applyResultNames();
    test_closeReasonNames();
    test_capabilityBits();
    test_wireConstants();

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
