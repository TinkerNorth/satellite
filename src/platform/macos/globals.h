// SPDX-License-Identifier: LGPL-3.0-or-later
// Layers macOS-only extras and system-header includes on top of the portable
// app state (Config, atomics, log ring, httplib, g_pairSock) in app/app_state.h.
#pragma once

// Portable shared state (pulls in net_compat.h).
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
