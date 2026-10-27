// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include "globals.h"

#include <cstddef>
#include <string>

namespace lifecycle {

void startFileLogger(const std::string& dir);

void stopFileLogger();

inline constexpr DWORD kLogFlushTimeoutMs = 2000;

bool flushFileLog(DWORD timeoutMs);

bool writeLogLineDirect(const char* line, size_t length);

} // namespace lifecycle
