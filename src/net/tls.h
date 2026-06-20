// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include <string>

// Ensure a self-signed server cert + key exist (generating on first call,
// reusing after), setting `certPath`/`keyPath` to the PEM locations beside the
// config file. The cert is intentionally unauthenticated (senders neither pin
// nor CA-verify it), so it provides transport encryption, not server identity.
bool ensureServerCert(std::string& certPath, std::string& keyPath);
