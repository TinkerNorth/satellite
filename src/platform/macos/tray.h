// SPDX-License-Identifier: LGPL-3.0-or-later
// Menu-bar status item; kept header-compatible with the Win32 tray module.
// The status item attaches to the shared NSStatusBar.
#pragma once
#include "globals.h"

#include <string>

void addTrayIcon();
void removeTrayIcon();
// pairing.cpp listener (registered from main.mm): native notification for a
// reverse-pairing request; activating it shows an Accept/Reject alert.
void notifyPairRequestMac(const std::string& deviceId);
