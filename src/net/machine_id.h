// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <string>

// Stable per-install identity as 32 lowercase hex chars (16 random bytes),
// generated + persisted (beside the config file) on first call, cached after.
// Thread-safe. Exists so the dish can dedupe a remembered satellite by an id
// that outlives DHCP lease changes, instead of keying on the (mutable) IP.
std::string ensureMachineId();
