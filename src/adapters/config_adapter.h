/*
 * adapters/config_adapter.h — IConfigPort implementation.
 *
 * Delegates to existing config.cpp functions.
 */
#pragma once

#include "../core/ports.h"

class ConfigAdapter : public IConfigPort {
  public:
    Config loadConfig() override;
    void saveConfig(const Config& cfg) override;
    void setAutoStart(bool enable) override;
    bool getAutoStart() override;
};
