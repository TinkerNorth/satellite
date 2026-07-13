// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "globals.h"

std::string configPath();

Config loadConfig();
void saveConfig(const Config& cfg);

bool atomicWriteFile(const std::string& path, const std::string& bytes);

void setAutoStart(bool enable);
bool getAutoStart();

std::string getExeDir();
std::string getCurrentDate();
