// SPDX-License-Identifier: LGPL-3.0-or-later
#include "config.h"

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
