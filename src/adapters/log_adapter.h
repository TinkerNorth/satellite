/*
 * adapters/log_adapter.h — ILogPort implementation.
 *
 * Wraps the existing log ring buffer in globals.cpp.
 */
#pragma once

#include "core/ports.h"

class LogAdapter : public ILogPort {
  public:
    void logMsg(LogLevel level, const std::string& source, const std::string& message) override;
};
