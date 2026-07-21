// SPDX-License-Identifier: LGPL-3.0-or-later
// Windows keystore surface: DPAPI-backed dpapi* plus BCrypt sha256hex. The
// platform-independent PIN-rotation/hex/CSPRNG/kx helpers live in
// net/pin_rotation.h (D11).
#pragma once
#include "globals.h"

#include "net/pin_rotation.h"

std::string base64Encode(const std::vector<BYTE>& data);
std::vector<BYTE> base64Decode(const std::string& s);

std::string sha256hex(const std::string& input);
std::string dpapiEncrypt(const std::string& plaintext);
std::string dpapiDecrypt(const std::string& encoded);
// Packet AEAD lives in net/session_crypto.h (shared across platforms).
