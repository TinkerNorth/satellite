/*
 * crypto.cpp — DPAPI, SHA-256, sessions, PIN, credentials, libsodium wrappers
 */
#include "crypto.h"
#include <sodium.h>

// ── Base64 helpers ──────────────────────────────────────────────────────────
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

// ── SHA-256 via Windows BCrypt ──────────────────────────────────────────────
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
    for (int i = 0; i < 32; i++) sprintf(hex + static_cast<ptrdiff_t>(i) * 2, "%02x", hash[i]);
    hex[64] = 0;
    return std::string(hex);
}

// ── DPAPI encrypt/decrypt ───────────────────────────────────────────────────
std::string dpapiEncrypt(const std::string& plaintext) {
    DATA_BLOB in, out;
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.data()));
    in.cbData = (DWORD)plaintext.size();
    if (!CryptProtectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out)) return "";
    std::vector<BYTE> blob(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    return base64Encode(blob);
}

std::string dpapiDecrypt(const std::string& encoded) {
    auto blob = base64Decode(encoded);
    DATA_BLOB in, out;
    in.pbData = blob.data();
    in.cbData = (DWORD)blob.size();
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out)) return "";
    std::string result(reinterpret_cast<char*>(out.pbData), out.cbData);
    LocalFree(out.pbData);
    return result;
}

// ── Random hex/digit generation ─────────────────────────────────────────────
std::string randomHex(int bytes) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);
    std::string out;
    char buf[3];
    for (int i = 0; i < bytes; i++) {
        sprintf(buf, "%02x", dis(gen));
        out += buf;
    }
    return out;
}

std::string randomDigits(int n) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 9);
    std::string out;
    for (int i = 0; i < n; i++) out += ('0' + dis(gen));
    return out;
}

// ── Session management ──────────────────────────────────────────────────────
static std::mutex g_sessionMtx;
static std::map<std::string, std::chrono::steady_clock::time_point> g_sessions;
static const int SESSION_LIFETIME_HOURS = 24;

std::string createSession() {
    std::string token = randomHex(32);
    std::lock_guard<std::mutex> lk(g_sessionMtx);
    g_sessions[token] =
        std::chrono::steady_clock::now() + std::chrono::hours(SESSION_LIFETIME_HOURS);
    return token;
}

bool validateSession(const std::string& token) {
    std::lock_guard<std::mutex> lk(g_sessionMtx);
    auto it = g_sessions.find(token);
    if (it == g_sessions.end()) return false;
    if (std::chrono::steady_clock::now() > it->second) {
        g_sessions.erase(it);
        return false;
    }
    return true;
}

void removeSession(const std::string& token) {
    std::lock_guard<std::mutex> lk(g_sessionMtx);
    g_sessions.erase(token);
}

std::string getSessionFromCookie(const httplib::Request& req) {
    auto it = req.headers.find("Cookie");
    if (it == req.headers.end()) return "";
    auto& cookie = it->second;
    auto pos = cookie.find("session=");
    if (pos == std::string::npos) return "";
    auto start = pos + 8;
    auto end = cookie.find(';', start);
    return cookie.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

// ── Credential helpers ──────────────────────────────────────────────────────
bool isConfigured(const Config& cfg) { return !cfg.credentials.empty(); }

bool setupCredentials(Config& cfg, const std::string& username, const std::string& password) {
    std::string salt = randomHex(16);
    std::string hash = sha256hex(salt + password);
    std::string plain = username + ":" + salt + ":" + hash;
    cfg.credentials = dpapiEncrypt(plain);
    return !cfg.credentials.empty();
}

bool verifyCredentials(const Config& cfg, const std::string& username,
                       const std::string& password) {
    std::string plain = dpapiDecrypt(cfg.credentials);
    if (plain.empty()) return false;
    auto p1 = plain.find(':');
    if (p1 == std::string::npos) return false;
    auto p2 = plain.find(':', p1 + 1);
    if (p2 == std::string::npos) return false;
    std::string storedUser = plain.substr(0, p1);
    std::string salt = plain.substr(p1 + 1, p2 - p1 - 1);
    std::string storedHash = plain.substr(p2 + 1);
    return username == storedUser && sha256hex(salt + password) == storedHash;
}

// ── PIN state ───────────────────────────────────────────────────────────────
static std::mutex g_pinMtx;
static std::string g_currentPin;
static std::chrono::steady_clock::time_point g_pinExpiry;

std::string generatePin() {
    std::lock_guard<std::mutex> lk(g_pinMtx);
    g_currentPin = randomDigits(4);
    g_pinExpiry = std::chrono::steady_clock::now() + std::chrono::minutes(5);
    return g_currentPin;
}

bool verifyPin(const std::string& pin) {
    std::lock_guard<std::mutex> lk(g_pinMtx);
    if (g_currentPin.empty() || std::chrono::steady_clock::now() > g_pinExpiry) return false;
    bool ok = (pin == g_currentPin);
    if (ok) g_currentPin.clear();
    return ok;
}

// ── Hex encode/decode ───────────────────────────────────────────────────────
std::string hexEncode(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(len * 2);
    char buf[3];
    for (size_t i = 0; i < len; i++) {
        sprintf(buf, "%02x", data[i]);
        out += buf;
    }
    return out;
}

bool hexDecode(const std::string& hex, uint8_t* out, size_t outLen) {
    if (hex.size() != outLen * 2) return false;
    for (size_t i = 0; i < outLen; i++) {
        unsigned int b;
        if (sscanf(hex.c_str() + i * 2, "%02x", &b) != 1) return false;
        out[i] = (uint8_t)b;
    }
    return true;
}

// ── libsodium initialization ────────────────────────────────────────────────
bool sodiumInit() {
    return sodium_init() >= 0; // returns 0 on first init, 1 if already initialized
}

// ── X25519 key pair generation ──────────────────────────────────────────────
void generateKeyPair(uint8_t pk[32], uint8_t sk[32]) {
    crypto_kx_keypair(pk, sk);
}

// ── Compute shared key from X25519 key exchange (server side) ───────────────
bool computeSharedKey(uint8_t sharedKey[32],
                      const uint8_t clientPk[32], const uint8_t serverSk[32],
                      const uint8_t serverPk[32]) {
    // Server uses rx key (what client sends is what server receives)
    uint8_t rx[32], tx[32];
    if (crypto_kx_server_session_keys(rx, tx, serverPk, serverSk, clientPk) != 0)
        return false;
    // Use rx as the shared key (client encrypts with tx, server decrypts with rx)
    // For simplicity, we use rx for both directions (symmetric)
    memcpy(sharedKey, rx, 32);
    return true;
}

// ── Generate a random 4-byte token ──────────────────────────────────────────
uint32_t generateToken() {
    uint32_t token;
    randombytes_buf(&token, sizeof(token));
    // Avoid token 0 (reserved for "no token")
    while (token == 0) randombytes_buf(&token, sizeof(token));
    return token;
}

// ── Encrypt a packet (ChaCha20-Poly1305 IETF) ──────────────────────────────
bool encryptPacket(const uint8_t key[32], uint32_t counter,
                   uint32_t token,
                   const uint8_t* plaintext, size_t ptLen,
                   uint8_t* ciphertext, unsigned long long* ctLen) {
    // Build 12-byte nonce: zero-pad the 4-byte counter (big-endian, right-aligned)
    uint8_t nonce[12] = {};
    nonce[8]  = (uint8_t)(counter >> 24);
    nonce[9]  = (uint8_t)(counter >> 16);
    nonce[10] = (uint8_t)(counter >> 8);
    nonce[11] = (uint8_t)(counter);

    // AAD is the 4-byte token (big-endian)
    uint8_t aad[4];
    aad[0] = (uint8_t)(token >> 24);
    aad[1] = (uint8_t)(token >> 16);
    aad[2] = (uint8_t)(token >> 8);
    aad[3] = (uint8_t)(token);

    return crypto_aead_chacha20poly1305_ietf_encrypt(
        ciphertext, ctLen,
        plaintext, ptLen,
        aad, sizeof(aad),
        nullptr, nonce, key) == 0;
}

// ── Decrypt a packet (ChaCha20-Poly1305 IETF) ──────────────────────────────
bool decryptPacket(const uint8_t key[32], uint32_t counter,
                   uint32_t token,
                   const uint8_t* ciphertext, size_t ctLen,
                   uint8_t* plaintext, unsigned long long* ptLen) {
    uint8_t nonce[12] = {};
    nonce[8]  = (uint8_t)(counter >> 24);
    nonce[9]  = (uint8_t)(counter >> 16);
    nonce[10] = (uint8_t)(counter >> 8);
    nonce[11] = (uint8_t)(counter);

    uint8_t aad[4];
    aad[0] = (uint8_t)(token >> 24);
    aad[1] = (uint8_t)(token >> 16);
    aad[2] = (uint8_t)(token >> 8);
    aad[3] = (uint8_t)(token);

    return crypto_aead_chacha20poly1305_ietf_decrypt(
        plaintext, ptLen,
        nullptr,
        ciphertext, ctLen,
        aad, sizeof(aad),
        nonce, key) == 0;
}
