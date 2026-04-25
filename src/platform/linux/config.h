// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * config.h — Configuration persistence, JSON helpers, auto-start (Linux)
 */
#pragma once
#include "globals.h"

// ── JSON helpers ────────────────────────────────────────────────────────────
std::string jsonEscape(const std::string& s);
std::string jsonGetString(const std::string& json, const std::string& key);

// ── Config path ─────────────────────────────────────────────────────────────
std::string configPath();

// ── Config persistence ──────────────────────────────────────────────────────
Config loadConfig();
void saveConfig(const Config& cfg);

// ── Auto-start (XDG autostart .desktop) ─────────────────────────────────────
void setAutoStart(bool enable);
bool getAutoStart();

// ── Utility ─────────────────────────────────────────────────────────────────
std::string getExeDir();
std::string getCurrentDate();
