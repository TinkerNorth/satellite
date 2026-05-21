// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * webserver.h — the two HTTP server threads.
 *
 * adminHttpThread — web UI + admin API. Plain HTTP, bound to 127.0.0.1, no
 *                   authentication (localhost is the trust boundary).
 * clientApiThread — sender-facing API: pairing and connection management.
 *                   HTTPS (self-signed cert) bound to 0.0.0.0; paired-device
 *                   authorization on the connection routes.
 */
#pragma once
#include "core/app_state.h"
#include "net/net_compat.h"

class SessionService;

void adminHttpThread(SessionService& svc);
void clientApiThread(SessionService& svc);
