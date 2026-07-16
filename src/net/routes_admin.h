// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "app/app_state.h"
#include "net/net_compat.h"

class SessionService;

// Registers the loopback admin surface (web UI mount, SPA fallbacks, /api/*
// admin routes, SSE) on `server`. Extracted from adminHttpThread (D10);
// webserver.cpp keeps server lifecycle (port, listen) only. Tests register
// the same routes on a loopback instance.
void registerAdminRoutes(httplib::Server& server, SessionService& svc);
