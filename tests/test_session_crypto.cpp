// SPDX-License-Identifier: LGPL-3.0-or-later
// Portable session-crypto interop suite (net/session_crypto.cpp), compiled and
// run on EVERY CI lane (Linux, Windows MinGW+MSVC, macOS) so a cross-end drift
// breaks all of them, not just one.
//
// The pinned vectors are shared verbatim with dish-linux
// (tests/test_session_crypto.cpp), dish-windows, dish-android
// (SessionCryptoTest) and dish-mac (DishCoreTests): they are the executable
// form of docs/contract.md §Crypto. Any change here is a wire-protocol break,
// never a refactor.
#include "../src/net/session_crypto.h"

#include <sodium.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

#include "test_util.h"

// Local hex helper so this suite never touches a platform crypto.h.
static std::string hexLower(const uint8_t* p, size_t n) {
    static const char* const digits = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; i++) {
        s.push_back(digits[p[i] >> 4]);
        s.push_back(digits[p[i] & 0x0F]);
    }
    return s;
}

// pairingKey = 01 02 .. 20 — the shared interop key all ends pin against.
static const uint8_t SC_KEY[CRYPTO_KEY_SIZE] = {1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11,
                                                12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22,
                                                23, 24, 25, 26, 27, 28, 29, 30, 31, 32};

static void testSessionKeyDerivation() {
    const uint8_t salt[SESSION_SALT_SIZE] = {0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x07, 0x18};

    TEST("deriveSessionKey: deterministic for fixed inputs");
    uint8_t k1[CRYPTO_KEY_SIZE], k2[CRYPTO_KEY_SIZE];
    deriveSessionKey(SC_KEY, salt, 0x12345678, k1);
    deriveSessionKey(SC_KEY, salt, 0x12345678, k2);
    EXPECT(std::memcmp(k1, k2, CRYPTO_KEY_SIZE) == 0);

    TEST("deriveSessionKey: never the raw pairing key");
    EXPECT(std::memcmp(k1, SC_KEY, CRYPTO_KEY_SIZE) != 0);

    TEST("deriveSessionKey: token changes the key");
    deriveSessionKey(SC_KEY, salt, 0x12345679, k2);
    EXPECT(std::memcmp(k1, k2, CRYPTO_KEY_SIZE) != 0);

    TEST("deriveSessionKey: salt changes the key");
    uint8_t salt2[SESSION_SALT_SIZE] = {0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x07, 0x19};
    deriveSessionKey(SC_KEY, salt2, 0x12345678, k2);
    EXPECT(std::memcmp(k1, k2, CRYPTO_KEY_SIZE) != 0);

    // Pinned vector: independently computed RFC 5869 HKDF-SHA256 with
    // ikm = 01..20, salt = a1b2c3d4e5f60718, info = "satellite-session-v1" ||
    // 12345678. Any drift here is a cross-end break (dish derives the same).
    TEST("deriveSessionKey: pinned interop vector");
    EXPECT_EQ(hexLower(k1, CRYPTO_KEY_SIZE),
              std::string("946f704cf07e2dde5e9995a70d3d103753b4687a7ed9656bc6481b06065a8584"));
}

static void testHmacProof() {
    TEST("computeHmacProof: 64 hex chars, deterministic");
    std::string p1 = computeHmacProof(SC_KEY, "device-1");
    std::string p2 = computeHmacProof(SC_KEY, "device-1");
    EXPECT_EQ(p1.size(), size_t{64});
    EXPECT_EQ(p1, p2);

    TEST("computeHmacProof: device-bound");
    EXPECT(computeHmacProof(SC_KEY, "device-2") != p1);

    TEST("verifyHmacProof: accepts the matching proof");
    EXPECT(verifyHmacProof(SC_KEY, "device-1", p1));

    TEST("verifyHmacProof: rejects a diverged key");
    uint8_t other[CRYPTO_KEY_SIZE];
    std::memcpy(other, SC_KEY, CRYPTO_KEY_SIZE);
    other[0] ^= 0xFF;
    EXPECT(!verifyHmacProof(other, "device-1", p1));

    TEST("verifyHmacProof: rejects the wrong device id");
    EXPECT(!verifyHmacProof(SC_KEY, "device-2", p1));

    TEST("verifyHmacProof: rejects malformed hex");
    EXPECT(!verifyHmacProof(SC_KEY, "device-1", ""));
    EXPECT(!verifyHmacProof(SC_KEY, "device-1", "abc"));
    std::string bad = p1;
    bad[0] = 'z';
    EXPECT(!verifyHmacProof(SC_KEY, "device-1", bad));

    TEST("verifyHmacProof: pinned interop vector");
    // Independently computed HMAC-SHA256(key=01..20, "satellite-proof:device-1").
    EXPECT_EQ(computeHmacProof(SC_KEY, "device-1"),
              std::string("05a035a10c55fdfe254c9df5df55a614ac128b123a5de225ea33b41f1d4eedde"));
}

static void testPacketAeadDirections() {
    const uint8_t key[CRYPTO_KEY_SIZE] = {9, 9, 9};
    const uint8_t plain[] = {0x00, 0x02, 0x00, 0x00}; // a heartbeat inner
    uint8_t ct[64];
    unsigned long long ctLen = 0;

    TEST("packet AEAD: roundtrip with matching direction");
    EXPECT(encryptPacket(key, CRYPTO_DIR_CLIENT_TO_SERVER, 1, 0xAABBCCDD, plain, sizeof(plain), ct,
                         &ctLen));
    uint8_t pt[64];
    unsigned long long ptLen = 0;
    EXPECT(decryptPacket(key, CRYPTO_DIR_CLIENT_TO_SERVER, 1, 0xAABBCCDD, ct, (size_t)ctLen, pt,
                         &ptLen));
    EXPECT_EQ((size_t)ptLen, sizeof(plain));
    EXPECT(std::memcmp(pt, plain, sizeof(plain)) == 0);

    TEST("packet AEAD: ciphertext is plaintext + 16-byte Poly1305 tag");
    EXPECT_EQ((size_t)ctLen, sizeof(plain) + (size_t)AUTH_TAG_SIZE);

    TEST("packet AEAD: direction mismatch fails authentication");
    EXPECT(!decryptPacket(key, CRYPTO_DIR_SERVER_TO_CLIENT, 1, 0xAABBCCDD, ct, (size_t)ctLen, pt,
                          &ptLen));

    TEST("packet AEAD: counter mismatch fails authentication");
    EXPECT(!decryptPacket(key, CRYPTO_DIR_CLIENT_TO_SERVER, 2, 0xAABBCCDD, ct, (size_t)ctLen, pt,
                          &ptLen));

    TEST("packet AEAD: token (AAD) mismatch fails authentication");
    EXPECT(!decryptPacket(key, CRYPTO_DIR_CLIENT_TO_SERVER, 1, 0xAABBCCDE, ct, (size_t)ctLen, pt,
                          &ptLen));

    TEST("packet AEAD: same (counter, key) in the two directions yields distinct ciphertext");
    uint8_t ct2[64];
    unsigned long long ct2Len = 0;
    EXPECT(encryptPacket(key, CRYPTO_DIR_SERVER_TO_CLIENT, 1, 0xAABBCCDD, plain, sizeof(plain), ct2,
                         &ct2Len));
    EXPECT_EQ(ctLen, ct2Len);
    EXPECT(std::memcmp(ct, ct2, (size_t)ctLen) != 0);
}

static void testPacketAeadTampering() {
    const uint8_t key[CRYPTO_KEY_SIZE] = {9, 9, 9};
    const uint8_t plain[] = {0x00, 0x01, 0x00, 0x0C, 0xDE, 0xAD};
    uint8_t ct[64];
    unsigned long long ctLen = 0;
    EXPECT(encryptPacket(key, CRYPTO_DIR_CLIENT_TO_SERVER, 7, 0x01020304, plain, sizeof(plain), ct,
                         &ctLen));
    uint8_t pt[64];
    unsigned long long ptLen = 0;

    TEST("packet AEAD: a flipped ciphertext bit fails authentication");
    ct[0] ^= 0x01;
    EXPECT(!decryptPacket(key, CRYPTO_DIR_CLIENT_TO_SERVER, 7, 0x01020304, ct, (size_t)ctLen, pt,
                          &ptLen));
    ct[0] ^= 0x01;

    TEST("packet AEAD: a flipped tag bit fails authentication");
    ct[ctLen - 1] ^= 0x80;
    EXPECT(!decryptPacket(key, CRYPTO_DIR_CLIENT_TO_SERVER, 7, 0x01020304, ct, (size_t)ctLen, pt,
                          &ptLen));
    ct[ctLen - 1] ^= 0x80;

    TEST("packet AEAD: a truncated box (shorter than the tag) is rejected");
    EXPECT(!decryptPacket(key, CRYPTO_DIR_CLIENT_TO_SERVER, 7, 0x01020304, ct,
                          (size_t)AUTH_TAG_SIZE - 1, pt, &ptLen));

    TEST("packet AEAD: a wrong key fails authentication");
    uint8_t otherKey[CRYPTO_KEY_SIZE] = {9, 9, 8};
    EXPECT(!decryptPacket(otherKey, CRYPTO_DIR_CLIENT_TO_SERVER, 7, 0x01020304, ct, (size_t)ctLen,
                          pt, &ptLen));

    TEST("packet AEAD: empty plaintext round-trips (tag-only box)");
    unsigned long long emptyCtLen = 0;
    EXPECT(encryptPacket(key, CRYPTO_DIR_SERVER_TO_CLIENT, 1, 0x01020304, nullptr, 0, ct,
                         &emptyCtLen));
    EXPECT_EQ((size_t)emptyCtLen, (size_t)AUTH_TAG_SIZE);
    EXPECT(decryptPacket(key, CRYPTO_DIR_SERVER_TO_CLIENT, 1, 0x01020304, ct, (size_t)emptyCtLen,
                         pt, &ptLen));
    EXPECT_EQ((size_t)ptLen, size_t{0});
}

// Mirrors dish-linux's "client-to-server send framing" case: the FIRST packet
// of a session uses counter 1 (counters start at 1, NOT 0 — the pre-protocol-1
// off-by-one), direction CLIENT_TO_SERVER, AAD = token(4 BE).
static void testFirstPacketCounterOrigin() {
    uint8_t sessionKey[CRYPTO_KEY_SIZE] = {};
    sessionKey[0] = 0xAB; // arbitrary derived key
    const uint32_t token = 0x0007A1B2;
    const uint8_t inner[4] = {0x00, 0x02, 0x00, 0x00}; // heartbeat inner frame

    uint8_t ct[64];
    unsigned long long ctLen = 0;
    EXPECT(encryptPacket(sessionKey, CRYPTO_DIR_CLIENT_TO_SERVER, /*counter=*/1, token, inner,
                         sizeof(inner), ct, &ctLen));

    TEST("first packet: decrypts at counter 1 with the client-to-server direction");
    uint8_t pt[64];
    unsigned long long ptLen = 0;
    EXPECT(decryptPacket(sessionKey, CRYPTO_DIR_CLIENT_TO_SERVER, 1, token, ct, (size_t)ctLen, pt,
                         &ptLen));
    EXPECT_EQ((size_t)ptLen, sizeof(inner));
    EXPECT(std::memcmp(pt, inner, sizeof(inner)) == 0);

    TEST("first packet: counter 0 (the old origin) must NOT decrypt");
    EXPECT(!decryptPacket(sessionKey, CRYPTO_DIR_CLIENT_TO_SERVER, 0, token, ct, (size_t)ctLen, pt,
                          &ptLen));

    TEST("first packet: the opposite direction must NOT decrypt");
    EXPECT(!decryptPacket(sessionKey, CRYPTO_DIR_SERVER_TO_CLIENT, 1, token, ct, (size_t)ctLen, pt,
                          &ptLen));
}

int main() {
    std::cout << "Running portable session-crypto interop tests...\n\n";

    if (sodium_init() < 0) {
        std::cerr << "sodium init failed\n";
        return 1;
    }

    testSessionKeyDerivation();
    testHmacProof();
    testPacketAeadDirections();
    testPacketAeadTampering();
    testFirstPacketCounterOrigin();

    std::cout << "\n=== Test Results ===\n";
    std::cout << "  Passed: " << g_pass << "\n";
    std::cout << "  Failed: " << g_fail << "\n";
    std::cout << "  STATUS: " << (g_fail == 0 ? "ALL PASSED" : "FAILED") << "\n";
    return g_fail == 0 ? 0 : 1;
}
