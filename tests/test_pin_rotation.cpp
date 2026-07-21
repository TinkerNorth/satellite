// SPDX-License-Identifier: LGPL-3.0-or-later
// Portable suite for net/pin_rotation.{h,cpp}, the D11 hoist of the logic the
// three platform crypto.cpp files used to triplicate: PIN rotation semantics,
// the hex codecs (including the strict malformed-hex rejection the
// unification standardized on), CSPRNG string helpers, and the X25519
// pairing-key exchange. Runs on EVERY lane (Linux, MinGW, MSVC, macOS) so a
// platform can no longer drift its own copy — the platform suites keep their
// own PIN tests as integration proof through "crypto.h".
//
// PIN time travel goes through the backdatePinClockForTest seam
// (SATELLITE_BUILD_TESTS), never real sleeps. Collision note (same as the
// platform suites): a re-mint draws 4 random digits, so assertions that "the
// old PIN stopped working" are gated on the mint actually differing.
#include "../src/net/pin_rotation.h"

#include <sodium.h>

#include <iostream>
#include <string>

#include "test_util.h"

// Returns a 4-digit PIN guaranteed != both snapshot PINs (flips the first
// digit of current; if that collides with previous, flips one more step).
static std::string wrongPinFor(const PinSnapshot& s) {
    std::string w = s.currentPin;
    for (int step = 1; step <= 2; step++) {
        w[0] = static_cast<char>('0' + ((s.currentPin[0] - '0') + step) % 10);
        if (w != s.previousPin) return w;
    }
    return w;
}

static void testRandomHelpers() {
    TEST("randomHex: length and charset");
    std::string h = randomHex(8);
    EXPECT_EQ(h.size(), size_t{16});
    EXPECT(h.find_first_not_of("0123456789abcdef") == std::string::npos);

    TEST("randomHex: draws fresh bytes per call");
    EXPECT(randomHex(8) != h);

    TEST("randomDigits: length and charset");
    std::string d = randomDigits(4);
    EXPECT_EQ(d.size(), size_t{4});
    EXPECT(d.find_first_not_of("0123456789") == std::string::npos);

    TEST("randomDigits: zero-length is empty");
    EXPECT_EQ(randomDigits(0), std::string(""));
}

static void testHexCodec() {
    TEST("hexEncode: known vector");
    const uint8_t in[] = {0x00, 0x0f, 0xa5, 0xff};
    EXPECT_EQ(hexEncode(in, sizeof(in)), std::string("000fa5ff"));

    TEST("hexEncode: empty input");
    EXPECT_EQ(hexEncode(in, 0), std::string(""));

    TEST("hexDecode: roundtrip of hexEncode");
    uint8_t out[4] = {0};
    EXPECT(hexDecode("000fa5ff", out, sizeof(out)));
    EXPECT_EQ((int)out[0], 0x00);
    EXPECT_EQ((int)out[1], 0x0f);
    EXPECT_EQ((int)out[2], 0xa5);
    EXPECT_EQ((int)out[3], 0xff);

    TEST("hexDecode: uppercase accepted");
    uint8_t up[2] = {0};
    EXPECT(hexDecode("A5FF", up, sizeof(up)));
    EXPECT_EQ((int)up[0], 0xa5);
    EXPECT_EQ((int)up[1], 0xff);

    TEST("hexDecode: wrong length rejected");
    uint8_t two[2] = {0};
    EXPECT(!hexDecode("abc", two, sizeof(two)));
    EXPECT(!hexDecode("", two, sizeof(two)));

    TEST("hexDecode: non-hex rejected");
    uint8_t two2[2] = {0};
    EXPECT(!hexDecode("zz00", two2, sizeof(two2)));

    TEST("hexDecode: strict — a valid digit followed by junk is rejected");
    // The pre-hoist POSIX sscanf("%02x") copies accepted "0g" as 0x00; the
    // unified parser must reject every malformed pair, wherever the junk sits.
    uint8_t strict[2] = {0};
    EXPECT(!hexDecode("0gff", strict, sizeof(strict)));
    EXPECT(!hexDecode("ff0g", strict, sizeof(strict)));
    EXPECT(!hexDecode(" 1ff", strict, sizeof(strict))); // sscanf skipped spaces
}

static void testKeyExchange() {
    TEST("generateKeyPair: distinct non-zero keys per call");
    uint8_t pk1[32], sk1[32], pk2[32], sk2[32];
    generateKeyPair(pk1, sk1);
    generateKeyPair(pk2, sk2);
    EXPECT(hexEncode(pk1, 32) != hexEncode(pk2, 32));
    EXPECT(hexEncode(pk1, 32) != std::string(64, '0'));

    TEST("computeSharedKey: server derivation matches the client's tx key");
    uint8_t serverPk[32], serverSk[32], clientPk[32], clientSk[32];
    generateKeyPair(serverPk, serverSk);
    generateKeyPair(clientPk, clientSk);
    uint8_t shared[32];
    EXPECT(computeSharedKey(shared, clientPk, serverSk, serverPk));
    uint8_t rx[32], tx[32];
    EXPECT(crypto_kx_client_session_keys(rx, tx, clientPk, clientSk, serverPk) == 0);
    EXPECT_EQ(hexEncode(shared, 32), hexEncode(tx, 32));

    TEST("computeSharedKey: rejects a low-order/degenerate client key");
    uint8_t zeroPk[32] = {};
    uint8_t out[32];
    EXPECT(!computeSharedKey(out, zeroPk, serverSk, serverPk));
}

static void testGenerateToken() {
    TEST("generateToken: never the reserved zero");
    for (int i = 0; i < 64; i++) EXPECT(generateToken() != 0u);

    TEST("generateToken: draws fresh values");
    EXPECT(generateToken() != generateToken() || generateToken() != generateToken());
}

// ---- PIN rotation semantics (ported from the macOS platform suite so the
// shared TU carries the coverage on every lane) ------------------------------

static void testPinSnapshotInitial() {
    // Consume any prior state: a successful verify resets to a fresh pair.
    PinSnapshot pre = pinSnapshot();
    verifyPin(pre.currentPin);

    PinSnapshot s = pinSnapshot();
    TEST("pinSnapshot: current is a 4-digit PIN");
    EXPECT_EQ(s.currentPin.size(), size_t{4});
    EXPECT(s.currentPin.find_first_not_of("0123456789") == std::string::npos);

    TEST("pinSnapshot: previous is empty before the first rotation");
    EXPECT_EQ(s.previousPin, std::string(""));

    TEST("pinSnapshot: rotation countdown is within (0, 300]");
    EXPECT(s.secondsRemaining > 0);
    EXPECT(s.secondsRemaining <= 300);

    TEST("pinSnapshot: does not rotate before the period lapses");
    PinSnapshot s2 = pinSnapshot();
    EXPECT_EQ(s2.currentPin, s.currentPin);
}

static void testPinRotationKeepsPrevious() {
    verifyPin(pinSnapshot().currentPin); // fresh pair, previous empty
    PinSnapshot before = pinSnapshot();

    // One period + 10 s: due, and within the previous-PIN validity window.
    backdatePinClockForTest(310);
    PinSnapshot after = pinSnapshot();

    TEST("rotation due: outgoing PIN is kept as previous for one period");
    EXPECT_EQ(after.previousPin, before.currentPin);
    EXPECT_EQ(after.currentPin.size(), size_t{4});

    TEST("rotation due: countdown restarts");
    EXPECT(after.secondsRemaining > 290);
    EXPECT(after.secondsRemaining <= 300);

    TEST("previous PIN still verifies inside its window");
    EXPECT(verifyPin(after.previousPin));

    TEST("a consumed PIN resets the pair (previous cleared)");
    PinSnapshot reset = pinSnapshot();
    EXPECT_EQ(reset.previousPin, std::string(""));
}

static void testPinStaleRotationDropsPrevious() {
    verifyPin(pinSnapshot().currentPin); // fresh pair
    PinSnapshot before = pinSnapshot();

    // Three periods past mint: the PIN went stale a full period ago, so the
    // old current must NOT survive as previous.
    backdatePinClockForTest(900);
    PinSnapshot after = pinSnapshot();

    TEST("stale rotation: does not keep a long-expired PIN as previous");
    EXPECT_EQ(after.previousPin, std::string(""));

    TEST("stale rotation: the expired PIN no longer verifies");
    // Gated on the re-mint actually differing (see the collision note above).
    if (after.currentPin != before.currentPin) EXPECT(!verifyPin(before.currentPin));
}

static void testVerifyPinConsumesAndFlashes() {
    PinSnapshot s = pinSnapshot();

    TEST("verifyPin: accepts the current PIN");
    EXPECT(verifyPin(s.currentPin));

    TEST("verifyPin: a consumed PIN cannot be replayed");
    // Gated on the re-mint actually differing (see the collision note above).
    if (pinSnapshot().currentPin != s.currentPin) EXPECT(!verifyPin(s.currentPin));

    TEST("pinSnapshot: flashes PinPaired right after a pair");
    PinSnapshot paired = pinSnapshot();
    EXPECT(paired.state == PinState::PinPaired);

    TEST("pinSnapshot: PinPaired flash decays back to PinActive");
    backdatePinClockForTest(6); // PIN_PAIRED_HOLD_SEC is 5
    PinSnapshot decayed = pinSnapshot();
    EXPECT(decayed.state == PinState::PinActive);
}

static void testPinRejectsWrongGuesses() {
    verifyPin(pinSnapshot().currentPin); // fresh pair
    PinSnapshot s = pinSnapshot();

    TEST("verifyPin: rejects a wrong 4-digit guess");
    EXPECT(!verifyPin(wrongPinFor(s)));

    TEST("verifyPin: rejects wrong-length input");
    EXPECT(!verifyPin(""));
    EXPECT(!verifyPin("123"));
    EXPECT(!verifyPin("99999"));

    TEST("verifyPin: the real PIN still works after scattered failures");
    EXPECT(verifyPin(pinSnapshot().currentPin));
}

static void testPinBurnsAfterMaxFails() {
    verifyPin(pinSnapshot().currentPin); // fresh pair, fail count 0
    PinSnapshot s = pinSnapshot();

    // PIN_MAX_FAILS is 5: the 5th wrong guess burns both PINs.
    for (int i = 0; i < 5; i++) (void)verifyPin("99999");

    TEST("burn: five wrong guesses reset the pair");
    PinSnapshot burned = pinSnapshot();
    EXPECT_EQ(burned.previousPin, std::string(""));
    EXPECT_EQ(burned.currentPin.size(), size_t{4});

    TEST("burn: the pre-burn PIN no longer verifies");
    // Gated on the re-mint actually differing (see the collision note above).
    if (burned.currentPin != s.currentPin) EXPECT(!verifyPin(s.currentPin));
}

int main() {
    std::cout << "Running PIN rotation / shared-crypto-helper tests...\n\n";

    if (!sodiumInit()) {
        std::cerr << "sodium init failed\n";
        return 1;
    }
    TEST("sodiumInit: idempotent");
    EXPECT(sodiumInit());

    testRandomHelpers();
    testHexCodec();
    testKeyExchange();
    testGenerateToken();
    testPinSnapshotInitial();
    testPinRotationKeepsPrevious();
    testPinStaleRotationDropsPrevious();
    testVerifyPinConsumesAndFlashes();
    testPinRejectsWrongGuesses();
    testPinBurnsAfterMaxFails();

    std::cout << "\n=== Test Results ===\n";
    std::cout << "  Passed: " << g_pass << "\n";
    std::cout << "  Failed: " << g_fail << "\n";
    std::cout << "  STATUS: " << (g_fail == 0 ? "ALL PASSED" : "FAILED") << "\n";
    return g_fail == 0 ? 0 : 1;
}
