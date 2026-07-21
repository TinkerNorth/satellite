// SPDX-License-Identifier: LGPL-3.0-or-later

// Windows-specific keystore surface only: DPAPI-backed dpapi* and BCrypt
// sha256hex (+ the base64 codec they share). The platform-independent
// PIN-rotation/hex/CSPRNG/kx helpers were hoisted to net/pin_rotation.cpp
// (D11), which is where libsodium is used; this TU no longer needs it.
#include "crypto.h"

static const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const std::vector<BYTE>& data) {
    std::string out;
    int val = 0, bits = -6;
    for (BYTE c : data) {
        val = (val << 8) + c;
        bits += 8;
        while (bits >= 0) {
            out.push_back(b64chars[(val >> bits) & 0x3F]);
            bits -= 6;
        }
    }
    if (bits > -6) out.push_back(b64chars[((val << 8) >> (bits + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

std::vector<BYTE> base64Decode(const std::string& s) {
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; i++) T[(int)b64chars[i]] = i;
    std::vector<BYTE> out;
    int val = 0, bits = -8;
    for (char c : s) {
        if (T[(unsigned char)c] == -1) break;
        val = (val << 6) + T[(unsigned char)c];
        bits += 6;
        if (bits >= 0) {
            out.push_back((BYTE)(val >> bits));
            bits -= 8;
        }
    }
    return out;
}

std::string sha256hex(const std::string& input) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    BYTE hash[32];
    BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0);
    BCryptHashData(hHash, (PUCHAR)input.data(), (ULONG)input.size(), 0);
    BCryptFinishHash(hHash, hash, 32, 0);
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    char hex[65];
    for (int i = 0; i < 32; i++)
        (void)snprintf(hex + static_cast<ptrdiff_t>(i) * 2, 3, "%02x", hash[i]);
    hex[64] = 0;
    return std::string(hex);
}

std::string dpapiEncrypt(const std::string& plaintext) {
    DATA_BLOB in, out;
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.data()));
    in.cbData = (DWORD)plaintext.size();
    if (CryptProtectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out) == FALSE) return "";
    std::vector<BYTE> blob(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    return base64Encode(blob);
}

std::string dpapiDecrypt(const std::string& encoded) {
    auto blob = base64Decode(encoded);
    DATA_BLOB in, out;
    in.pbData = blob.data();
    in.cbData = (DWORD)blob.size();
    if (CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out) == FALSE) return "";
    std::string result(reinterpret_cast<char*>(out.pbData), out.cbData);
    LocalFree(out.pbData);
    return result;
}
