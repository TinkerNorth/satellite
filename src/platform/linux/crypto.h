// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * crypto.h — SHA-256, sessions, PIN, credentials, libsodium wrappers (Linux).
 *
 * Mirrors the Windows API surface (crypto.h) so files under src/net/ compile
 * unchanged.
 * Where Windows uses DPAPI + BCrypt, Linux uses libsodium primitives backed by
 * a per-user keyfile stored in the XDG config directory.
 */
#pragma once
#include "globals.h"

#include <cstdint>

// ── Crypto ──────────────────────────────────────────────────────────────────
std::string sha256hex(const std::string& input);

// Encrypt/decrypt arbitrary-length plaintext for local persistence.
// Windows uses DPAPI; Linux uses libsodium secretbox + local keyfile.
std::string dpapiEncrypt(const std::string& plaintext);
std::string dpapiDecrypt(const std::string& encoded);

// ── Random ──────────────────────────────────────────────────────────────────
std::string randomHex(int bytes);
std::string randomDigits(int n);

// ── PIN ─────────────────────────────────────────────────────────────────────
std::string generatePin();
bool verifyPin(const std::string& pin);

// Snapshot of the in-process PIN state (for /api/pin/status). Mirrors the
// PinState enum in core/types.h. `secondsRemaining` is 0 unless the state is
// PinState::PinActive; the dashboard uses it to render an "Expires in m:ss"
// countdown.
struct PinSnapshot {
    PinState state = PinState::PinIdle;
    int secondsRemaining = 0;
};
PinSnapshot pinSnapshot();

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
