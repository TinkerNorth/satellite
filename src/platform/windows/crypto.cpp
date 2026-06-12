// SPDX-License-Identifier: LGPL-3.0-or-later
#include "crypto.h"
#include <sodium.h>

static const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const std::vector<BYTE>& data) {
    std::string out;
    int val = 0, bits = -6;
    for (BYTE c : data) {
        val = (val << 8) + c;
        bits += 8;
        while (bits >= 0) {
            out.push_back(b64chars[(val >> bits) & 0x3F]);
            bits -= 6;
        }
    }
    if (bits > -6) out.push_back(b64chars[((val << 8) >> (bits + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

std::vector<BYTE> base64Decode(const std::string& s) {
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; i++) T[(int)b64chars[i]] = i;
    std::vector<BYTE> out;
    int val = 0, bits = -8;
    for (char c : s) {
        if (T[(unsigned char)c] == -1) break;
        val = (val << 6) + T[(unsigned char)c];
        bits += 6;
        if (bits >= 0) {
            out.push_back((BYTE)(val >> bits));
            bits -= 8;
        }
    }
    return out;
}

std::string sha256hex(const std::string& input) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    BYTE hash[32];
    BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0);
    BCryptHashData(hHash, (PUCHAR)input.data(), (ULONG)input.size(), 0);
    BCryptFinishHash(hHash, hash, 32, 0);
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    char hex[65];
    for (int i = 0; i < 32; i++)
        (void)snprintf(hex + static_cast<ptrdiff_t>(i) * 2, 3, "%02x", hash[i]);
    hex[64] = 0;
    return std::string(hex);
}

std::string dpapiEncrypt(const std::string& plaintext) {
    DATA_BLOB in, out;
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.data()));
    in.cbData = (DWORD)plaintext.size();
    if (CryptProtectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out) == FALSE) return "";
    std::vector<BYTE> blob(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    return base64Encode(blob);
}

std::string dpapiDecrypt(const std::string& encoded) {
    auto blob = base64Decode(encoded);
    DATA_BLOB in, out;
    in.pbData = blob.data();
    in.cbData = (DWORD)blob.size();
    if (CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out) == FALSE) return "";
    std::string result(reinterpret_cast<char*>(out.pbData), out.cbData);
    LocalFree(out.pbData);
    return result;
}

// PINs and identity tokens are security-sensitive, so these draw from
// libsodium's CSPRNG, never std::mt19937 (deterministic, reconstructable).
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
    // randombytes_uniform is rejection-sampled — no modulo bias (unlike % 10).
    for (int i = 0; i < n; i++) out += static_cast<char>('0' + randombytes_uniform(10));
    return out;
}

// PIN state machine surfaced to the dashboard; see PinState in core/types.h.
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
    // Constant-time compare so a wrong guess can't leak (via timing) how many
    // leading digits matched. sodium_memcmp needs equal lengths — gate on size.
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

PinSnapshot pinSnapshot() {
    std::lock_guard<std::mutex> lk(g_pinMtx);
    rotatePinsIfDueLocked();
    const auto now = std::chrono::steady_clock::now();
    PinSnapshot s;
    s.currentPin = g_currentPin;
    s.previousPin = g_previousPin;
    const auto rem = std::chrono::duration_cast<std::chrono::seconds>(g_pinRotateAt - now).count();
    s.secondsRemaining = rem < 0 ? 0 : static_cast<int>(rem);
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
