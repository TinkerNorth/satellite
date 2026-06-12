// SPDX-License-Identifier: LGPL-3.0-or-later
#include "session_crypto.h"

#include <sodium.h>

#include <cstring>

namespace {

void hmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* msg, size_t msgLen,
                uint8_t out[crypto_auth_hmacsha256_BYTES]) {
    crypto_auth_hmacsha256_state st;
    crypto_auth_hmacsha256_init(&st, key, keyLen);
    crypto_auth_hmacsha256_update(&st, msg, msgLen);
    crypto_auth_hmacsha256_final(&st, out);
}

int hexNibbleAt(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

void deriveSessionKey(const uint8_t pairingKey[CRYPTO_KEY_SIZE],
                      const uint8_t sessionSalt[SESSION_SALT_SIZE], uint32_t token,
                      uint8_t outSessionKey[CRYPTO_KEY_SIZE]) {
    // RFC 5869: PRK = HMAC(salt, IKM); OKM(32) = T1 = HMAC(PRK, info || 0x01).
    uint8_t prk[crypto_auth_hmacsha256_BYTES];
    hmacSha256(sessionSalt, SESSION_SALT_SIZE, pairingKey, CRYPTO_KEY_SIZE, prk);

    static const char infoLabel[] = "satellite-session-v1";
    uint8_t info[sizeof(infoLabel) - 1 + 4 + 1];
    std::memcpy(info, infoLabel, sizeof(infoLabel) - 1);
    info[sizeof(infoLabel) - 1 + 0] = (uint8_t)(token >> 24);
    info[sizeof(infoLabel) - 1 + 1] = (uint8_t)(token >> 16);
    info[sizeof(infoLabel) - 1 + 2] = (uint8_t)(token >> 8);
    info[sizeof(infoLabel) - 1 + 3] = (uint8_t)(token);
    info[sizeof(infoLabel) - 1 + 4] = 0x01;

    uint8_t t1[crypto_auth_hmacsha256_BYTES];
    hmacSha256(prk, sizeof(prk), info, sizeof(info), t1);
    static_assert(sizeof(t1) == CRYPTO_KEY_SIZE, "one HKDF block fills the session key");
    std::memcpy(outSessionKey, t1, CRYPTO_KEY_SIZE);
    sodium_memzero(prk, sizeof(prk));
    sodium_memzero(t1, sizeof(t1));
}

std::string computeHmacProof(const uint8_t pairingKey[CRYPTO_KEY_SIZE],
                             const std::string& deviceId) {
    const std::string msg = "satellite-proof:" + deviceId;
    uint8_t mac[crypto_auth_hmacsha256_BYTES];
    hmacSha256(pairingKey, CRYPTO_KEY_SIZE, reinterpret_cast<const uint8_t*>(msg.data()),
               msg.size(), mac);
    char hex[crypto_auth_hmacsha256_BYTES * 2 + 1];
    sodium_bin2hex(hex, sizeof(hex), mac, sizeof(mac));
    sodium_memzero(mac, sizeof(mac));
    return std::string(hex);
}

bool verifyHmacProof(const uint8_t pairingKey[CRYPTO_KEY_SIZE], const std::string& deviceId,
                     const std::string& proofHex) {
    if (proofHex.size() != crypto_auth_hmacsha256_BYTES * 2) return false;
    uint8_t supplied[crypto_auth_hmacsha256_BYTES];
    for (size_t i = 0; i < sizeof(supplied); i++) {
        int hi = hexNibbleAt(proofHex[i * 2]);
        int lo = hexNibbleAt(proofHex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        supplied[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    const std::string msg = "satellite-proof:" + deviceId;
    uint8_t expected[crypto_auth_hmacsha256_BYTES];
    hmacSha256(pairingKey, CRYPTO_KEY_SIZE, reinterpret_cast<const uint8_t*>(msg.data()),
               msg.size(), expected);
    const bool ok = sodium_memcmp(supplied, expected, sizeof(expected)) == 0;
    sodium_memzero(expected, sizeof(expected));
    return ok;
}

namespace {

void buildNonceAndAad(uint8_t direction, uint32_t counter, uint32_t token,
                      uint8_t nonce[CRYPTO_NONCE_SIZE], uint8_t aad[4]) {
    std::memset(nonce, 0, CRYPTO_NONCE_SIZE);
    nonce[0] = direction;
    nonce[8] = (uint8_t)(counter >> 24);
    nonce[9] = (uint8_t)(counter >> 16);
    nonce[10] = (uint8_t)(counter >> 8);
    nonce[11] = (uint8_t)(counter);
    aad[0] = (uint8_t)(token >> 24);
    aad[1] = (uint8_t)(token >> 16);
    aad[2] = (uint8_t)(token >> 8);
    aad[3] = (uint8_t)(token);
}

} // namespace

bool encryptPacket(const uint8_t key[CRYPTO_KEY_SIZE], uint8_t direction, uint32_t counter,
                   uint32_t token, const uint8_t* plaintext, size_t ptLen, uint8_t* ciphertext,
                   unsigned long long* ctLen) {
    uint8_t nonce[CRYPTO_NONCE_SIZE];
    uint8_t aad[4];
    buildNonceAndAad(direction, counter, token, nonce, aad);
    return crypto_aead_chacha20poly1305_ietf_encrypt(ciphertext, ctLen, plaintext, ptLen, aad,
                                                     sizeof(aad), nullptr, nonce, key) == 0;
}

bool decryptPacket(const uint8_t key[CRYPTO_KEY_SIZE], uint8_t direction, uint32_t counter,
                   uint32_t token, const uint8_t* ciphertext, size_t ctLen, uint8_t* plaintext,
                   unsigned long long* ptLen) {
    uint8_t nonce[CRYPTO_NONCE_SIZE];
    uint8_t aad[4];
    buildNonceAndAad(direction, counter, token, nonce, aad);
    return crypto_aead_chacha20poly1305_ietf_decrypt(plaintext, ptLen, nullptr, ciphertext, ctLen,
                                                     aad, sizeof(aad), nonce, key) == 0;
}
