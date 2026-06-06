// SPDX-License-Identifier: LGPL-3.0-or-later

#pragma once

#include "core/ports.h"

class ConfigAdapter : public IConfigPort {
  public:
    Config loadConfig() override;
    void saveConfig(const Config& cfg) override;
    void setAutoStart(bool enable) override;
    bool getAutoStart() override;
};
