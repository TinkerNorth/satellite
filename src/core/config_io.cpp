// SPDX-License-Identifier: LGPL-3.0-or-later
#include "core/config_io.h"

#include "core/config_json.h"

#include <fstream>
#include <iterator>
#include <string>

Config loadConfig() {
    Config cfg;
    std::ifstream f(configPath());
    if (!f.is_open()) return cfg;
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    satellite::parseConfigInto(content, cfg);
    return cfg;
}

void saveConfig(const Config& cfg) {
    atomicWriteFile(configPath(), satellite::serializeConfig(cfg));
}
