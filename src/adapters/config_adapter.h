// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * adapters/config_adapter.h — IConfigPort implementation.
 *
 * Delegates to existing config.cpp functions.
 */
#pragma once

#include "core/ports.h"

class ConfigAdapter : public IConfigPort {
  public:
    Config loadConfig() override;
    void saveConfig(const Config& cfg) override;
    void setAutoStart(bool enable) override;
    bool getAutoStart() override;
};
