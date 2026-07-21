// SPDX-License-Identifier: LGPL-3.0-or-later

// Platform-independent crypto helpers hoisted out of the triplicated
// src/platform/{windows,linux,macos}/crypto.cpp (D11): the pairing-PIN
// rotation state machine, hex codecs, CSPRNG string helpers, and the X25519
// pairing-key exchange. Everything here is libsodium-backed and identical on
// every OS; the per-platform crypto.{h,cpp} keep only genuinely
// platform-specific keystore storage (DPAPI vs POSIX keyfile) and re-export
// this header so `#include "crypto.h"` callers compile unchanged.
#pragma once

#include "core/types.h"

#include <cstdint>
#include <string>

// PINs and identity tokens are security-sensitive, so these draw from
// libsodium's CSPRNG, never std::mt19937 (deterministic, reconstructable).
// randomDigits is rejection-sampled (randombytes_uniform): no modulo bias.
std::string randomHex(int bytes);
std::string randomDigits(int n);

// A 4-digit pairing PIN rotates every 5 minutes; the outgoing PIN stays
// accepted as "previous" for one more period; a successful pair or a burst of
// wrong guesses burns both. Verification is constant-time.
bool verifyPin(const std::string& pin);

// secondsRemaining counts down to the next PIN rotation; previousPin is empty
// until the first rotation (and after a pair/burn reset).
struct PinSnapshot {
    PinState state = PinState::PinActive;
    std::string currentPin;
    std::string previousPin;
    int secondsRemaining = 0;
};
PinSnapshot pinSnapshot();

#ifdef SATELLITE_BUILD_TESTS
// Test seam: shift the PIN rotation/paired-flash clocks backwards (as if
// `seconds` had elapsed) so rotation-due and validity-window paths are
// testable without real sleeps. Mirrors SessionService::backdateForTest.
void backdatePinClockForTest(int seconds);
#endif

std::string hexEncode(const uint8_t* data, size_t len);

// Strict: exact length, hex digits only (no sscanf leniency).
bool hexDecode(const std::string& hex, uint8_t* out, size_t outLen);

bool sodiumInit();
void generateKeyPair(uint8_t pk[32], uint8_t sk[32]);
bool computeSharedKey(uint8_t sharedKey[32], const uint8_t clientPk[32], const uint8_t serverSk[32],
                      const uint8_t serverPk[32]);
uint32_t generateToken();
// Packet AEAD lives in net/session_crypto.h (shared across platforms).
