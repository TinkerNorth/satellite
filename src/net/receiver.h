// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * receiver.h — UDP receiver thread
 */
#pragma once
#include "core/app_state.h"
#include "net/net_compat.h"

class SessionService;
class ClientAdapter;

void receiverThread(SessionService& svc, ClientAdapter& client);
