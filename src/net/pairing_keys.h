// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>

enum class PairingKeyOutcome {
    Derived,
    Random,
    InvalidClientKey,
};

PairingKeyOutcome resolvePairingSharedKey(const std::string& clientPkHex,
                                          const uint8_t serverPk[32], const uint8_t serverSk[32],
                                          std::string& outSharedKeyHex);
