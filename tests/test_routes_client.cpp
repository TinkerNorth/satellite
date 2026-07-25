// SPDX-License-Identifier: LGPL-3.0-or-later
// In-process route tests for the sender-facing client API (routes_client.cpp):
// clientAuthed accept/reject (missing/invalid proof, unknown device, body-key
// fallback), the terminal-401 machine codes (NOT_PAIRED / BAD_PROOF), the
// protocol-version 409 path, pairing paths A (server PIN, with and without a
// client X25519 key) and B (dish PIN + status poll), key rotation, self-unpair,
// and the session/controller CRUD surface.
//
// The routes are registered on a plain loopback httplib::Server (production
// wraps the same table in an SSLServer; route behavior is transport-
// independent) and driven with httplib::Client. SessionService runs against
// bare port stubs, so no virtual-gamepad backend is needed. Hermeticity: HOME
// and XDG_CONFIG_HOME are redirected into a per-run tmpdir, so saveConfig()
// inside the pairing routes never touches the real user profile.
#include "../src/core/json.h"
#include "../src/core/session_service.h"
#include "../src/net/pairing_service.h"
#include "../src/net/routes_client.h"
#include "../src/net/session_crypto.h"
#include "config.h" // bare platform seam: resolved per-OS by the test target
#include "crypto.h"

#include <sodium.h>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include "test_util.h"

using satellite::Json;
using satellite::jsonBool;
using satellite::jsonInt;
using satellite::jsonParse;
using satellite::jsonStr;

// Per-run tmp dir as HOME + XDG_CONFIG_HOME (macOS config lives under
// ~/Library/..., Linux under $XDG_CONFIG_HOME), so route-triggered
// saveConfig() calls land in the sandbox.
struct TempHome {
    std::string path;
    TempHome() {
        char tmpl[] = "/tmp/satellite-routes-XXXXXX";
        char* d = mkdtemp(tmpl);
        path = (d != nullptr) ? d : "/tmp";
        setenv("HOME", path.c_str(), 1);
        setenv("XDG_CONFIG_HOME", path.c_str(), 1);
    }
    ~TempHome() {
        if (path.rfind("/tmp/satellite-routes-", 0) == 0) {
            std::string cmd = "rm -rf " + path;
            int rc = system(cmd.c_str());
            (void)rc;
        }
    }
};

// Bare port stubs (same shape as test_receiver.cpp): plug/submit succeed so
// descriptor applies report ok; nothing touches a real backend.
struct StubGamepad : IGamepadPort {
    bool ensureBusOpen() override { return true; }
    void closeBus() override {}
    bool isBusOpen() const override { return true; }
    bool pluginDevice(uint32_t, GamepadIdentity) override { return true; }
    bool supportsIdentity(GamepadIdentity) const override { return true; }
    bool unplugDevice(uint32_t) override { return true; }
    bool submitReport(uint32_t, const GamepadReport&) override { return true; }
    void setRumbleCallback(RumbleCallback) override {}
};

struct StubClient : IClientPort {
    void updateClientAddr(uint32_t, const std::string&, uint16_t) override {}
    void removeClientAddr(uint32_t) override {}
    void sendHeartbeatAck(const Connection&, bool, uint8_t, uint16_t, uint16_t) override {}
    void sendSessionClose(const Connection&, uint8_t) override { closeNotifies++; }
    void sendRumble(const Connection&, uint8_t, const RumbleReport&) override {}
    void sendLightbar(const Connection&, uint8_t, uint8_t, uint8_t, uint8_t) override {}
    int closeNotifies = 0;
};

struct StubLog : ILogPort {
    void logMsg(LogLevel, const std::string&, const std::string&) override {}
};

static Json parseJson(const std::string& body) {
    Json j;
    if (!jsonParse(body, j)) return Json();
    return j;
}

static bool isHex(const std::string& s, size_t chars) {
    return s.size() == chars && s.find_first_not_of("0123456789abcdef") == std::string::npos;
}

static void putPairedDevice(const std::string& id, const std::string& keyHex) {
    PairedDevice d;
    d.id = id;
    d.name = "RouteTester";
    d.lastIP = "127.0.0.1";
    d.pairedAt = "2026-07-16";
    d.sharedKeyHex = keyHex;
    std::lock_guard<std::mutex> lk(g_configMtx);
    g_config.pairedDevices.push_back(d);
}

static std::string storedKeyHex(const std::string& id) {
    std::lock_guard<std::mutex> lk(g_configMtx);
    for (const auto& d : g_config.pairedDevices) {
        if (d.id == id) return d.sharedKeyHex;
    }
    return "";
}

// Returns a 4-digit PIN guaranteed != both live PINs (same construction as the
// platform suites).
static std::string wrongPinFor(const PinSnapshot& s) {
    std::string w = s.currentPin;
    for (int step = 1; step <= 2; step++) {
        w[0] = static_cast<char>('0' + ((s.currentPin[0] - '0') + step) % 10);
        if (w != s.previousPin) return w;
    }
    return w;
}

int main() {
    std::cout << "Running client API route tests...\n\n";

    if (!sodiumInit()) {
        std::cerr << "sodium init failed\n";
        return 1;
    }

    TempHome home;
    g_appRunning = true;

    StubGamepad gamepad;
    StubClient clientPort;
    StubLog log;
    SessionService svc(gamepad, clientPort, log, deriveSessionKey);

    httplib::Server server;
    registerClientRoutes(server, svc);
    int port = server.bind_to_any_port("127.0.0.1");
    if (port <= 0) {
        std::cerr << "failed to bind loopback port\n";
        return 1;
    }
    std::thread serverThread([&server] { server.listen_after_bind(); });
    server.wait_until_ready();

    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(5);
    cli.set_read_timeout(5);

    // Paired identity used throughout.
    const std::string devId = "dev-routes-1";
    uint8_t pairingKey[CRYPTO_KEY_SIZE];
    for (int i = 0; i < CRYPTO_KEY_SIZE; i++) pairingKey[i] = static_cast<uint8_t>(0xA0 ^ i);
    putPairedDevice(devId, hexEncode(pairingKey, CRYPTO_KEY_SIZE));
    const std::string proof = computeHmacProof(pairingKey, devId);
    const httplib::Headers auth = {{"X-Device-Id", devId}, {"X-Hmac-Proof", proof}};

    // ---- clientAuthed: terminal-401 machine codes --------------------------
    {
        TEST("PUT /api/connections with no credentials: 401 NOT_PAIRED");
        auto res = cli.Put("/api/connections", "{}", "application/json");
        EXPECT(res && res->status == 401);
        if (res) {
            Json j = parseJson(res->body);
            EXPECT_EQ(jsonStr(j, "error"), std::string("unauthorized"));
            EXPECT_EQ(jsonStr(j, "code"), std::string("NOT_PAIRED"));
        }
    }
    {
        TEST("unknown deviceId: 401 NOT_PAIRED (never BAD_PROOF for strangers)");
        httplib::Headers h = {{"X-Device-Id", "dev-unknown"}, {"X-Hmac-Proof", proof}};
        auto res = cli.Put("/api/connections", h, "{}", "application/json");
        EXPECT(res && res->status == 401);
        if (res) EXPECT_EQ(jsonStr(parseJson(res->body), "code"), std::string("NOT_PAIRED"));
    }
    {
        TEST("known device, malformed proof: 401 BAD_PROOF");
        httplib::Headers h = {{"X-Device-Id", devId}, {"X-Hmac-Proof", "zz-not-hex"}};
        auto res = cli.Put("/api/connections", h, "{}", "application/json");
        EXPECT(res && res->status == 401);
        if (res) EXPECT_EQ(jsonStr(parseJson(res->body), "code"), std::string("BAD_PROOF"));
    }
    {
        TEST("known device, proof of the WRONG key: 401 BAD_PROOF");
        uint8_t otherKey[CRYPTO_KEY_SIZE] = {};
        httplib::Headers h = {{"X-Device-Id", devId},
                              {"X-Hmac-Proof", computeHmacProof(otherKey, devId)}};
        auto res = cli.Put("/api/connections", h, "{}", "application/json");
        EXPECT(res && res->status == 401);
        if (res) EXPECT_EQ(jsonStr(parseJson(res->body), "code"), std::string("BAD_PROOF"));
    }
    {
        TEST("GET /api/connections/:id is authed too: 401 without credentials");
        auto res = cli.Get("/api/connections/conn_00000000");
        EXPECT(res && res->status == 401);
    }

    // ---- protocol-version gate ---------------------------------------------
    {
        TEST("PUT /api/connections with protocolVersion 2: 409 + supported echo");
        auto res =
            cli.Put("/api/connections", auth, R"({"protocolVersion":2})", "application/json");
        EXPECT(res && res->status == 409);
        if (res) {
            Json j = parseJson(res->body);
            EXPECT_EQ(jsonInt(j, "supported"), static_cast<long>(PROTOCOL_VERSION));
        }
    }
    {
        TEST("absent protocolVersion is accepted (legacy-lenient)");
        auto res = cli.Put("/api/connections", auth, "{}", "application/json");
        EXPECT(res && res->status == 200);
    }

    // ---- session upsert + reconcile + CRUD ----------------------------------
    std::string connId;
    {
        TEST("authed PUT /api/connections: 200 with token/salt/connectionId");
        const std::string body = R"({
            "protocolVersion": 1,
            "deviceName": "Route Tester",
            "controllers": [
                {"ctrlIdx": 0, "type": 0, "caps": {"rumble": true, "analogTriggers": true}},
                {"ctrlIdx": 1, "type": 1, "caps": {"motion": true, "lightbar": true},
                 "touchpadMode": "ds4"}
            ]
        })";
        auto res = cli.Put("/api/connections", auth, body, "application/json");
        EXPECT(res && res->status == 200);
        if (res) {
            Json j = parseJson(res->body);
            connId = jsonStr(j, "connectionId");
            EXPECT_EQ(connId.rfind("conn_", 0), size_t{0});
            EXPECT(isHex(jsonStr(j, "token"), 8));
            EXPECT(isHex(jsonStr(j, "sessionSalt"), SESSION_SALT_SIZE * 2));
            EXPECT_EQ(jsonInt(j, "protocolVersion"), static_cast<long>(PROTOCOL_VERSION));
            EXPECT_EQ(jsonInt(j, "maxControllers"), static_cast<long>(MAX_BACKEND_CONTROLLERS));
            EXPECT(j.contains("controllers") && j["controllers"].is_array() &&
                   j["controllers"].size() == 2);
            if (j.contains("controllers") && j["controllers"].size() == 2) {
                EXPECT_EQ(jsonStr(j["controllers"][0], "result"), std::string("ok"));
                EXPECT_EQ(jsonInt(j["controllers"][1], "ctrlIdx"), 1L);
            }
            EXPECT(j.contains("hostFeatures"));
            if (j.contains("hostFeatures")) {
                EXPECT_EQ(jsonBool(j["hostFeatures"]["mouseControl"], "granted"), false);
            }
        }
    }
    std::string firstToken;
    {
        TEST("re-PUT converges in place: same connectionId, rotated token");
        auto res1 = cli.Put("/api/connections", auth, R"({"controllers":[{"ctrlIdx":0,"type":0}]})",
                            "application/json");
        EXPECT(res1 && res1->status == 200);
        if (res1) firstToken = jsonStr(parseJson(res1->body), "token");
        auto res2 = cli.Put("/api/connections", auth, R"({"controllers":[{"ctrlIdx":0,"type":0}]})",
                            "application/json");
        EXPECT(res2 && res2->status == 200);
        if (res1 && res2) {
            Json j2 = parseJson(res2->body);
            EXPECT_EQ(jsonStr(j2, "connectionId"), connId);
            EXPECT(jsonStr(j2, "token") != firstToken);
        }
    }
    {
        TEST("malformed controllers array (missing type): 400");
        auto res = cli.Put("/api/connections", auth, R"({"controllers":[{"ctrlIdx":0}]})",
                           "application/json");
        EXPECT(res && res->status == 400);
    }
    {
        TEST("auth via body keys (deviceId/hmacProof) works without headers");
        const std::string body =
            std::string(R"({"deviceId":")") + devId + R"(","hmacProof":")" + proof + R"("})";
        auto res = cli.Put("/api/connections", body, "application/json");
        EXPECT(res && res->status == 200);
    }
    {
        TEST("GET /api/connections/:id (reconcile view): applied state echoed");
        auto res = cli.Put("/api/connections", auth,
                           R"({"controllers":[{"ctrlIdx":3,"type":1,"caps":{"motion":true},
                               "touchpadMode":"ds4"}]})",
                           "application/json");
        EXPECT(res && res->status == 200);
        auto view = cli.Get(("/api/connections/" + connId).c_str(), auth);
        EXPECT(view && view->status == 200);
        if (view) {
            Json j = parseJson(view->body);
            EXPECT_EQ(jsonStr(j, "connectionId"), connId);
            EXPECT_EQ(jsonStr(j, "deviceId"), devId);
            EXPECT(j.contains("controllers") && j["controllers"].is_array() &&
                   j["controllers"].size() == 1);
            if (j.contains("controllers") && j["controllers"].size() == 1) {
                const Json& c = j["controllers"][0];
                EXPECT_EQ(jsonInt(c, "ctrlIdx"), 3L);
                EXPECT_EQ(jsonStr(c, "touchpadMode"), std::string("ds4"));
                EXPECT_EQ(jsonBool(c["caps"], "motion"), true);
                EXPECT_EQ(jsonBool(c["caps"], "rumble"), false);
            }
        }
    }
    {
        TEST("GET /api/connections/:id for an unknown id: 404");
        auto res = cli.Get("/api/connections/conn_ffffffff", auth);
        EXPECT(res && res->status == 404);
    }
    {
        TEST("per-slot PUT: full descriptor upsert, path index wins");
        auto res = cli.Put(("/api/connections/" + connId + "/controllers/5").c_str(), auth,
                           R"({"type":0,"caps":{"rumble":true}})", "application/json");
        EXPECT(res && res->status == 200);
        if (res) {
            Json j = parseJson(res->body);
            EXPECT(j.contains("epoch"));
            EXPECT(j.contains("controller"));
            if (j.contains("controller")) {
                EXPECT_EQ(jsonInt(j["controller"], "ctrlIdx"), 5L);
                EXPECT_EQ(jsonStr(j["controller"], "result"), std::string("ok"));
            }
        }
    }
    {
        TEST("per-slot PUT without type: 400");
        auto res = cli.Put(("/api/connections/" + connId + "/controllers/5").c_str(), auth,
                           R"({"caps":{"rumble":true}})", "application/json");
        EXPECT(res && res->status == 400);
    }
    {
        TEST("per-slot PUT with protocolVersion 2: 409");
        auto res = cli.Put(("/api/connections/" + connId + "/controllers/5").c_str(), auth,
                           R"({"type":0,"protocolVersion":2})", "application/json");
        EXPECT(res && res->status == 409);
    }
    {
        TEST("per-slot DELETE removes the slot, session lives on");
        auto res = cli.Delete(("/api/connections/" + connId + "/controllers/5").c_str(), auth);
        EXPECT(res && res->status == 200);
        if (res) EXPECT(jsonBool(parseJson(res->body), "ok"));
        auto view = cli.Get(("/api/connections/" + connId).c_str(), auth);
        EXPECT(view && view->status == 200);
    }
    {
        TEST("per-slot DELETE on an unknown connection: 404");
        auto res = cli.Delete("/api/connections/conn_ffffffff/controllers/0", auth);
        EXPECT(res && res->status == 404);
    }
    {
        TEST("DELETE /api/connections/:id: own-session close, no close-notify");
        int notifiesBefore = clientPort.closeNotifies;
        auto res = cli.Delete(("/api/connections/" + connId).c_str(), auth);
        EXPECT(res && res->status == 200);
        if (res) EXPECT(jsonBool(parseJson(res->body), "ok"));
        EXPECT_EQ(clientPort.closeNotifies, notifiesBefore);

        TEST("DELETE of the already-closed session: 404");
        auto res2 = cli.Delete(("/api/connections/" + connId).c_str(), auth);
        EXPECT(res2 && res2->status == 404);
    }

    // ---- pairing: path A (server PIN) ---------------------------------------
    {
        TEST("POST /api/pair without deviceId: ok:false");
        auto res = cli.Post("/api/pair", R"({"pin":"1234"})", "application/json");
        EXPECT(res && res->status == 200);
        if (res) {
            Json j = parseJson(res->body);
            EXPECT_EQ(jsonBool(j, "ok"), false);
            EXPECT_EQ(jsonStr(j, "error"), std::string("missing deviceId"));
        }
    }
    {
        TEST("POST /api/pair with a wrong PIN: ok:false invalid/expired");
        std::string wrong = wrongPinFor(pinSnapshot());
        auto res = cli.Post("/api/pair",
                            std::string(R"({"deviceId":"dev-a-wrong","pin":")") + wrong + R"("})",
                            "application/json");
        EXPECT(res && res->status == 200);
        if (res) {
            Json j = parseJson(res->body);
            EXPECT_EQ(jsonBool(j, "ok"), false);
            EXPECT_EQ(jsonStr(j, "error"), std::string("invalid or expired PIN"));
        }
        EXPECT_EQ(storedKeyHex("dev-a-wrong"), std::string(""));
    }
    std::string pinPairedKeyHex;
    {
        TEST("path A: correct server PIN pairs and returns a random sharedKey");
        std::string pin = pinSnapshot().currentPin;
        auto res =
            cli.Post("/api/pair",
                     std::string(R"({"deviceId":"dev-a-pin","deviceName":"PinDev","pin":")") + pin +
                         R"(","protocolVersion":1})",
                     "application/json");
        EXPECT(res && res->status == 200);
        if (res) {
            Json j = parseJson(res->body);
            EXPECT_EQ(jsonBool(j, "ok"), true);
            EXPECT_EQ(jsonStr(j, "message"), std::string("paired successfully"));
            pinPairedKeyHex = jsonStr(j, "sharedKey");
            EXPECT(isHex(pinPairedKeyHex, 64));
            EXPECT(!j.contains("serverPublicKey"));
            EXPECT_EQ(jsonInt(j, "protocolVersion"), static_cast<long>(PROTOCOL_VERSION));
        }
        EXPECT_EQ(storedKeyHex("dev-a-pin"), pinPairedKeyHex);
    }
    {
        TEST("path A with a client X25519 key: X25519-derived, key never on the wire");
        uint8_t clientPk[32], clientSk[32];
        crypto_kx_keypair(clientPk, clientSk);
        std::string pin = pinSnapshot().currentPin;
        auto res = cli.Post("/api/pair",
                            std::string(R"({"deviceId":"dev-a-kx","pin":")") + pin +
                                R"(","publicKey":")" + hexEncode(clientPk, 32) + R"("})",
                            "application/json");
        EXPECT(res && res->status == 200);
        if (res) {
            Json j = parseJson(res->body);
            EXPECT_EQ(jsonBool(j, "ok"), true);
            std::string serverPkHex = jsonStr(j, "serverPublicKey");
            EXPECT(isHex(serverPkHex, 64));
            EXPECT(!j.contains("sharedKey")); // derived: never sent over the wire

            // Client-side derivation must agree with the persisted server key.
            uint8_t serverPk[32], rx[32], tx[32];
            EXPECT(hexDecode(serverPkHex, serverPk, 32));
            EXPECT(crypto_kx_client_session_keys(rx, tx, clientPk, clientSk, serverPk) == 0);
            EXPECT_EQ(storedKeyHex("dev-a-kx"), hexEncode(tx, 32));
        }
    }
    {
        TEST("path A with an unusable client key: rejected, nothing persisted");
        std::string pin = pinSnapshot().currentPin;
        auto res = cli.Post("/api/pair",
                            std::string(R"({"deviceId":"dev-a-badkey","pin":")") + pin +
                                R"(","publicKey":"zzzz"})",
                            "application/json");
        EXPECT(res && res->status == 200);
        if (res) {
            Json j = parseJson(res->body);
            EXPECT_EQ(jsonBool(j, "ok"), false);
            EXPECT_EQ(jsonStr(j, "error"), std::string("invalid public key"));
        }
        EXPECT_EQ(storedKeyHex("dev-a-badkey"), std::string(""));

        TEST("a rejected key does not consume the operator PIN (retry can succeed)");
        EXPECT_EQ(pinSnapshot().currentPin, pin);
        EXPECT(verifyPin(pin)); // still valid; consumed here by the assertion
    }
    {
        TEST("POST /api/pair with protocolVersion 2: 409 before any PIN check");
        auto res = cli.Post("/api/pair", R"({"deviceId":"dev-v2","pin":"0000",
                            "protocolVersion":2})",
                            "application/json");
        EXPECT(res && res->status == 409);
    }

    // ---- pairing: key rotation ----------------------------------------------
    {
        TEST("hmacProof of the current key rotates it (and returns the new one)");
        uint8_t curKey[CRYPTO_KEY_SIZE];
        EXPECT(hexDecode(pinPairedKeyHex, curKey, CRYPTO_KEY_SIZE));
        auto res = cli.Post("/api/pair",
                            std::string(R"({"deviceId":"dev-a-pin","hmacProof":")") +
                                computeHmacProof(curKey, "dev-a-pin") + R"("})",
                            "application/json");
        EXPECT(res && res->status == 200);
        if (res) {
            Json j = parseJson(res->body);
            EXPECT_EQ(jsonBool(j, "ok"), true);
            EXPECT_EQ(jsonStr(j, "message"), std::string("key rotated"));
            std::string rotated = jsonStr(j, "sharedKey");
            EXPECT(isHex(rotated, 64));
            EXPECT(rotated != pinPairedKeyHex);
            EXPECT_EQ(storedKeyHex("dev-a-pin"), rotated);
        }
    }
    {
        TEST("a bad rotation proof falls through to the PIN paths (no PIN: fail)");
        uint8_t otherKey[CRYPTO_KEY_SIZE] = {};
        std::string before = storedKeyHex("dev-a-pin");
        auto res = cli.Post("/api/pair",
                            std::string(R"({"deviceId":"dev-a-pin","hmacProof":")") +
                                computeHmacProof(otherKey, "dev-a-pin") + R"("})",
                            "application/json");
        EXPECT(res && res->status == 200);
        if (res) {
            Json j = parseJson(res->body);
            EXPECT_EQ(jsonBool(j, "ok"), false);
            EXPECT_EQ(jsonStr(j, "error"), std::string("invalid or expired PIN"));
        }
        EXPECT_EQ(storedKeyHex("dev-a-pin"), before); // key untouched
    }

    // ---- pairing: path B (dish PIN + poll) -----------------------------------
    {
        TEST("path B: clientPin registers a pending request");
        auto res = cli.Post("/api/pair",
                            R"({"deviceId":"dev-b-poll","deviceName":"B","clientPin":"7391"})",
                            "application/json");
        EXPECT(res && res->status == 200);
        if (res) {
            Json j = parseJson(res->body);
            EXPECT_EQ(jsonBool(j, "ok"), false);
            EXPECT_EQ(jsonBool(j, "pending"), true);
        }

        TEST("path B: poll reads pending while the operator decides");
        auto poll = cli.Get("/api/pair/status?deviceId=dev-b-poll");
        EXPECT(poll && poll->status == 200);
        if (poll) {
            Json j = parseJson(poll->body);
            EXPECT_EQ(jsonBool(j, "ok"), false);
            EXPECT_EQ(jsonStr(j, "status"), std::string("pending"));
        }

        TEST("path B: operator accept hands the minted key back exactly once");
        EXPECT(confirmPairing("dev-b-poll")); // operator side (dashboard/tray seam)
        auto approved = cli.Get("/api/pair/status?deviceId=dev-b-poll");
        EXPECT(approved && approved->status == 200);
        std::string handedKey;
        if (approved) {
            Json j = parseJson(approved->body);
            EXPECT_EQ(jsonBool(j, "ok"), true);
            EXPECT_EQ(jsonStr(j, "status"), std::string("approved"));
            handedKey = jsonStr(j, "sharedKey");
            EXPECT(isHex(handedKey, 64));
        }
        EXPECT_EQ(storedKeyHex("dev-b-poll"), handedKey);

        TEST("path B: a replayed poll gets none (single-use handback)");
        auto replay = cli.Get("/api/pair/status?deviceId=dev-b-poll");
        EXPECT(replay && replay->status == 200);
        if (replay) {
            Json j = parseJson(replay->body);
            EXPECT_EQ(jsonBool(j, "ok"), false);
            EXPECT_EQ(jsonStr(j, "status"), std::string("none"));
        }
    }
    {
        TEST("path B: poll after operator deny reads none (deny erases the request)");
        auto res = cli.Post("/api/pair",
                            R"({"deviceId":"dev-b-deny","deviceName":"B2","clientPin":"4185"})",
                            "application/json");
        EXPECT(res && res->status == 200);
        EXPECT(declinePairing("dev-b-deny"));
        auto poll = cli.Get("/api/pair/status?deviceId=dev-b-deny");
        EXPECT(poll && poll->status == 200);
        if (poll) {
            Json j = parseJson(poll->body);
            EXPECT_EQ(jsonBool(j, "ok"), false);
            // Never "denied": PairRequestState::Denied is assigned nowhere.
            EXPECT_EQ(jsonStr(j, "status"), std::string("none"));
        }
        EXPECT_EQ(storedKeyHex("dev-b-deny"), std::string(""));
    }
    {
        TEST("pair status without deviceId: 400");
        auto res = cli.Get("/api/pair/status");
        EXPECT(res && res->status == 400);
    }

    // ---- self-unpair -----------------------------------------------------------
    {
        TEST("DELETE /api/pair without credentials: 401");
        auto res = cli.Delete("/api/pair");
        EXPECT(res && res->status == 401);

        TEST("DELETE /api/pair: authed self-unpair removes the device");
        auto ok = cli.Delete("/api/pair", auth);
        EXPECT(ok && ok->status == 200);
        if (ok) EXPECT(jsonBool(parseJson(ok->body), "ok"));
        EXPECT_EQ(storedKeyHex(devId), std::string(""));

        TEST("after self-unpair the old credentials are terminal: 401 NOT_PAIRED");
        auto rejected = cli.Put("/api/connections", auth, "{}", "application/json");
        EXPECT(rejected && rejected->status == 401);
        if (rejected) {
            EXPECT_EQ(jsonStr(parseJson(rejected->body), "code"), std::string("NOT_PAIRED"));
        }
    }

    // ---- unauthenticated info surface -----------------------------------------
    {
        TEST("GET /api/server/capabilities: open, versioned, backend present");
        auto res = cli.Get("/api/server/capabilities");
        EXPECT(res && res->status == 200);
        if (res) {
            Json j = parseJson(res->body);
            EXPECT_EQ(jsonInt(j, "protocolVersion"), static_cast<long>(PROTOCOL_VERSION));
            EXPECT_EQ(jsonInt(j, "maxControllers"), static_cast<long>(MAX_BACKEND_CONTROLLERS));
            EXPECT(j.contains("backend") && j["backend"].is_object());
            EXPECT(j.contains("host") && j["host"].is_object());
            if (j.contains("backend")) EXPECT(!jsonStr(j["backend"], "id").empty());
        }
    }

    // ---- shutdown guard ---------------------------------------------------------
    {
        TEST("PUT /api/connections while shutting down: 503");
        putPairedDevice(devId, hexEncode(pairingKey, CRYPTO_KEY_SIZE)); // re-pair for auth
        g_appRunning = false;
        auto res = cli.Put("/api/connections", auth, "{}", "application/json");
        EXPECT(res && res->status == 503);
        g_appRunning = true;

        TEST("recovers once running again");
        auto ok = cli.Put("/api/connections", auth, "{}", "application/json");
        EXPECT(ok && ok->status == 200);
    }

    server.stop();
    serverThread.join();

    std::cout << "\n=== Test Results ===\n";
    std::cout << "  Passed: " << g_pass << "\n";
    std::cout << "  Failed: " << g_fail << "\n";
    std::cout << "  STATUS: " << (g_fail == 0 ? "ALL PASSED" : "FAILED") << "\n";
    return g_fail == 0 ? 0 : 1;
}
