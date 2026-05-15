// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * net/mdns_responder.h — Multicast-DNS responder thread (Tier 1 Task 1.6).
 *
 * Joins the 224.0.0.251:5353 multicast group and answers PTR / ANY queries
 * for `_satellite._udp.local.` with PTR + SRV + TXT (+ A) records, so that
 * senders using a native Bonjour / Avahi stack — and clients on subnets that
 * drop the legacy UDP broadcast beacon — can discover this receiver.
 *
 * Runs for the process lifetime alongside `discoveryThread()`. The legacy
 * broadcast beacon stays on as a fallback for one release; this responder is
 * additive, never a replacement-in-place.
 *
 * The wire encoders/parsers live in mdns_protocol.h (pure, unit-tested); this
 * module is just the multicast socket + recv loop, mirroring discovery.cpp.
 */
#pragma once

#include "core/app_state.h"
#include "net/mdns_protocol.h"
#include "net/net_compat.h"

// Background responder thread. Binds the mDNS multicast socket, joins the
// group, and answers matching queries until `g_appRunning` clears. If the
// socket cannot be bound (e.g. another responder holds 5353 without
// SO_REUSEPORT), the thread logs and exits cleanly — discovery still works
// via the legacy broadcast beacon, so a bind failure is non-fatal.
//
// The query-match decision is `mdns::questionMatchesService` (pure, in
// mdns_protocol.h, unit-tested there).
void mdnsResponderThread();
