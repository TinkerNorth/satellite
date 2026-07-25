// SPDX-License-Identifier: LGPL-3.0-or-later

// Robustness fuzz harness for the receiver's decrypt -> inner_dispatch path:
// "malformed datagrams never crash or read out of bounds". It mirrors the
// packet pipeline of receiver.cpp against a live SessionService (stub ports,
// real HKDF/AEAD from net/session_crypto.cpp) without sockets:
//
//   input = counter(4 BE) || inner-plaintext (type(2) | len(2) | payload...)
//
//   1. Well-formed-envelope path: the input plaintext is sealed with the
//      session key exactly as a sender would (so the AEAD opens) and then
//      pushed through the verbatim receiver guards: replay check, in-place
//      decrypt, inner-header length guards, fused gamepad hot path vs. cold
//      dispatch. This is the attacker who owns a paired device and sends
//      arbitrary *authenticated* bytes.
//   2. Garbage-ciphertext path: the raw input is fed to decryptPacket as if
//      it arrived off the wire (and to the unknown-token lookup) — the
//      attacker who owns nothing. The AEAD must reject; nothing may crash.
//
// Dispatch happens from an exact-size heap copy of the payload (the
// receiver's 256-byte stack buffer has slack that would hide small
// over-reads from ASan), same trick as test_receiver.cpp's dispatchTight.
//
// Counter saturation mirrors production: once the session's replay floor
// nears exhaustion the harness re-PUTs the session (token/key rotate,
// counters reset), like a real client's proactive re-key.
//
// Built by -DSATELLITE_FUZZ=ON (clang): with the libFuzzer runtime when the
// toolchain ships it, else as a standalone corpus-replay + deterministic-
// sweep driver (SATELLITE_FUZZ_STANDALONE) under the same ASan/UBSan flags,
// so hosts without libFuzzer (e.g. Apple CLT) still execute the harness.
#include "core/session_service.h"
#include "net/inner_dispatch.h"
#include "net/session_crypto.h"

#include <sodium.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

struct FuzzGamepad : IGamepadPort {
    bool ensureBusOpen() override { return true; }
    void closeBus() override {}
    bool isBusOpen() const override { return true; }
    bool pluginDevice(uint32_t, GamepadIdentity) override { return true; }
    bool supportsIdentity(GamepadIdentity) const override { return true; }
    bool unplugDevice(uint32_t) override { return true; }
    bool submitReport(uint32_t, const GamepadReport&) override { return true; }
    void setRumbleCallback(RumbleCallback) override {}
    bool submitMotion(uint32_t, const MotionReport&) override { return true; }
    bool supportsMotionForType(uint8_t type) const override {
        return controllerTypeHasMotion(type);
    }
    bool submitBattery(uint32_t, const BatteryReport&) override { return true; }
    bool submitTouchpad(uint32_t, const TouchpadReport&) override { return true; }
    bool submitRelativeMouse(int, int, bool) override { return true; }
    bool supportsRelativeMouse() const override { return true; }
    void setLightbarCallback(LightbarCallback) override {}
};

struct FuzzClient : IClientPort {
    void updateClientAddr(uint32_t, const std::string&, uint16_t) override {}
    void removeClientAddr(uint32_t) override {}
    void sendHeartbeatAck(const Connection&, bool, uint8_t, uint16_t, uint16_t) override {}
    void sendSessionClose(const Connection&, uint8_t) override {}
    void sendRumble(const Connection&, uint8_t, const RumbleReport&) override {}
    void sendLightbar(const Connection&, uint8_t, uint8_t, uint8_t, uint8_t) override {}
};

struct FuzzLog : ILogPort {
    void logMsg(LogLevel, const std::string&, const std::string&) override {}
};

FuzzGamepad g_gamepad;
FuzzClient g_client;
FuzzLog g_log;
SessionService* g_svc = nullptr;
uint32_t g_token = 0;

// The receiver reads at most 256-byte datagrams: header(8) + ciphertext, and
// ciphertext carries a 16-byte tag, so plaintext is capped at 232.
constexpr size_t MAX_DATAGRAM = 256;
constexpr size_t MAX_PLAINTEXT = MAX_DATAGRAM - HEADER_SIZE - AUTH_TAG_SIZE;

// Rotate well before the u32 replay floor can saturate the session (mirrors
// the sender's proactive re-key threshold).
constexpr uint32_t REKEY_COUNTER_FLOOR = 0xF0000000u;

void openSession() {
    uint8_t pairingKey[CRYPTO_KEY_SIZE];
    for (int i = 0; i < CRYPTO_KEY_SIZE; i++) pairingKey[i] = static_cast<uint8_t>(0x5C ^ i);
    std::vector<ControllerDescriptor> descriptors(2);
    descriptors[0].ctrlIdx = 0;
    descriptors[0].type = CONTROLLER_TYPE_XBOX;
    descriptors[0].caps = CAP_RUMBLE | CAP_ANALOG_TRIGGERS;
    descriptors[0].touchpadMode = TOUCHPAD_MODE_OFF;
    descriptors[1].ctrlIdx = 1;
    descriptors[1].type = CONTROLLER_TYPE_PLAYSTATION;
    descriptors[1].caps = CAP_RUMBLE | CAP_MOTION | CAP_LIGHTBAR;
    descriptors[1].touchpadMode = TOUCHPAD_MODE_DS4;
    auto r = g_svc->upsertSession("fuzz-dev", "Fuzzer", "192.0.2.1", pairingKey, descriptors,
                                  /*requestMouseControl=*/true);
    g_token = r.token;
}

void initOnce() {
    if (g_svc != nullptr) return;
    // AEAD behavior is undefined before sodium_init (production calls it at
    // startup; the harness must too).
    if (sodium_init() < 0) abort();
    static SessionService svc(g_gamepad, g_client, g_log, deriveSessionKey);
    g_svc = &svc;
    openSession();
}

// The exact post-decrypt tail of receiver.cpp's packet loop, with the payload
// re-homed into an exact-size heap buffer so any decoder over-read trips ASan.
void parseAndDispatch(uint32_t token, uint32_t counter, const uint8_t* plaintext,
                      unsigned long long ptLen) {
    const uint32_t senderIPv4 = 0x0100007fu; // 127.0.0.1, network byte order
    const uint16_t senderPort = 40100;

    if (ptLen < (unsigned long long)INNER_HEADER_SIZE) return;
    uint16_t msgType = ((uint16_t)plaintext[0] << 8) | (uint16_t)plaintext[1];
    uint16_t msgLen = ((uint16_t)plaintext[2] << 8) | (uint16_t)plaintext[3];
    if ((size_t)(INNER_HEADER_SIZE + msgLen) > ptLen) return;

    std::vector<uint8_t> tight(plaintext + INNER_HEADER_SIZE,
                               plaintext + INNER_HEADER_SIZE + msgLen);
    const uint8_t* payload = tight.empty() ? nullptr : tight.data();

    if (msgType == MSG_GAMEPAD_DATA && msgLen >= 13) {
        uint8_t ctrlIdx = payload[0];
        GamepadReport report;
        std::memcpy(&report, payload + 1, sizeof(GamepadReport));
        (void)g_svc->handleGamepadDataAndUpdate(token, counter, senderIPv4, senderPort, ctrlIdx,
                                                report);
    } else {
        g_svc->updatePostDecryptV4(token, counter, senderIPv4, senderPort);
        (void)dispatchInnerMessage(*g_svc, token, msgType, payload, msgLen);
    }

    // Same bytes against a token nobody owns: every handler's lookup-miss
    // branch must be as crash-free as the hit path.
    (void)dispatchInnerMessage(*g_svc, token ^ 0x5A5A5A5Au, msgType, payload, msgLen);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    initOnce();
    if (size < 5) return 0;

    // Proactive re-key: keep the session decryptable for the next input even
    // after a counter near the top advanced the replay floor.
    uint8_t key[CRYPTO_KEY_SIZE];
    uint32_t lastCounter = 0;
    if (!g_svc->getDecryptInfo(g_token, key, lastCounter) || lastCounter >= REKEY_COUNTER_FLOOR) {
        openSession();
        if (!g_svc->getDecryptInfo(g_token, key, lastCounter)) return 0;
    }

    const uint32_t counter = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
                             ((uint32_t)data[2] << 8) | (uint32_t)data[3];
    const uint8_t* inner = data + 4;
    const size_t innerLen = size - 4 > MAX_PLAINTEXT ? MAX_PLAINTEXT : size - 4;

    // ---- Path 1: authenticated envelope around attacker-chosen plaintext ----
    // Replay guard first, exactly like the receiver (decrypt never runs for a
    // replayed counter).
    if (!(counter <= lastCounter && lastCounter != 0)) {
        uint8_t ct[MAX_DATAGRAM];
        unsigned long long ctLen = 0;
        if (encryptPacket(key, CRYPTO_DIR_CLIENT_TO_SERVER, counter, g_token, inner, innerLen, ct,
                          &ctLen)) {
            unsigned long long ptLen = 0;
            // In-place decrypt, receiver-style (libsodium supports m == c).
            if (decryptPacket(key, CRYPTO_DIR_CLIENT_TO_SERVER, counter, g_token, ct, (size_t)ctLen,
                              ct, &ptLen)) {
                parseAndDispatch(g_token, counter, ct, ptLen);
            }
        }
    }

    // ---- Path 2: the raw input as hostile ciphertext ------------------------
    // An off-LAN attacker's datagram: unknown token first (lookup miss), then
    // the real token with an unauthenticated body (AEAD must reject; the
    // rejection path must be crash-free). A forged tag passing is 2^-128.
    {
        uint8_t bogusKey[CRYPTO_KEY_SIZE];
        uint32_t bogusLast = 0;
        (void)g_svc->getDecryptInfo(g_token ^ 0xA5A5A5A5u, bogusKey, bogusLast);

        if (innerLen >= (size_t)AUTH_TAG_SIZE) {
            uint8_t hostile[MAX_DATAGRAM];
            std::memcpy(hostile, inner, innerLen);
            unsigned long long ptLen = 0;
            if (decryptPacket(key, CRYPTO_DIR_CLIENT_TO_SERVER, counter, g_token, hostile, innerLen,
                              hostile, &ptLen)) {
                parseAndDispatch(g_token, counter, hostile, ptLen);
            }
        }
    }

    return 0;
}

#ifdef SATELLITE_FUZZ_STANDALONE
// Corpus-replay + deterministic-sweep driver for toolchains without the
// libFuzzer runtime (e.g. Apple CLT): replays every file in the corpus dirs
// passed on argv, then feeds a fixed xorshift sweep of pseudo-random inputs.
// Same harness, same sanitizers; only the mutation engine is missing.
#include <dirent.h>

#include <cstdio>
#include <fstream>
#include <string>

static int runOneFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return 0;
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
    (void)LLVMFuzzerTestOneInput(bytes.empty() ? nullptr : bytes.data(), bytes.size());
    return 1;
}

int main(int argc, char** argv) {
    if (sodium_init() < 0) {
        std::fprintf(stderr, "sodium_init failed\n");
        return 1;
    }
    int replayed = 0;
    for (int i = 1; i < argc; i++) {
        DIR* dir = opendir(argv[i]);
        if (dir != nullptr) {
            for (dirent* e = readdir(dir); e != nullptr; e = readdir(dir)) {
                if (e->d_name[0] == '.') continue;
                replayed += runOneFile(std::string(argv[i]) + "/" + e->d_name);
            }
            closedir(dir);
        } else {
            replayed += runOneFile(argv[i]);
        }
    }

    // Deterministic sweep: xorshift32-generated inputs across the interesting
    // size range (empty .. beyond the datagram cap).
    uint32_t s = 0x2545F491u;
    auto next = [&s]() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    };
    int swept = 0;
    for (int iter = 0; iter < 4096; iter++) {
        uint8_t buf[300];
        size_t len = next() % (sizeof(buf) + 1);
        for (size_t i = 0; i < len; i++) buf[i] = static_cast<uint8_t>(next());
        (void)LLVMFuzzerTestOneInput(buf, len);
        swept++;
    }

    std::printf("fuzz_receiver_dispatch (standalone): %d corpus inputs + %d sweep inputs, "
                "no crashes\n",
                replayed, swept);
    return 0;
}
#endif
