/*
 * crypto.h — DPAPI, SHA-256, sessions, PIN, credentials
 */
#pragma once
#include "globals.h"

// ── Base64 ──────────────────────────────────────────────────────────────────
std::string base64Encode(const std::vector<BYTE>& data);
std::vector<BYTE> base64Decode(const std::string& s);

// ── Crypto ──────────────────────────────────────────────────────────────────
std::string sha256hex(const std::string& input);
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

