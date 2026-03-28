/*
 * adapters/config_adapter.cpp — IConfigPort implementation.
 *
 * Delegates to the free functions in config.cpp.
 * This adapter exists so that SessionService can persist config
 * through a mockable interface.
 */
#include "config_adapter.h"

// ── Forward declarations of functions in config.cpp ─────────────────────
// (These will remain as free functions; the adapter just wraps them.)
extern Config loadConfig();
extern void saveConfig(const Config& cfg);
extern void setAutoStart(bool enable);
extern bool getAutoStart();

Config ConfigAdapter::loadConfig() {
    return ::loadConfig();
}

void ConfigAdapter::saveConfig(const Config& cfg) {
    ::saveConfig(cfg);
}

void ConfigAdapter::setAutoStart(bool enable) {
    ::setAutoStart(enable);
}

bool ConfigAdapter::getAutoStart() {
    return ::getAutoStart();
}

