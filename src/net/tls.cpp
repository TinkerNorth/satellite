// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * tls.cpp — self-signed TLS certificate generation (OpenSSL).
 *
 * The client API server (webserver.cpp) is HTTPS-only. It needs a cert+key;
 * with no CA available on a LAN we generate a self-signed pair on first run
 * and persist it next to the config file so the identity is stable across
 * restarts. Requires OpenSSL 3.0+ (EVP_RSA_gen).
 */
#include "tls.h"
#include "config.h" // configPath()

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <cstdio>
#include <fstream>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace {

// The cert/key live in the same directory as the config file.
std::string certDir() {
    std::string p = configPath();
    auto pos = p.find_last_of("/\\");
    return (pos != std::string::npos) ? p.substr(0, pos) : ".";
}

bool fileExists(const std::string& path) {
    std::ifstream f(path);
    return f.good();
}

// Generate a 2048-bit RSA self-signed certificate valid for 10 years and
// write the cert + private key as PEM files.
bool generateSelfSigned(const std::string& certPath, const std::string& keyPath) {
    EVP_PKEY* pkey = EVP_RSA_gen(2048);
    if (pkey == nullptr) return false;

    X509* x509 = X509_new();
    if (x509 == nullptr) {
        EVP_PKEY_free(pkey);
        return false;
    }

    bool ok = false;
    do {
        ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
        X509_set_version(x509, 2); // X.509 v3
        X509_gmtime_adj(X509_getm_notBefore(x509), 0);
        X509_gmtime_adj(X509_getm_notAfter(x509), 60L * 60 * 24 * 3650);
        if (X509_set_pubkey(x509, pkey) != 1) break;

        X509_NAME* name = X509_get_subject_name(x509);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>("satellite"), -1, -1, 0);
        if (X509_set_issuer_name(x509, name) != 1) break; // self-signed: issuer == subject
        if (X509_sign(x509, pkey, EVP_sha256()) == 0) break;

        FILE* cf = std::fopen(certPath.c_str(), "wb");
        if (cf == nullptr) break;
        const int cwrote = PEM_write_X509(cf, x509);
        std::fclose(cf);
        if (cwrote != 1) break;

        FILE* kf = std::fopen(keyPath.c_str(), "wb");
        if (kf == nullptr) break;
        const int kwrote = PEM_write_PrivateKey(kf, pkey, nullptr, nullptr, 0, nullptr, nullptr);
        std::fclose(kf);
        if (kwrote != 1) break;

#ifndef _WIN32
        ::chmod(keyPath.c_str(), 0600); // private key is owner-readable only
#endif
        ok = true;
    } while (false);

    X509_free(x509);
    EVP_PKEY_free(pkey);
    return ok;
}

} // namespace

bool ensureServerCert(std::string& certPath, std::string& keyPath) {
    const std::string dir = certDir();
    certPath = dir + "/server-cert.pem";
    keyPath = dir + "/server-key.pem";

    if (fileExists(certPath) && fileExists(keyPath)) return true;
    return generateSelfSigned(certPath, keyPath);
}
