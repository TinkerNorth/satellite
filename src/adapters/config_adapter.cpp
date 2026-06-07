// SPDX-License-Identifier: LGPL-3.0-or-later

// Wraps config.cpp's free functions so SessionService can persist config
// through a mockable interface.
#include "config_adapter.h"

extern Config loadConfig();
extern void saveConfig(const Config& cfg);
extern void setAutoStart(bool enable);
extern bool getAutoStart();

Config ConfigAdapter::loadConfig() { return ::loadConfig(); }

void ConfigAdapter::saveConfig(const Config& cfg) { ::saveConfig(cfg); }

void ConfigAdapter::setAutoStart(bool enable) { ::setAutoStart(enable); }

bool ConfigAdapter::getAutoStart() { return ::getAutoStart(); }
