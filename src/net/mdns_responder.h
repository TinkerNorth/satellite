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
 * RFC 6762 behaviours implemented:
 *   - §8.1  probing — before announcing, the instance name is claimed via
 *           the full probe sequence: a random 0-250 ms startup delay, then
 *           three ANY probe queries 250 ms apart, each carrying the proposed
 *           unique records (SRV/TXT/A) in the authority section. A
 *           conflicting response triggers a §9 rename and a fresh probe
 *           sequence; conflict rate-limiting (15 conflicts / 10 s → 5 s
 *           backoff) is applied.
 *   - §8.2  simultaneous-probe tiebreaking — a peer probe for a name we are
 *           also probing is resolved by the §8.2.1 lexicographic record-set
 *           comparison rather than treated as an outright conflict; on a
 *           loss we defer one second and re-probe.
 *   - §9    conflict resolution — a name clash disambiguates the instance
 *           label ("<host>", "<host> (2)", "<host> (3)", …), capped at ten
 *           rename attempts.
 *   - §8.3  startup announcement — the full unsolicited answer set is
 *           multicast three times ~1 s apart so senders already running
 *           discover us without waiting to re-query.
 *   - §7.1  Known-Answer suppression — records a querier already holds with
 *           a fresh-enough TTL are dropped from the reply; if every record
 *           is known, no response is sent at all.
 *   - §10.1 goodbye — a TTL-0 record set is multicast on shutdown.
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
