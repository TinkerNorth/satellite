// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * config.cpp — Configuration persistence, JSON helpers, auto-start
 */
#include "config.h"

// ── JSON string escaping ────────────────────────────────────────────────────
// Escapes the JSON structural characters plus every C0 control byte (< 0x20).
// A raw control byte — a \r or \t buried in a device name, say — is invalid
// inside a JSON string and would corrupt the config file, so anything below
// 0x20 that isn't \n is emitted as a \uXXXX escape.
std::string jsonEscape(const std::string& s) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    for (char ch : s) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (c == '"') {
            out += "\\\"";
        } else if (c == '\\') {
            out += "\\\\";
        } else if (c == '\n') {
            out += "\\n";
        } else if (c < 0x20) {
            out += "\\u00";
            out += kHex[(c >> 4) & 0xF];
            out += kHex[c & 0xF];
        } else {
            out += ch;
        }
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
// SHGetKnownFolderPath supersedes the deprecated SHGetFolderPath -- the
// CSIDL_* enum has been replaced by the FOLDERID_* GUID surface since
// Vista. The wide return is converted at the boundary so the rest of
// the path handling can stay narrow (config is ASCII).
std::string configPath() {
    PWSTR raw = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &raw)) && raw) {
        std::wstring w(raw);
        CoTaskMemFree(raw);
        w += L"\\satellite";
        CreateDirectoryW(w.c_str(), nullptr);
        w += L"\\config.json";
        int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                    nullptr, 0, nullptr, nullptr);
        std::string s(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                            s.data(), n, nullptr, nullptr);
        return s;
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
    // Task 1.6 — absent on pre-1.6 configs, where the default (true) keeps the
    // legacy broadcast beacon on so discovery doesn't silently regress.
    getBoolOpt("discoveryBroadcastEnabled", &cfg.discoveryBroadcastEnabled);
    cfg.autoStart = getBool("autoStart");

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
      << "  \"discoveryBroadcastEnabled\": " << (cfg.discoveryBroadcastEnabled ? "true" : "false")
      << ",\n"
      << "  \"autoStart\": " << (cfg.autoStart ? "true" : "false") << ",\n"
      << "  \"updateChannel\": \"" << jsonEscape(cfg.updateChannel) << "\",\n"
      << "  \"autoCheck\": " << (cfg.autoCheck ? "true" : "false") << ",\n"
      << "  \"autoDownload\": " << (cfg.autoDownload ? "true" : "false") << ",\n"
      << "  \"autoInstall\": " << (cfg.autoInstall ? "true" : "false") << ",\n"
      << "  \"updateCheckIntervalHours\": " << cfg.updateCheckIntervalHours << ",\n"
      << "  \"lastCheckEpoch\": " << cfg.lastCheckEpoch << ",\n"
      << "  \"lastSeenVersion\": \"" << jsonEscape(cfg.lastSeenVersion) << "\",\n"
      << "  \"skipVersion\": \"" << jsonEscape(cfg.skipVersion) << "\",\n"
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
//
// Value name is the human-visible "Satellite" (the same literal Inno
// Setup writes from {#MyAppName}). Lookups are case-insensitive, but
// writes can rename a value to whatever case we pass -- so we hold the
// installer's casing consistently across all our writes to avoid
// ugly case flicker in Task Manager > Startup tab.
//
// Value data is always the quoted absolute path. An unquoted Run-key
// value pointing into "C:\Program Files\..." is the textbook
// binary-planting vector (Windows tries "C:\Program.exe" first).
//
// getAutoStart returns true only if the registry value EXISTS AND
// resolves to THIS exe. If the value exists but points at a stale
// install location, we return false so the UI shows the truth and the
// caller can re-enable to repair.
static const char* kRunSubkey    = "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run";
static const char* kRunValueName = "Satellite";

static std::string quotedExePath() {
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return {};
    return std::string("\"") + buf + "\"";
}

// Strip surrounding quotes for comparison; the registry value should be
// quoted but we tolerate either form for robustness against pre-1.0
// installs that wrote unquoted.
static std::string stripQuotes(const std::string& s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        return s.substr(1, s.size() - 2);
    return s;
}

void setAutoStart(bool enable) {
    HKEY key = nullptr;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, kRunSubkey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    if (enable) {
        std::string q = quotedExePath();
        if (!q.empty()) {
            RegSetValueExA(key, kRunValueName, 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(q.c_str()),
                           static_cast<DWORD>(q.size() + 1));
        }
    } else {
        RegDeleteValueA(key, kRunValueName);
    }
    RegCloseKey(key);
}

bool getAutoStart() {
    HKEY key = nullptr;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, kRunSubkey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;

    DWORD type = 0, size = 0;
    LONG r = RegQueryValueExA(key, kRunValueName, nullptr, &type, nullptr, &size);
    if (r != ERROR_SUCCESS || type != REG_SZ || size == 0) {
        RegCloseKey(key);
        return false;
    }

    std::string buf(size, '\0');
    r = RegQueryValueExA(key, kRunValueName, nullptr, &type,
                         reinterpret_cast<BYTE*>(&buf[0]), &size);
    RegCloseKey(key);
    if (r != ERROR_SUCCESS) return false;

    // Strip trailing NULs that REG_SZ may include in the byte count.
    while (!buf.empty() && buf.back() == '\0') buf.pop_back();

    char self[MAX_PATH];
    if (GetModuleFileNameA(nullptr, self, MAX_PATH) == 0) return !buf.empty();

    // Path comparison is case-insensitive on Windows. If the value
    // points at a different exe (stale install location, side-loaded
    // copy in Downloads, etc.), report autostart as disabled so the
    // UI doesn't lie to the user.
    return _stricmp(stripQuotes(buf).c_str(), self) == 0;
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
