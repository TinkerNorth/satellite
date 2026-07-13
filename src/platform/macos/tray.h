// SPDX-License-Identifier: LGPL-3.0-or-later
// Kept header-compatible with the Win32 tray module.
#pragma once
#include "globals.h"

#include <string>

void addTrayIcon();
void removeTrayIcon();
void notifyPairRequestMac(const std::string& deviceId);
