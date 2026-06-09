// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <string>

// A machine id is exactly 32 lowercase hex chars. Pulled out of the persisted-id
// load path so the "regenerate on corruption rather than advertise junk" rule is
// unit-testable without touching the filesystem.
inline bool isValidMachineId(const std::string& id) {
    return id.size() == 32 && id.find_first_not_of("0123456789abcdef") == std::string::npos;
}

// Stable per-install identity as 32 lowercase hex chars (16 random bytes),
// generated + persisted (beside the config file) on first call, cached after.
// Thread-safe. Exists so the dish can dedupe a remembered satellite by an id
// that outlives DHCP lease changes, instead of keying on the (mutable) IP.
std::string ensureMachineId();
