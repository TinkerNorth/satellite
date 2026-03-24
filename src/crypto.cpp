/*
 * crypto.cpp — DPAPI, SHA-256, sessions, PIN, credentials
 */
#include "crypto.h"

// ── Base64 helpers ──────────────────────────────────────────────────────────
static const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const std::vector<BYTE>& data) {
    std::string out;
    int val = 0, bits = -6;
    for (BYTE c : data) {
        val = (val << 8) + c;
        bits += 8;
        while (bits >= 0) { out.push_back(b64chars[(val >> bits) & 0x3F]); bits -= 6; }
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
        if (bits >= 0) { out.push_back((BYTE)(val >> bits)); bits -= 8; }
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
    for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", hash[i]);
    hex[64] = 0;
    return std::string(hex);
}

// ── DPAPI encrypt/decrypt ───────────────────────────────────────────────────
std::string dpapiEncrypt(const std::string& plaintext) {
    DATA_BLOB in, out;
    in.pbData = (BYTE*)plaintext.data();
    in.cbData = (DWORD)plaintext.size();
    if (!CryptProtectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out))
        return "";
    std::vector<BYTE> blob(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    return base64Encode(blob);
}

std::string dpapiDecrypt(const std::string& encoded) {
    auto blob = base64Decode(encoded);
    DATA_BLOB in, out;
    in.pbData = blob.data();
    in.cbData = (DWORD)blob.size();
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out))
        return "";
    std::string result((char*)out.pbData, out.cbData);
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
    for (int i = 0; i < bytes; i++) { sprintf(buf, "%02x", dis(gen)); out += buf; }
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
    g_sessions[token] = std::chrono::steady_clock::now() + std::chrono::hours(SESSION_LIFETIME_HOURS);
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
bool isConfigured(const Config& cfg) {
    return !cfg.credentials.empty();
}

bool setupCredentials(Config& cfg, const std::string& username, const std::string& password) {
    std::string salt = randomHex(16);
    std::string hash = sha256hex(salt + password);
    std::string plain = username + ":" + salt + ":" + hash;
    cfg.credentials = dpapiEncrypt(plain);
    return !cfg.credentials.empty();
}

bool verifyCredentials(const Config& cfg, const std::string& username, const std::string& password) {
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

