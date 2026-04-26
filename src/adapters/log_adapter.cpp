// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * adapters/log_adapter.cpp — ILogPort implementation.
 *
 * Delegates to the global logMsg function in globals.cpp.
 */
#include "log_adapter.h"

// Forward declaration of the existing global logMsg function
extern void logMsg(LogLevel level, const std::string& source, const std::string& message);

void LogAdapter::logMsg(LogLevel level, const std::string& source, const std::string& message) {
    // Delegate to the existing global function
    ::logMsg(level, source, message);
}
