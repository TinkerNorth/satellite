// SPDX-License-Identifier: LGPL-3.0-or-later
#include "config.h"

// C0 control bytes (< 0x20) are invalid raw inside a JSON string and would
// corrupt the config file, so emit them as \uXXXX (except \n).
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

// SHGetKnownFolderPath (FOLDERID_*) supersedes the deprecated SHGetFolderPath
// (CSIDL_*). Converted to narrow at the boundary; config paths are ASCII.
std::string configPath() {
    PWSTR raw = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &raw)) && raw) {
        std::wstring w(raw);
        CoTaskMemFree(raw);
        w += L"\\satellite";
        CreateDirectoryW(w.c_str(), nullptr);
        w += L"\\config.json";
        int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                    nullptr, nullptr);
        std::string s(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr,
                            nullptr);
        return s;
    }
    return "config.json";
}

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
    // Only writes *out if the key is present, so absent keys keep struct defaults.
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
    // Absent on pre-1.6 configs; default (true) keeps the broadcast beacon on.
    getBoolOpt("discoveryBroadcastEnabled", &cfg.discoveryBroadcastEnabled);
    cfg.autoStart = getBool("autoStart");

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
    cfg.networkInterface = jsonGetString(content, "networkInterface");
    cfg.allowPublicNetwork = getBool("allowPublicNetwork");

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
                if (!dev.id.empty()) cfg.pairedDevices.push_back(dev);
                pos = objEnd + 1;
            }
        }
    }
    return cfg;
}

static std::wstring toWidePath(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

bool atomicWriteFile(const std::string& path, const std::string& bytes) {
    std::wstring wpath = toWidePath(path);
    if (wpath.empty()) return false;
    std::wstring wtmp = wpath + L".tmp";

    HANDLE h = CreateFileW(wtmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    bool ok = true;
    size_t off = 0;
    while (off < bytes.size()) {
        size_t remaining = bytes.size() - off;
        DWORD chunk = remaining > 0x10000000u ? 0x10000000u : static_cast<DWORD>(remaining);
        DWORD wrote = 0;
        if (!WriteFile(h, bytes.data() + off, chunk, &wrote, nullptr) || wrote == 0) {
            ok = false;
            break;
        }
        off += wrote;
    }
    if (ok) ok = FlushFileBuffers(h) != 0;
    CloseHandle(h);

    if (!ok) {
        DeleteFileW(wtmp.c_str());
        return false;
    }
    if (!MoveFileExW(wtmp.c_str(), wpath.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(wtmp.c_str());
        return false;
    }
    return true;
}

void saveConfig(const Config& cfg) {
    std::ostringstream f;
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
      << "  \"networkInterface\": \"" << jsonEscape(cfg.networkInterface) << "\",\n"
      << "  \"allowPublicNetwork\": " << (cfg.allowPublicNetwork ? "true" : "false") << ",\n"
      << "  \"pairedDevices\": [\n";
    for (size_t i = 0; i < cfg.pairedDevices.size(); i++) {
        const auto& d = cfg.pairedDevices[i];
        f << "    {\"id\":\"" << jsonEscape(d.id) << "\",\"name\":\"" << jsonEscape(d.name)
          << "\",\"lastIP\":\"" << jsonEscape(d.lastIP) << "\",\"pairedAt\":\""
          << jsonEscape(d.pairedAt) << "\",\"sharedKey\":\"" << jsonEscape(d.sharedKeyHex) << "\"}";
        if (i + 1 < cfg.pairedDevices.size()) f << ",";
        f << "\n";
    }
    f << "  ]\n}\n";
    atomicWriteFile(configPath(), f.str());
}

// Value name must match the literal Inno Setup writes ({#MyAppName}); keep its
// casing consistent across writes (lookups are case-insensitive but writes
// rename). Value data is always the quoted absolute path: an unquoted Run-key
// value into "C:\Program Files\..." is a binary-planting vector (Windows tries
// "C:\Program.exe" first).
static const char* kRunSubkey = "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run";
static const char* kRunValueName = "Satellite";

static std::string quotedExePath() {
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return {};
    return std::string("\"") + buf + "\"";
}

// Tolerate either form; pre-1.0 installs wrote the value unquoted.
static std::string stripQuotes(const std::string& s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') return s.substr(1, s.size() - 2);
    return s;
}

void setAutoStart(bool enable) {
    HKEY key = nullptr;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, kRunSubkey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS)
        return;
    if (enable) {
        std::string q = quotedExePath();
        if (!q.empty()) {
            RegSetValueExA(key, kRunValueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(q.c_str()),
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
    r = RegQueryValueExA(key, kRunValueName, nullptr, &type, reinterpret_cast<BYTE*>(&buf[0]),
                         &size);
    RegCloseKey(key);
    if (r != ERROR_SUCCESS) return false;

    // Strip trailing NULs that REG_SZ may include in the byte count.
    while (!buf.empty() && buf.back() == '\0') buf.pop_back();

    char self[MAX_PATH];
    if (GetModuleFileNameA(nullptr, self, MAX_PATH) == 0) return !buf.empty();

    // A value pointing at a different exe (stale/side-loaded) reads as disabled
    // so the UI doesn't lie. Case-insensitive per Windows path rules.
    return _stricmp(stripQuotes(buf).c_str(), self) == 0;
}

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
