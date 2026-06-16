// SPDX-License-Identifier: LGPL-3.0-or-later
#include "pairing_keys.h"

#include "crypto.h"

#include <sodium.h>

PairingKeyOutcome resolvePairingSharedKey(const std::string& clientPkHex,
                                          const uint8_t serverPk[32], const uint8_t serverSk[32],
                                          std::string& outSharedKeyHex) {
    outSharedKeyHex.clear();

    if (!clientPkHex.empty()) {
        uint8_t clientPk[32];
        uint8_t sharedKey[32];
        if (!hexDecode(clientPkHex, clientPk, 32) ||
            !computeSharedKey(sharedKey, clientPk, serverSk, serverPk)) {
            return PairingKeyOutcome::InvalidClientKey;
        }
        outSharedKeyHex = hexEncode(sharedKey, 32);
        sodium_memzero(sharedKey, 32);
        return PairingKeyOutcome::Derived;
    }

    uint8_t randomKey[32];
    randombytes_buf(randomKey, 32);
    outSharedKeyHex = hexEncode(randomKey, 32);
    sodium_memzero(randomKey, 32);
    return PairingKeyOutcome::Random;
}
