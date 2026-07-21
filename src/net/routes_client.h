// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "app/app_state.h"
#include "net/net_compat.h"

class SessionService;

// Registers the sender-facing client API (pairing + sessions + catalog) on
// `server`. Extracted from clientApiThread (D10); webserver.cpp keeps server
// lifecycle (TLS cert, construction, listen) only. In production `server` is
// the httplib::SSLServer; tests register the same routes on a plain loopback
// instance (route behavior is transport-independent).
void registerClientRoutes(httplib::Server& server, SessionService& svc);
