// SPDX-License-Identifier: LGPL-3.0-or-later

// Session-key derivation + REST proof-of-key-possession (docs/contract.md).
// Shared by every platform; libsodium-backed, kept out of core/ so the core
// stays libsodium-free.
#pragma once

#include "core/types.h"

#include <cstdint>
#include <string>

// sessionKey = HKDF-SHA256(ikm = pairingKey, salt = sessionSalt,
//                          info = "satellite-session-v1" || token(4 BE)).
// RFC 5869 extract-then-expand, single 32-byte output block. Both ends derive
// the same key from the PUT response's token + sessionSalt.
void deriveSessionKey(const uint8_t pairingKey[CRYPTO_KEY_SIZE],
                      const uint8_t sessionSalt[SESSION_SALT_SIZE], uint32_t token,
                      uint8_t outSessionKey[CRYPTO_KEY_SIZE]);

// hex( HMAC-SHA256( pairingKey, "satellite-proof:" + deviceId ) ).
std::string computeHmacProof(const uint8_t pairingKey[CRYPTO_KEY_SIZE],
                             const std::string& deviceId);

// Constant-time verify of a client-supplied hex proof. False on malformed hex.
bool verifyHmacProof(const uint8_t pairingKey[CRYPTO_KEY_SIZE], const std::string& deviceId,
                     const std::string& proofHex);

// UDP packet AEAD (ChaCha20-Poly1305-IETF), one shared implementation for all
// platforms. Nonce = direction(1) | 0x7 | counter(4 BE); AAD = token(4 BE).
// The direction byte (CRYPTO_DIR_*) keeps the two directions of one session
// key from ever sharing a nonce.
bool encryptPacket(const uint8_t key[CRYPTO_KEY_SIZE], uint8_t direction, uint32_t counter,
                   uint32_t token, const uint8_t* plaintext, size_t ptLen, uint8_t* ciphertext,
                   unsigned long long* ctLen);
bool decryptPacket(const uint8_t key[CRYPTO_KEY_SIZE], uint8_t direction, uint32_t counter,
                   uint32_t token, const uint8_t* ciphertext, size_t ctLen, uint8_t* plaintext,
                   unsigned long long* ptLen);
