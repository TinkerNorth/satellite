// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

// Pulls in <winsock2.h> before <windows.h> — keep first to preserve the
// required winsock2-before-windows.h include order.
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

inline const UINT WM_TRAYICON = WM_APP + 1;
inline const UINT IDM_OPEN_UI = 1001;
inline const UINT IDM_EXIT = 1003;
inline const UINT IDM_CHECK_UPDATES = 1004;  // tray "Check for Updates…"
inline const UINT IDM_INSTALL_UPDATE = 1005; // tray "Install Update vX.Y.Z" (dynamic)
inline const UINT IDM_OPEN_LOGS = 1006;      // tray "Open Logs Folder"
inline const UINT IDM_REPORT_PROBLEM = 1007; // tray "Report a Problem..."
// Dynamic per-request "review pairing" items occupy [IDM_PAIR_REVIEW_BASE,
// IDM_PAIR_REVIEW_BASE + N); their order maps to the deviceId vector the tray
// menu snapshots from pendingPairRequests().
inline const UINT IDM_PAIR_REVIEW_BASE = 1100;

extern HWND g_hwnd;
