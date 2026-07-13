// SPDX-License-Identifier: LGPL-3.0-or-later

// Multicast-DNS responder thread: joins 224.0.0.251:5353 and answers PTR/ANY
// queries for `_satellite._udp.local.` with PTR + SRV + TXT (+ A), so native
// Bonjour/Avahi senders discover us. Additive: runs alongside the legacy
// beacon, never a replacement.
//
// RFC 6762 behaviours implemented: §8.1 probing, §8.2 simultaneous-probe
// tiebreak, §9 name-conflict rename, §8.3 startup announcement, §7.1
// Known-Answer suppression, §10.1 goodbye on shutdown. The wire encoders live
// in mdns_protocol.h; this is just the socket + recv loop.
#pragma once

#include "app/app_state.h"
#include "net/mdns_protocol.h"
#include "net/net_compat.h"

// Runs until `g_appRunning` clears. A bind failure is non-fatal: the thread
// logs and exits; the legacy broadcast beacon still advertises us.
void mdnsResponderThread();

void requestMdnsRejoin();
