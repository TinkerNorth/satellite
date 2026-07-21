// SPDX-License-Identifier: LGPL-3.0-or-later
// Mirrors the Windows crypto.h API surface so src/net/ compiles unchanged;
// macOS backs DPAPI+BCrypt with libsodium + a keyfile in app-support (no
// Keychain yet). The platform-independent PIN-rotation/hex/CSPRNG/kx helpers
// live in net/pin_rotation.h (D11); only the keystore-backed pieces below are
// platform surface (implemented in platform/posix/crypto_posix.cpp, shared
// with Linux).
#pragma once
#include "globals.h"

#include "net/pin_rotation.h"

#include <cstdint>

std::string sha256hex(const std::string& input);

// dpapi* names kept for cross-platform parity; macOS uses libsodium secretbox.
std::string dpapiEncrypt(const std::string& plaintext);
std::string dpapiDecrypt(const std::string& encoded);
// Packet AEAD lives in net/session_crypto.h (shared across platforms).
