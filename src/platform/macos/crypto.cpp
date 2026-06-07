// SPDX-License-Identifier: LGPL-3.0-or-later
#include "crypto.h"
#include "config.h"

#include <sodium.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64Encode(const std::vector<uint8_t>& data) {
    std::string out;
    int val = 0, bits = -6;
    for (uint8_t c : data) {
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

static std::vector<uint8_t> base64Decode(const std::string& s) {
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; i++) T[(int)b64chars[i]] = i;
    std::vector<uint8_t> out;
    int val = 0, bits = -8;
    for (char c : s) {
        if (T[(unsigned char)c] == -1) break;
        val = (val << 6) + T[(unsigned char)c];
        bits += 6;
        if (bits >= 0) {
            out.push_back((uint8_t)(val >> bits));
            bits -= 8;
        }
    }
    return out;
}

std::string sha256hex(const std::string& input) {
    uint8_t hash[crypto_hash_sha256_BYTES]; // 32
    crypto_hash_sha256(hash, reinterpret_cast<const uint8_t*>(input.data()), input.size());
    char hex[65];
    for (int i = 0; i < 32; i++) (void)snprintf(hex + i * 2, 3, "%02x", hash[i]);
    hex[64] = 0;
    return std::string(hex);
}

// Local keyfile replaces DPAPI: 0600 file next to the config, the practical
// equivalent of DPAPI's user-scoped key. Keychain would be stronger but adds
// much more surface.
static constexpr size_t LOCAL_KEY_LEN = crypto_secretbox_KEYBYTES; // 32

static std::string localKeyPath() {
    // configPath() lives in the same dir; strip the trailing filename.
    std::string p = configPath();
    auto pos = p.find_last_of('/');
    std::string dir = (pos != std::string::npos) ? p.substr(0, pos) : ".";
    return dir + "/keyfile";
}

static bool readLocalKey(uint8_t out[LOCAL_KEY_LEN]) {
    int fd = ::open(localKeyPath().c_str(), O_RDONLY);
    if (fd < 0) return false;
    ssize_t n = ::read(fd, out, LOCAL_KEY_LEN);
    ::close(fd);
    return n == (ssize_t)LOCAL_KEY_LEN;
}

static bool writeLocalKey(const uint8_t key[LOCAL_KEY_LEN]) {
    std::string path = localKeyPath();
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    ssize_t n = ::write(fd, key, LOCAL_KEY_LEN);
    ::close(fd);
    (void)::chmod(path.c_str(), 0600);
    return n == (ssize_t)LOCAL_KEY_LEN;
}

static bool getOrCreateLocalKey(uint8_t out[LOCAL_KEY_LEN]) {
    if (readLocalKey(out)) return true;
    randombytes_buf(out, LOCAL_KEY_LEN);
    return writeLocalKey(out);
}

// DPAPI equivalent: libsodium secretbox, base64-encoded (nonce ‖ ciphertext).
std::string dpapiEncrypt(const std::string& plaintext) {
    uint8_t key[LOCAL_KEY_LEN];
    if (!getOrCreateLocalKey(key)) return "";

    uint8_t nonce[crypto_secretbox_NONCEBYTES];
    randombytes_buf(nonce, sizeof(nonce));

    std::vector<uint8_t> ct(crypto_secretbox_MACBYTES + plaintext.size());
    if (crypto_secretbox_easy(ct.data(), reinterpret_cast<const uint8_t*>(plaintext.data()),
                              plaintext.size(), nonce, key) != 0) {
        sodium_memzero(key, sizeof(key));
        return "";
    }
    sodium_memzero(key, sizeof(key));

    std::vector<uint8_t> blob;
    blob.reserve(sizeof(nonce) + ct.size());
    blob.insert(blob.end(), nonce, nonce + sizeof(nonce));
    blob.insert(blob.end(), ct.begin(), ct.end());
    return base64Encode(blob);
}

std::string dpapiDecrypt(const std::string& encoded) {
    auto blob = base64Decode(encoded);
    if (blob.size() < crypto_secretbox_NONCEBYTES + crypto_secretbox_MACBYTES) return "";

    uint8_t key[LOCAL_KEY_LEN];
    if (!readLocalKey(key)) return "";

    const uint8_t* nonce = blob.data();
    const uint8_t* ct = blob.data() + crypto_secretbox_NONCEBYTES;
    size_t ctLen = blob.size() - crypto_secretbox_NONCEBYTES;

    std::vector<uint8_t> pt(ctLen - crypto_secretbox_MACBYTES);
    if (crypto_secretbox_open_easy(pt.data(), ct, ctLen, nonce, key) != 0) {
        sodium_memzero(key, sizeof(key));
        return "";
    }
    sodium_memzero(key, sizeof(key));
    return std::string(reinterpret_cast<char*>(pt.data()), pt.size());
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
    for (int i = 0; i < n; i++) {
        uint8_t b = 0;
        randombytes_buf(&b, 1);
        out += ('0' + (b % 10));
    }
    return out;
}

// PIN state machine surfaced to the dashboard; see PinState in core/types.h.
static std::mutex g_pinMtx;
static std::string g_currentPin;
static std::chrono::steady_clock::time_point g_pinExpiry;
static std::chrono::steady_clock::time_point g_pinPairedAt; // default-constructed = "never"
static bool g_pinPairedValid = false;
static int g_pinFailCount = 0;
static constexpr int PIN_PAIRED_HOLD_SEC = 5;
// Burn the PIN after this many wrong guesses; otherwise the 4-digit space is
// online-brute-forceable within the 5-minute window.
static constexpr int PIN_MAX_FAILS = 5;

std::string generatePin() {
    std::lock_guard<std::mutex> lk(g_pinMtx);
    g_currentPin = randomDigits(4);
    g_pinExpiry = std::chrono::steady_clock::now() + std::chrono::minutes(5);
    g_pinPairedValid = false;
    g_pinFailCount = 0;
    return g_currentPin;
}

bool verifyPin(const std::string& pin) {
    std::lock_guard<std::mutex> lk(g_pinMtx);
    if (g_currentPin.empty() || std::chrono::steady_clock::now() > g_pinExpiry) return false;
    // Constant-time compare so a wrong guess can't leak (via timing) how many
    // leading digits matched. sodium_memcmp needs equal lengths — gate on size.
    bool ok = (pin.size() == g_currentPin.size()) &&
              sodium_memcmp(pin.data(), g_currentPin.data(), g_currentPin.size()) == 0;
    if (ok) {
        g_currentPin.clear();
        g_pinFailCount = 0;
        g_pinPairedAt = std::chrono::steady_clock::now();
        g_pinPairedValid = true;
        return true;
    }
    if (++g_pinFailCount >= PIN_MAX_FAILS) g_currentPin.clear();
    return false;
}

PinSnapshot pinSnapshot() {
    std::lock_guard<std::mutex> lk(g_pinMtx);
    const auto now = std::chrono::steady_clock::now();
    PinSnapshot s;
    if (!g_currentPin.empty()) {
        if (now > g_pinExpiry) {
            s.state = PinState::PinExpired;
            s.secondsRemaining = 0;
        } else {
            s.state = PinState::PinActive;
            s.secondsRemaining = static_cast<int>(
                std::chrono::duration_cast<std::chrono::seconds>(g_pinExpiry - now).count());
        }
        return s;
    }
    // Flash PinPaired briefly after a successful pair so the dashboard can
    // show "Paired!" without sticking on it.
    if (g_pinPairedValid && now - g_pinPairedAt <= std::chrono::seconds(PIN_PAIRED_HOLD_SEC)) {
        s.state = PinState::PinPaired;
        s.secondsRemaining = 0;
        return s;
    }
    s.state = PinState::PinIdle;
    s.secondsRemaining = 0;
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

bool hexDecode(const std::string& hex, uint8_t* out, size_t outLen) {
    if (hex.size() != outLen * 2) return false;
    for (size_t i = 0; i < outLen; i++) {
        unsigned int b = 0;
        if (sscanf(hex.c_str() + i * 2, "%02x", &b) != 1) return false;
        out[i] = (uint8_t)b;
    }
    return true;
}

bool sodiumInit() { return sodium_init() >= 0; }

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

bool encryptPacket(const uint8_t key[32], uint32_t counter, uint32_t token,
                   const uint8_t* plaintext, size_t ptLen, uint8_t* ciphertext,
                   unsigned long long* ctLen) {
    // Nonce: 4-byte counter big-endian, right-aligned in 12 zero bytes. AAD: token big-endian.
    uint8_t nonce[12] = {};
    nonce[8] = (uint8_t)(counter >> 24);
    nonce[9] = (uint8_t)(counter >> 16);
    nonce[10] = (uint8_t)(counter >> 8);
    nonce[11] = (uint8_t)(counter);

    uint8_t aad[4];
    aad[0] = (uint8_t)(token >> 24);
    aad[1] = (uint8_t)(token >> 16);
    aad[2] = (uint8_t)(token >> 8);
    aad[3] = (uint8_t)(token);

    return crypto_aead_chacha20poly1305_ietf_encrypt(ciphertext, ctLen, plaintext, ptLen, aad,
                                                     sizeof(aad), nullptr, nonce, key) == 0;
}

bool decryptPacket(const uint8_t key[32], uint32_t counter, uint32_t token,
                   const uint8_t* ciphertext, size_t ctLen, uint8_t* plaintext,
                   unsigned long long* ptLen) {
    uint8_t nonce[12] = {};
    nonce[8] = (uint8_t)(counter >> 24);
    nonce[9] = (uint8_t)(counter >> 16);
    nonce[10] = (uint8_t)(counter >> 8);
    nonce[11] = (uint8_t)(counter);

    uint8_t aad[4];
    aad[0] = (uint8_t)(token >> 24);
    aad[1] = (uint8_t)(token >> 16);
    aad[2] = (uint8_t)(token >> 8);
    aad[3] = (uint8_t)(token);

    return crypto_aead_chacha20poly1305_ietf_decrypt(plaintext, ptLen, nullptr, ciphertext, ctLen,
                                                     aad, sizeof(aad), nonce, key) == 0;
}
