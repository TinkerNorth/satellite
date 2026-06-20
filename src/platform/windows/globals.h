// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

// Keep first: pulls in <winsock2.h> before <windows.h>, the required order.
#include "app/app_state.h"

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
inline const UINT IDM_CHECK_UPDATES = 1004;
inline const UINT IDM_INSTALL_UPDATE = 1005;
inline const UINT IDM_OPEN_LOGS = 1006;
inline const UINT IDM_REPORT_PROBLEM = 1007;
inline const UINT IDM_DONATE = 1008;
// Dynamic per-request review-pairing items occupy [BASE, BASE + N); their order
// maps to the deviceId vector snapshotted from pendingPairRequests().
inline const UINT IDM_PAIR_REVIEW_BASE = 1100;

extern HWND g_hwnd;
