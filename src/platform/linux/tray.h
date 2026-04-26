// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * tray.h — System-tray status icon (Linux / libayatana-appindicator).
 *
 * Diverges slightly from the Win32/macOS tray API: addTrayIcon() returns a
 * bool so the composition root can fall back to a headless main loop when
 * no graphical session is available (no DISPLAY/WAYLAND_DISPLAY) or when
 * the binary was built without libayatana-appindicator/GTK.
 */
#pragma once
#include "globals.h"

// Returns true if the tray icon was created and the GTK main loop is usable.
// Returns false on truly headless boxes or when SATELLITE_HAS_TRAY was not
// defined at build time; the caller should run a sigwait-based main loop.
bool addTrayIcon();
void removeTrayIcon();
