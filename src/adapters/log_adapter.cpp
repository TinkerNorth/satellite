// SPDX-License-Identifier: LGPL-3.0-or-later

#include "log_adapter.h"

// Defined in globals.cpp.
extern void logMsg(LogLevel level, const std::string& source, const std::string& message);

void LogAdapter::logMsg(LogLevel level, const std::string& source, const std::string& message) {
    ::logMsg(level, source, message);
}
