/*
 * crypto.h — SHA-256, sessions, PIN, credentials, libsodium wrappers (macOS).
 *
 * Mirrors the Windows API surface (crypto.h) so files under src/net/ compile
 * unchanged.
 * Where Windows uses DPAPI + BCrypt, macOS uses libsodium primitives backed by
 * a per-user keyfile stored in the app-support directory.
 */
#pragma once
#include "globals.h"

#include <cstdint>

// ── Crypto ──────────────────────────────────────────────────────────────────
std::string sha256hex(const std::string& input);

// Encrypt/decrypt arbitrary-length plaintext for local persistence.
// Windows uses DPAPI; macOS uses libsodium secretbox + local keyfile.
std::string dpapiEncrypt(const std::string& plaintext);
std::string dpapiDecrypt(const std::string& encoded);

// ── Random ──────────────────────────────────────────────────────────────────
std::string randomHex(int bytes);
std::string randomDigits(int n);

// ── Session management ──────────────────────────────────────────────────────
std::string createSession();
bool validateSession(const std::string& token);
void removeSession(const std::string& token);
std::string getSessionFromCookie(const httplib::Request& req);

// ── Credential helpers ──────────────────────────────────────────────────────
bool isConfigured(const Config& cfg);
bool setupCredentials(Config& cfg, const std::string& username, const std::string& password);
bool verifyCredentials(const Config& cfg, const std::string& username, const std::string& password);

// ── PIN ─────────────────────────────────────────────────────────────────────
std::string generatePin();
bool verifyPin(const std::string& pin);

// ── Hex encode/decode ───────────────────────────────────────────────────────
std::string hexEncode(const uint8_t* data, size_t len);
bool hexDecode(const std::string& hex, uint8_t* out, size_t outLen);

// ── libsodium / ChaCha20-Poly1305 ──────────────────────────────────────────
bool sodiumInit();
void generateKeyPair(uint8_t pk[32], uint8_t sk[32]);
bool computeSharedKey(uint8_t sharedKey[32], const uint8_t clientPk[32], const uint8_t serverSk[32],
                      const uint8_t serverPk[32]);
uint32_t generateToken();
bool encryptPacket(const uint8_t key[32], uint32_t counter, uint32_t token,
                   const uint8_t* plaintext, size_t ptLen, uint8_t* ciphertext,
                   unsigned long long* ctLen);
bool decryptPacket(const uint8_t key[32], uint32_t counter, uint32_t token,
                   const uint8_t* ciphertext, size_t ctLen, uint8_t* plaintext,
                   unsigned long long* ptLen);
