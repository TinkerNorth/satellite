// SPDX-License-Identifier: LGPL-3.0-or-later
// Linux-only shared state on top of core/app_state.h, plus the system headers
// the rest of platform/linux/ relies on.
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
