// SPDX-License-Identifier: LGPL-3.0-or-later

// POSIX keystore shared by Linux and macOS, consolidated from the previously
// byte-identical halves of platform/{linux,macos}/crypto.cpp (D11): the
// dpapi* names (kept for Windows parity) are backed by libsodium secretbox
// with a 0600 keyfile next to the config. An OS keystore (libsecret/Keyring
// on Linux, Keychain on macOS) would be stronger but pulls in a hard desktop
// dependency / much more surface. The platform-independent PIN/hex/CSPRNG/kx
// helpers live in net/pin_rotation.cpp.
#include "crypto.h"
#include "config.h"

#include <sodium.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64Encode(const std::vector<uint8_t>& data) {
    std::string out;
    int val = 0, bits = -6;
    for (uint8_t c : data) {
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

static std::vector<uint8_t> base64Decode(const std::string& s) {
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; i++) T[(int)b64chars[i]] = i;
    std::vector<uint8_t> out;
    int val = 0, bits = -8;
    for (char c : s) {
        if (T[(unsigned char)c] == -1) break;
        val = (val << 6) + T[(unsigned char)c];
        bits += 6;
        if (bits >= 0) {
            out.push_back((uint8_t)(val >> bits));
            bits -= 8;
        }
    }
    return out;
}

std::string sha256hex(const std::string& input) {
    uint8_t hash[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(hash, reinterpret_cast<const uint8_t*>(input.data()), input.size());
    char hex[65];
    for (int i = 0; i < 32; i++) (void)snprintf(hex + i * 2, 3, "%02x", hash[i]);
    hex[64] = 0;
    return std::string(hex);
}

// Local 0600 keyfile next to the config replaces DPAPI's user-scoped key.
static constexpr size_t LOCAL_KEY_LEN = crypto_secretbox_KEYBYTES;

static std::string localKeyPath() {
    std::string p = configPath();
    auto pos = p.find_last_of('/');
    std::string dir = (pos != std::string::npos) ? p.substr(0, pos) : ".";
    return dir + "/keyfile";
}

static bool readLocalKey(uint8_t out[LOCAL_KEY_LEN]) {
    int fd = ::open(localKeyPath().c_str(), O_RDONLY);
    if (fd < 0) return false;
    ssize_t n = ::read(fd, out, LOCAL_KEY_LEN);
    ::close(fd);
    return n == (ssize_t)LOCAL_KEY_LEN;
}

static bool writeLocalKey(const uint8_t key[LOCAL_KEY_LEN]) {
    std::string path = localKeyPath();
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    ssize_t n = ::write(fd, key, LOCAL_KEY_LEN);
    ::close(fd);
    (void)::chmod(path.c_str(), 0600);
    return n == (ssize_t)LOCAL_KEY_LEN;
}

static bool getOrCreateLocalKey(uint8_t out[LOCAL_KEY_LEN]) {
    if (readLocalKey(out)) return true;
    randombytes_buf(out, LOCAL_KEY_LEN);
    return writeLocalKey(out);
}

// libsodium secretbox, base64-encoded (nonce || ciphertext).
std::string dpapiEncrypt(const std::string& plaintext) {
    uint8_t key[LOCAL_KEY_LEN];
    if (!getOrCreateLocalKey(key)) return "";

    uint8_t nonce[crypto_secretbox_NONCEBYTES];
    randombytes_buf(nonce, sizeof(nonce));

    std::vector<uint8_t> ct(crypto_secretbox_MACBYTES + plaintext.size());
    if (crypto_secretbox_easy(ct.data(), reinterpret_cast<const uint8_t*>(plaintext.data()),
                              plaintext.size(), nonce, key) != 0) {
        sodium_memzero(key, sizeof(key));
        return "";
    }
    sodium_memzero(key, sizeof(key));

    std::vector<uint8_t> blob;
    blob.reserve(sizeof(nonce) + ct.size());
    blob.insert(blob.end(), nonce, nonce + sizeof(nonce));
    blob.insert(blob.end(), ct.begin(), ct.end());
    return base64Encode(blob);
}

std::string dpapiDecrypt(const std::string& encoded) {
    auto blob = base64Decode(encoded);
    if (blob.size() < crypto_secretbox_NONCEBYTES + crypto_secretbox_MACBYTES) return "";

    uint8_t key[LOCAL_KEY_LEN];
    if (!readLocalKey(key)) return "";

    const uint8_t* nonce = blob.data();
    const uint8_t* ct = blob.data() + crypto_secretbox_NONCEBYTES;
    size_t ctLen = blob.size() - crypto_secretbox_NONCEBYTES;

    std::vector<uint8_t> pt(ctLen - crypto_secretbox_MACBYTES);
    if (crypto_secretbox_open_easy(pt.data(), ct, ctLen, nonce, key) != 0) {
        sodium_memzero(key, sizeof(key));
        return "";
    }
    sodium_memzero(key, sizeof(key));
    return std::string(reinterpret_cast<char*>(pt.data()), pt.size());
}
