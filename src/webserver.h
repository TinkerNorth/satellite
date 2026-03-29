/*
 * webserver.h — HTTP server thread with all API routes
 */
#pragma once
#include "globals.h"

class SessionService;

void httpThread(SessionService& svc);
