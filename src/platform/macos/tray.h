// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * tray.h — Menu-bar status item (macOS)
 *
 * Kept header-compatible with the Win32 tray module (addTrayIcon/removeTrayIcon).
 * On macOS the HWND parameter is ignored; the status item is attached to the
 * shared NSStatusBar.
 */
#pragma once
#include "globals.h"

#include <string>

void addTrayIcon();
void removeTrayIcon();
// pairing.cpp listener: deliver a native notification for a reverse-pairing
// request; activating it shows an Accept/Reject alert. Registered from main.mm.
void notifyPairRequestMac(const std::string& deviceId);
