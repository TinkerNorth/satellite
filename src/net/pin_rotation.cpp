// SPDX-License-Identifier: LGPL-3.0-or-later

// Hoisted verbatim from the triplicated platform crypto.cpp files (D11).
// Two deliberate unifications, both taking the strictest of the three copies:
//   - randomDigits uses randombytes_uniform (Windows copy; rejection-sampled,
//     no modulo bias) instead of the POSIX copies' `byte % 10`.
//   - hexDecode uses explicit nibble parsing (Windows copy) instead of the
//     POSIX copies' sscanf("%02x"), which silently accepted some malformed
//     hex (e.g. "0g" as 0x00) instead of rejecting it.
// The backdatePinClockForTest seam (previously macOS-only) is now available
// to every platform's test build.
#include "pin_rotation.h"

#include <sodium.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>

std::string randomHex(int bytes) {
    std::string out;
    char buf[3];
    for (int i = 0; i < bytes; i++) {
        uint8_t b = 0;
        randombytes_buf(&b, 1);
        (void)snprintf(buf, sizeof(buf), "%02x", b);
        out += buf;
    }
    return out;
}

std::string randomDigits(int n) {
    std::string out;
    // randombytes_uniform is rejection-sampled: no modulo bias (unlike % 10).
    for (int i = 0; i < n; i++) out += static_cast<char>('0' + randombytes_uniform(10));
    return out;
}

static std::mutex g_pinMtx;
static std::string g_currentPin;
static std::string g_previousPin;
static std::chrono::steady_clock::time_point g_pinRotateAt;
static std::chrono::steady_clock::time_point g_pinPairedAt;
static bool g_pinPairedValid = false;
static int g_pinFailCount = 0;
static constexpr int PIN_PAIRED_HOLD_SEC = 5;
static constexpr auto PIN_ROTATE_PERIOD = std::chrono::minutes(5);
// Burn both PINs after this many wrong guesses; otherwise the 4-digit space is
// online-brute-forceable within the rotation period.
static constexpr int PIN_MAX_FAILS = 5;

static void resetPinsLocked() {
    g_currentPin = randomDigits(4);
    g_previousPin.clear();
    g_pinRotateAt = std::chrono::steady_clock::now() + PIN_ROTATE_PERIOD;
    g_pinFailCount = 0;
}

static void rotatePinsIfDueLocked() {
    const auto now = std::chrono::steady_clock::now();
    if (g_currentPin.empty()) {
        resetPinsLocked();
        return;
    }
    if (now < g_pinRotateAt) return;
    g_previousPin = (now - g_pinRotateAt < PIN_ROTATE_PERIOD) ? g_currentPin : std::string();
    g_currentPin = randomDigits(4);
    g_pinRotateAt = now + PIN_ROTATE_PERIOD;
    g_pinFailCount = 0;
}

bool verifyPin(const std::string& pin) {
    std::lock_guard<std::mutex> lk(g_pinMtx);
    rotatePinsIfDueLocked();
    // Constant-time compare so timing can't leak how many leading digits
    // matched. sodium_memcmp needs equal lengths, so gate on size first.
    auto matches = [&](const std::string& candidate) {
        return !candidate.empty() && pin.size() == candidate.size() &&
               sodium_memcmp(pin.data(), candidate.data(), candidate.size()) == 0;
    };
    const bool okCurrent = matches(g_currentPin);
    const bool okPrevious = matches(g_previousPin);
    if (okCurrent || okPrevious) {
        resetPinsLocked();
        g_pinPairedAt = std::chrono::steady_clock::now();
        g_pinPairedValid = true;
        return true;
    }
    if (++g_pinFailCount >= PIN_MAX_FAILS) resetPinsLocked();
    return false;
}

#ifdef SATELLITE_BUILD_TESTS
void backdatePinClockForTest(int seconds) {
    std::lock_guard<std::mutex> lk(g_pinMtx);
    g_pinRotateAt -= std::chrono::seconds(seconds);
    g_pinPairedAt -= std::chrono::seconds(seconds);
}
#endif

PinSnapshot pinSnapshot() {
    std::lock_guard<std::mutex> lk(g_pinMtx);
    rotatePinsIfDueLocked();
    const auto now = std::chrono::steady_clock::now();
    PinSnapshot s;
    s.currentPin = g_currentPin;
    s.previousPin = g_previousPin;
    const auto rem = std::chrono::duration_cast<std::chrono::seconds>(g_pinRotateAt - now).count();
    s.secondsRemaining = rem < 0 ? 0 : static_cast<int>(rem);
    // Flash PinPaired briefly after a pair so the dashboard doesn't stick on it.
    s.state = (g_pinPairedValid && now - g_pinPairedAt <= std::chrono::seconds(PIN_PAIRED_HOLD_SEC))
                  ? PinState::PinPaired
                  : PinState::PinActive;
    return s;
}

std::string hexEncode(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(len * 2);
    char buf[3];
    for (size_t i = 0; i < len; i++) {
        (void)snprintf(buf, sizeof(buf), "%02x", data[i]);
        out += buf;
    }
    return out;
}

static int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool hexDecode(const std::string& hex, uint8_t* out, size_t outLen) {
    if (hex.size() != outLen * 2) return false;
    for (size_t i = 0; i < outLen; i++) {
        int hi = hexNibble(hex[i * 2]);
        int lo = hexNibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

bool sodiumInit() {
    return sodium_init() >= 0; // 0 = first init, 1 = already initialized; both OK
}

void generateKeyPair(uint8_t pk[32], uint8_t sk[32]) { crypto_kx_keypair(pk, sk); }

bool computeSharedKey(uint8_t sharedKey[32], const uint8_t clientPk[32], const uint8_t serverSk[32],
                      const uint8_t serverPk[32]) {
    uint8_t rx[32], tx[32];
    if (crypto_kx_server_session_keys(rx, tx, serverPk, serverSk, clientPk) != 0) return false;
    // Symmetric: both directions key off rx, so the client's tx matches.
    memcpy(sharedKey, rx, 32);
    return true;
}

uint32_t generateToken() {
    uint32_t token = 0;
    randombytes_buf(&token, sizeof(token));
    while (token == 0) randombytes_buf(&token, sizeof(token)); // 0 is reserved for "no token"
    return token;
}
