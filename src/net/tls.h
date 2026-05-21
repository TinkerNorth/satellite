// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * tls.h — self-signed TLS certificate for the client API HTTPS server.
 */
#pragma once
#include <string>

// Ensure a self-signed server certificate + private key exist on disk,
// generating them on the first call and reusing them afterwards. On success
// `certPath` / `keyPath` are set to the PEM file locations (alongside the
// config file) and the function returns true.
//
// The certificate is intentionally unauthenticated — senders neither pin it
// nor CA-verify it — so it provides transport encryption, not server
// authentication. See clientApiThread() in webserver.cpp.
bool ensureServerCert(std::string& certPath, std::string& keyPath);
