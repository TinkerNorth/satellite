// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * globals.h — Windows-specific shared state and constants.
 *
 * Portable app state (Config, atomics, log ring, httplib, g_pairSock) now
 * lives in core/app_state.h; this header just layers the Win32-only
 * extras on top and pulls in the Windows system headers that all
 * platform/windows translation units transitively rely on.
 */
#pragma once

// Portable shared state (pulls in net_compat.h, which in turn pulls in
// <winsock2.h> before <windows.h> — keep this first to preserve the
// required winsock2-before-windows.h include order).
#include "core/app_state.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <fstream>
#include <vector>
#include <map>
#include <unordered_map>
#include <random>
#include <chrono>
#include <sstream>
#include <algorithm>
#include <array>
#include <functional>

#include <shellapi.h>
#include <setupapi.h>
#include <shlobj.h>
#include <wincrypt.h>
#include <bcrypt.h>

#include "ViGEm/BusShared.h"

// ── Win32-only constants (not part of domain) ───────────────────────────────
inline const UINT WM_TRAYICON = WM_APP + 1;
inline const UINT IDM_OPEN_UI = 1001;
inline const UINT IDM_TOGGLE = 1002;
inline const UINT IDM_EXIT = 1003;
inline const UINT IDM_CHECK_UPDATES = 1004;  // tray "Check for Updates…"
inline const UINT IDM_INSTALL_UPDATE = 1005; // tray "Install Update vX.Y.Z" (dynamic)

// ── Win32-only shared state ─────────────────────────────────────────────────
extern HWND g_hwnd;
