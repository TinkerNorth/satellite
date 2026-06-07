// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "core/app_state.h"
#include "net/net_compat.h"

class SessionService;
class ClientAdapter;

void receiverThread(SessionService& svc, ClientAdapter& client);
