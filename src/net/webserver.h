// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * webserver.h — HTTP server thread with all API routes
 */
#pragma once
#include "core/app_state.h"
#include "net/net_compat.h"

class SessionService;

void httpThread(SessionService& svc);
