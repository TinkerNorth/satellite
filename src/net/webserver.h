/*
 * webserver.h — HTTP server thread with all API routes
 */
#pragma once
#include "core/app_state.h"
#include "net/net_compat.h"

class SessionService;

void httpThread(SessionService& svc);
