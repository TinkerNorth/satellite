// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * net/machine_id.h — stable per-install identifier advertised in discovery.
 */
#pragma once

#include <string>

// This satellite's stable identity as 32 lowercase hex chars (16 random
// bytes), generated and persisted on first call, cached thereafter.
//
// Why this exists: a dish keys a remembered satellite by identity, and the
// only thing that distinguished one satellite from another used to be its
// IP:port — which changes on every DHCP lease renewal, so the same physical
// receiver kept reappearing as a brand-new entry (two rows, one dead). An id
// that outlives the address lets the dish collapse those into one connection.
// Persisted beside the config file (mirrors tls.cpp's self-signed cert) so the
// identity survives restarts. Thread-safe.
std::string ensureMachineId();
