// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * config.cpp — Configuration persistence, JSON helpers, auto-start
 */
#include "config.h"

// ── JSON string escaping ────────────────────────────────────────────────────
std::string jsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"')
            out += "\\\"";
        else if (c == '\\')
            out += "\\\\";
        else if (c == '\n')
            out += "\\n";
        else
            out += c;
    }
    return out;
}

std::string jsonGetString(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return "";
    auto colon = json.find(':', pos + needle.size());
    if (colon == std::string::npos) return "";
    auto q1 = json.find('"', colon + 1);
    if (q1 == std::string::npos) return "";
    auto q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos) return "";
    return json.substr(q1 + 1, q2 - q1 - 1);
}

// ── Config path ─────────────────────────────────────────────────────────────
std::string configPath() {
    char buf[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, buf))) {
        std::string dir = std::string(buf) + "\\satellite";
        CreateDirectoryA(dir.c_str(), nullptr);
        return dir + "\\config.json";
    }
    return "config.json";
}

// ── Load config ─────────────────────────────────────────────────────────────
Config loadConfig() {
    Config cfg;
    std::ifstream f(configPath());
    if (!f.is_open()) return cfg;
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    auto getInt = [&](const char* key) -> int {
        auto pos = content.find(std::string("\"") + key + "\"");
        if (pos == std::string::npos) return -1;
        auto colon = content.find(':', pos);
        if (colon == std::string::npos) return -1;
        return atoi(content.c_str() + colon + 1);
    };
    auto getBool = [&](const char* key) -> bool {
        auto pos = content.find(std::string("\"") + key + "\"");
        if (pos == std::string::npos) return false;
        auto colon = content.find(':', pos);
        if (colon == std::string::npos) return false;
        auto rest = content.substr(colon + 1, 10);
        return rest.find("true") != std::string::npos;
    };
    // Like getBool but only sets *out if the key is present, so we don't
    // silently overwrite struct defaults for absent keys.
    auto getBoolOpt = [&](const char* key, bool* out) {
        auto pos = content.find(std::string("\"") + key + "\"");
        if (pos == std::string::npos) return;
        *out = getBool(key);
    };
    auto getInt64 = [&](const char* key) -> int64_t {
        auto pos = content.find(std::string("\"") + key + "\"");
        if (pos == std::string::npos) return -1;
        auto colon = content.find(':', pos);
        if (colon == std::string::npos) return -1;
        return strtoll(content.c_str() + colon + 1, nullptr, 10);
    };

    int v = 0;
    v = getInt("udpPort");
    if (v > 0) cfg.udpPort = v;
    v = getInt("webPort");
    if (v > 0) cfg.webPort = v;
    v = getInt("pairPort");
    if (v > 0) cfg.pairPort = v;
    v = getInt("discPort");
    if (v > 0) cfg.discPort = v;
    cfg.autoStart = getBool("autoStart");
    cfg.credentials = jsonGetString(content, "credentials");

    // OTA update preferences. Absent keys keep struct defaults — important
    // on first run after upgrade, so we don't clobber autoCheck=true.
    std::string ch = jsonGetString(content, "updateChannel");
    if (!ch.empty()) cfg.updateChannel = ch;
    getBoolOpt("autoCheck", &cfg.autoCheck);
    getBoolOpt("autoDownload", &cfg.autoDownload);
    getBoolOpt("autoInstall", &cfg.autoInstall);
    int iv = getInt("updateCheckIntervalHours");
    if (iv > 0) cfg.updateCheckIntervalHours = iv;
    int64_t le = getInt64("lastCheckEpoch");
    if (le >= 0) cfg.lastCheckEpoch = le;
    cfg.lastSeenVersion = jsonGetString(content, "lastSeenVersion");
    cfg.skipVersion = jsonGetString(content, "skipVersion");

    // Parse paired devices array
    auto arrStart = content.find("\"pairedDevices\"");
    if (arrStart != std::string::npos) {
        auto bracket = content.find('[', arrStart);
        auto bracketEnd = content.find(']', bracket);
        if (bracket != std::string::npos && bracketEnd != std::string::npos) {
            std::string arr = content.substr(bracket, bracketEnd - bracket + 1);
            size_t pos = 0;
            while (true) {
                auto objStart = arr.find('{', pos);
                if (objStart == std::string::npos) break;
                auto objEnd = arr.find('}', objStart);
                if (objEnd == std::string::npos) break;
                std::string obj = arr.substr(objStart, objEnd - objStart + 1);
                PairedDevice dev;
                dev.id = jsonGetString(obj, "id");
                dev.name = jsonGetString(obj, "name");
                dev.lastIP = jsonGetString(obj, "lastIP");
                dev.pairedAt = jsonGetString(obj, "pairedAt");
                dev.sharedKeyHex = jsonGetString(obj, "sharedKey");
                // touchpadMode (Task 1.3) — absent on pre-1.3 configs, where
                // touchpadModeFromName("") yields TOUCHPAD_MODE_DS4.
                dev.touchpadMode = touchpadModeFromName(jsonGetString(obj, "touchpadMode"));
                if (!dev.id.empty()) cfg.pairedDevices.push_back(dev);
                pos = objEnd + 1;
            }
        }
    }
    return cfg;
}

// ── Save config ─────────────────────────────────────────────────────────────
void saveConfig(const Config& cfg) {
    std::ofstream f(configPath());
    f << "{\n"
      << "  \"udpPort\": " << cfg.udpPort << ",\n"
      << "  \"webPort\": " << cfg.webPort << ",\n"
      << "  \"pairPort\": " << cfg.pairPort << ",\n"
      << "  \"discPort\": " << cfg.discPort << ",\n"
      << "  \"autoStart\": " << (cfg.autoStart ? "true" : "false") << ",\n"
      << "  \"updateChannel\": \"" << jsonEscape(cfg.updateChannel) << "\",\n"
      << "  \"autoCheck\": " << (cfg.autoCheck ? "true" : "false") << ",\n"
      << "  \"autoDownload\": " << (cfg.autoDownload ? "true" : "false") << ",\n"
      << "  \"autoInstall\": " << (cfg.autoInstall ? "true" : "false") << ",\n"
      << "  \"updateCheckIntervalHours\": " << cfg.updateCheckIntervalHours << ",\n"
      << "  \"lastCheckEpoch\": " << cfg.lastCheckEpoch << ",\n"
      << "  \"lastSeenVersion\": \"" << jsonEscape(cfg.lastSeenVersion) << "\",\n"
      << "  \"skipVersion\": \"" << jsonEscape(cfg.skipVersion) << "\",\n"
      << "  \"credentials\": \"" << jsonEscape(cfg.credentials) << "\",\n"
      << "  \"pairedDevices\": [\n";
    for (size_t i = 0; i < cfg.pairedDevices.size(); i++) {
        const auto& d = cfg.pairedDevices[i];
        f << "    {\"id\":\"" << jsonEscape(d.id) << "\",\"name\":\"" << jsonEscape(d.name)
          << "\",\"lastIP\":\"" << jsonEscape(d.lastIP) << "\",\"pairedAt\":\""
          << jsonEscape(d.pairedAt) << "\",\"sharedKey\":\"" << jsonEscape(d.sharedKeyHex)
          << "\",\"touchpadMode\":\"" << touchpadModeName(d.touchpadMode) << "\"}";
        if (i + 1 < cfg.pairedDevices.size()) f << ",";
        f << "\n";
    }
    f << "  ]\n}\n";
}

// ── Auto-start (registry) ───────────────────────────────────────────────────
void setAutoStart(bool enable) {
    HKEY key = nullptr;
    const char* run = "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run";
    if (RegOpenKeyExA(HKEY_CURRENT_USER, run, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    if (enable) {
        char exePath[MAX_PATH];
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        RegSetValueExA(key, APP_NAME, 0, REG_SZ, reinterpret_cast<const BYTE*>(exePath),
                       (DWORD)strlen(exePath) + 1);
    } else {
        RegDeleteValueA(key, APP_NAME);
    }
    RegCloseKey(key);
}

bool getAutoStart() {
    HKEY key = nullptr;
    const char* run = "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run";
    if (RegOpenKeyExA(HKEY_CURRENT_USER, run, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0, size = 0;
    bool exists = RegQueryValueExA(key, APP_NAME, nullptr, &type, nullptr, &size) == ERROR_SUCCESS;
    RegCloseKey(key);
    return exists;
}

// ── Utility ─────────────────────────────────────────────────────────────────
std::string getExeDir() {
    char buf[MAX_PATH];
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string path(buf);
    auto pos = path.find_last_of("\\/");
    return (pos != std::string::npos) ? path.substr(0, pos) : ".";
}

std::string getCurrentDate() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    char buf[32];
    struct tm tm{};
    localtime_s(&tm, &t);
    strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}
