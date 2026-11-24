// SPDX-License-Identifier: LGPL-3.0-or-later

// Windows-specific keystore surface only: DPAPI-backed dpapi* and BCrypt
// sha256hex; the base64 codec is core/base64.h, shared with POSIX. The
// platform-independent PIN-rotation/hex/CSPRNG/kx helpers were hoisted to
// net/pin_rotation.cpp (D11), which is where libsodium is used; this TU no
// longer needs it.
#include "crypto.h"
#include "core/base64.h"

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
    return hexEncode(hash, sizeof(hash));
}

std::string dpapiEncrypt(const std::string& plaintext) {
    DATA_BLOB in, out;
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.data()));
    in.cbData = (DWORD)plaintext.size();
    if (CryptProtectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out) == FALSE) return "";
    std::vector<BYTE> blob(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    return satellite::base64Encode(blob);
}

std::string dpapiDecrypt(const std::string& encoded) {
    auto blob = satellite::base64Decode(encoded);
    DATA_BLOB in, out;
    in.pbData = blob.data();
    in.cbData = (DWORD)blob.size();
    if (CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out) == FALSE) return "";
    std::string result(reinterpret_cast<char*>(out.pbData), out.cbData);
    LocalFree(out.pbData);
    return result;
}
