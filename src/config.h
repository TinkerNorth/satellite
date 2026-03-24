/*
 * config.h — Configuration persistence, JSON helpers, auto-start
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
void   saveConfig(const Config& cfg);

// ── Auto-start (Windows registry) ───────────────────────────────────────────
void setAutoStart(bool enable);
bool getAutoStart();

// ── Utility ─────────────────────────────────────────────────────────────────
std::string getExeDir();
std::string getCurrentDate();

