// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <string>

namespace lifecycle {

std::string logDir();

void startFileLogger(const std::string& dir);

void stopFileLogger();

inline constexpr unsigned kLogFlushTimeoutMs = 2000;

bool flushFileLog(unsigned timeoutMs);

} // namespace lifecycle
