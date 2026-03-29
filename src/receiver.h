/*
 * receiver.h — UDP receiver thread
 */
#pragma once
#include "globals.h"

class SessionService;
class ClientAdapter;

void receiverThread(SessionService& svc, ClientAdapter& client);
