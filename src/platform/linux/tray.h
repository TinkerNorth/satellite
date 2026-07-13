// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "globals.h"

#include <string>

// True if the tray icon was created and the GTK loop is usable; false on
// headless/non-tray builds, where the caller runs a sigwait loop instead.
bool addTrayIcon();
void removeTrayIcon();
void notifyPairRequestLinux(const std::string& deviceId);
