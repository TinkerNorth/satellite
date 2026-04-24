/*
 * globals.h — macOS-specific shared state and constants.
 *
 * Portable app state (Config, atomics, log ring, httplib, g_pairSock) lives in
 * core/app_state.h; this header layers the macOS-only extras on top and pulls
 * in the system headers that the rest of the files under platform/macos/ rely on.
 */
#pragma once

// Portable shared state (pulls in net_compat.h).
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
