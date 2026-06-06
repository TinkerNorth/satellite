// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "globals.h"

#include <string>

// True if the tray icon was created and the GTK loop is usable; false on
// headless boxes or non-tray builds, where the caller runs a sigwait loop.
bool addTrayIcon();
void removeTrayIcon();
// Pairing listener: raises a libnotify Accept/Reject prompt for a reverse-pairing
// request. No-op on headless / non-libnotify builds. Registered from main.cpp.
void notifyPairRequestLinux(const std::string& deviceId);
