// SPDX-License-Identifier: LGPL-3.0-or-later

#pragma once

#include "core/ports.h"

class LogAdapter : public ILogPort {
  public:
    void logMsg(LogLevel level, const std::string& source, const std::string& message) override;
};
