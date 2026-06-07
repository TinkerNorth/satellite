// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "globals.h"

std::string jsonEscape(const std::string& s);
std::string jsonGetString(const std::string& json, const std::string& key);

std::string configPath();

Config loadConfig();
void saveConfig(const Config& cfg);

void setAutoStart(bool enable);
bool getAutoStart();

std::string getExeDir();
std::string getCurrentDate();
