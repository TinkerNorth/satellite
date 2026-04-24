/*
 * receiver.h — UDP receiver thread
 */
#pragma once
#include "core/app_state.h"
#include "net/net_compat.h"

class SessionService;
class ClientAdapter;

void receiverThread(SessionService& svc, ClientAdapter& client);
