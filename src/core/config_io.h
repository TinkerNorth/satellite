// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include "core/types.h"

#include <string>

Config loadConfig();
void saveConfig(const Config& cfg);

std::string configPath();
bool atomicWriteFile(const std::string& path, const std::string& bytes);
