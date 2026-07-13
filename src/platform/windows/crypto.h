// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "globals.h"

std::string base64Encode(const std::vector<BYTE>& data);
std::vector<BYTE> base64Decode(const std::string& s);

std::string sha256hex(const std::string& input);
std::string dpapiEncrypt(const std::string& plaintext);
std::string dpapiDecrypt(const std::string& encoded);

std::string randomHex(int bytes);
std::string randomDigits(int n);

bool verifyPin(const std::string& pin);

// previousPin is empty until the first rotation (and after a pair/burn reset).
struct PinSnapshot {
    PinState state = PinState::PinActive;
    std::string currentPin;
    std::string previousPin;
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
// Packet AEAD lives in net/session_crypto.h (shared across platforms).
