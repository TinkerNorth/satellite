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

void addTrayIcon();
void removeTrayIcon();
