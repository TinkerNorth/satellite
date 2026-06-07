// SPDX-License-Identifier: LGPL-3.0-or-later
// Mirrors the Windows crypto.h API surface so files under src/net/ compile
// unchanged; Linux backs DPAPI+BCrypt's role with libsodium + a per-user
// keyfile in the XDG config dir.
#pragma once
#include "globals.h"

#include <cstdint>

std::string sha256hex(const std::string& input);

// Encrypt/decrypt arbitrary-length plaintext for local persistence
// (libsodium secretbox + local keyfile; DPAPI on Windows).
std::string dpapiEncrypt(const std::string& plaintext);
std::string dpapiDecrypt(const std::string& encoded);

std::string randomHex(int bytes);
std::string randomDigits(int n);

std::string generatePin();
bool verifyPin(const std::string& pin);

// secondsRemaining is 0 unless state == PinState::PinActive.
struct PinSnapshot {
    PinState state = PinState::PinIdle;
    int secondsRemaining = 0;
};
PinSnapshot pinSnapshot();

std::string hexEncode(const uint8_t* data, size_t len);
bool hexDecode(const std::string& hex, uint8_t* out, size_t outLen);

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
