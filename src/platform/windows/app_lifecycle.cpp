// SPDX-License-Identifier: LGPL-3.0-or-later
#include "app_lifecycle.h"
#include "config.h"

#include <DbgHelp.h>
#include <knownfolders.h>
#include <processthreadsapi.h>
#include <shlobj.h>
#include <strsafe.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <fstream>
#include <thread>
#include <vector>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"

extern void logMsg(LogLevel level, const std::string& source, const std::string& message);

namespace lifecycle {

namespace {

constexpr const wchar_t* kAppFolder = L"TinkerNorth\\Satellite";
constexpr const char* kSingletonMutex = "Local\\TinkerNorth.Satellite.Singleton.v1";
constexpr const char* kRunKey = "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run";
// Must match the literal Inno Setup writes (case-insensitive rename otherwise
// collides); changing it needs a migration step.
constexpr const char* kRunValueName = "Satellite";

// Retention cap -- a leaky build can drop a 5-20MB .dmp per minute.
constexpr size_t kMaxDumpFiles = 10;

// Rolls on size or date change, whichever trips first.
constexpr size_t kMaxLogFileBytes = 5 * 1024 * 1024;
constexpr int kLogRetentionDays = 7;

HANDLE g_singletonMutex = nullptr;

std::atomic<bool> g_loggerStarted{false};
std::thread g_loggerThread;

wchar_t g_dumpDirW[MAX_PATH] = {};

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr,
                        nullptr);
    return s;
}

// Resolve %LOCALAPPDATA%\TinkerNorth\Satellite\<sub>, creating dirs on the way.
std::wstring ensureSubdirW(const wchar_t* sub) {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)) || raw == nullptr)
        return {};
    std::wstring out(raw);
    CoTaskMemFree(raw);

    out += L"\\";
    out += kAppFolder;
    CreateDirectoryW(out.c_str(), nullptr); // ok if exists
    out += L"\\";
    out += sub;
    CreateDirectoryW(out.c_str(), nullptr);
    return out;
}

std::string ensureSubdir(const wchar_t* sub) { return wideToUtf8(ensureSubdirW(sub)); }

// Keep the N newest files matching `ext`, delete the rest. Best-effort.
void retainNewestN(const std::wstring& dir, const wchar_t* ext, size_t keep) {
    std::wstring pattern = dir + L"\\*" + ext;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    struct Entry {
        std::wstring name;
        ULONGLONG mtime;
    };
    std::vector<Entry> entries;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        ULONGLONG t = (static_cast<ULONGLONG>(fd.ftLastWriteTime.dwHighDateTime) << 32) |
                      fd.ftLastWriteTime.dwLowDateTime;
        entries.push_back({fd.cFileName, t});
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    if (entries.size() <= keep) return;
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.mtime > b.mtime; });
    for (size_t i = keep; i < entries.size(); i++) {
        std::wstring full = dir + L"\\" + entries[i].name;
        DeleteFileW(full.c_str());
    }
}

// Delete files older than `days` regardless of count (log rotation cap).
void deleteOlderThan(const std::wstring& dir, const wchar_t* ext, int days) {
    std::wstring pattern = dir + L"\\*" + ext;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    FILETIME ftNow;
    GetSystemTimeAsFileTime(&ftNow);
    ULONGLONG now = (static_cast<ULONGLONG>(ftNow.dwHighDateTime) << 32) | ftNow.dwLowDateTime;
    const ULONGLONG kFtPerDay = 10000000ULL * 60ULL * 60ULL * 24ULL;
    ULONGLONG cutoff = now - static_cast<ULONGLONG>(days) * kFtPerDay;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        ULONGLONG t = (static_cast<ULONGLONG>(fd.ftLastWriteTime.dwHighDateTime) << 32) |
                      fd.ftLastWriteTime.dwLowDateTime;
        if (t < cutoff) {
            std::wstring full = dir + L"\\" + fd.cFileName;
            DeleteFileW(full.c_str());
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

LONG WINAPI dumpFilter(EXCEPTION_POINTERS* ep) {
    if (g_dumpDirW[0] == L'\0') return EXCEPTION_EXECUTE_HANDLER;

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t path[MAX_PATH];
    if (FAILED(StringCchPrintfW(path, ARRAYSIZE(path),
                                L"%s\\satellite-%04u%02u%02u-%02u%02u%02u.dmp", g_dumpDirW,
                                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond))) {
        return EXCEPTION_EXECUTE_HANDLER;
    }

    HANDLE f =
        CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), ep, FALSE};
        // Small dumps that still capture locals + per-thread state.
        MINIDUMP_TYPE type =
            static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithIndirectlyReferencedMemory |
                                       MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f, type, ep ? &mei : nullptr,
                          nullptr, nullptr);
        CloseHandle(f);
    }

    // Hand off to the default handler so WER still runs.
    return EXCEPTION_EXECUTE_HANDLER;
}

const char* levelStr(LogLevel l) {
    switch (l) {
    case LogLevel::INFO:
        return "INFO ";
    case LogLevel::WARN:
        return "WARN ";
    case LogLevel::ERR:
        return "ERROR";
    default:
        return "INFO ";
    }
}

std::string isoTimestamp(std::chrono::system_clock::time_point tp) {
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
    localtime_s(&tm, &t);
    auto us = std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()) % 1000;
    char buf[40];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    char out[60];
    StringCchPrintfA(out, sizeof(out), "%s.%03lld", buf, static_cast<long long>(us.count()));
    return out;
}

std::wstring todaysLogPath(const std::wstring& dir) {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now);
    wchar_t name[32];
    StringCchPrintfW(name, ARRAYSIZE(name), L"satellite-%04d%02d%02d.log", tm.tm_year + 1900,
                     tm.tm_mon + 1, tm.tm_mday);
    return dir + L"\\" + name;
}

void loggerLoop() {
    using namespace std::chrono_literals;
    std::wstring dir = utf8ToWide(logDir());
    if (dir.empty()) return;

    deleteOlderThan(dir, L".log", kLogRetentionDays);

    uint64_t lastSeq = 0;
    std::wstring currentPath = todaysLogPath(dir);
    HANDLE h =
        CreateFileW(currentPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_DELETE,
                    nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    while (g_appRunning.load(std::memory_order_relaxed)) {
        std::vector<LogEntry> drained;
        uint64_t snapshotSeq;
        {
            std::lock_guard<std::mutex> lk(g_logMtx); // serialise with logMsg() ring writes
            snapshotSeq = g_logSeq;
            if (snapshotSeq > lastSeq) {
                uint64_t count = std::min<uint64_t>(snapshotSeq - lastSeq, LOG_RING_SIZE);
                drained.reserve(static_cast<size_t>(count));
                int size = static_cast<int>(g_logRing.size());
                // Newest entry is g_logHead-1; replay the oldest `count` first.
                int start = (g_logHead - static_cast<int>(count) + size) % size;
                for (uint64_t i = 0; i < count; i++) {
                    int idx = (start + static_cast<int>(i)) % size;
                    drained.push_back(g_logRing[idx]);
                }
            }
        }

        for (const auto& e : drained) {
            std::string line = isoTimestamp(e.timestamp);
            line += " [";
            line += levelStr(e.level);
            line += "] [";
            line += e.source;
            line += "] ";
            line += e.message;
            line += "\r\n";
            DWORD wrote = 0;
            WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &wrote, nullptr);
        }
        if (!drained.empty()) FlushFileBuffers(h);
        lastSeq = snapshotSeq;

        // Rotation: date change or size cap. Reopen on the new path.
        LARGE_INTEGER size{};
        if (GetFileSizeEx(h, &size) && static_cast<size_t>(size.QuadPart) >= kMaxLogFileBytes) {
            CloseHandle(h);
            // Counter suffix so the day's earlier log isn't clobbered
            // (satellite-20260525.log -> satellite-20260525.1.log).
            for (int n = 1; n < 100; n++) {
                wchar_t tail[16];
                StringCchPrintfW(tail, ARRAYSIZE(tail), L".%d.log", n);
                std::wstring rotated = currentPath.substr(0, currentPath.size() - 4) + tail;
                if (MoveFileExW(currentPath.c_str(), rotated.c_str(), MOVEFILE_REPLACE_EXISTING))
                    break;
            }
            h = CreateFileW(currentPath.c_str(), FILE_APPEND_DATA,
                            FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) return;
            deleteOlderThan(dir, L".log", kLogRetentionDays);
        } else {
            std::wstring fresh = todaysLogPath(dir);
            if (fresh != currentPath) {
                CloseHandle(h);
                currentPath = fresh;
                h = CreateFileW(currentPath.c_str(), FILE_APPEND_DATA,
                                FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h == INVALID_HANDLE_VALUE) return;
                deleteOlderThan(dir, L".log", kLogRetentionDays);
            }
        }

        std::this_thread::sleep_for(1s);
    }
    CloseHandle(h);
}

} // namespace

std::string dumpDir() { return ensureSubdir(L"dumps"); }
std::string logDir() { return ensureSubdir(L"logs"); }

bool acquireSingleInstance(const char* appTitle) {
    // Local\ namespace = per-session, so RDP sessions / fast user switching get
    // independent instances (what a tray app wants).
    g_singletonMutex = CreateMutexA(nullptr, FALSE, kSingletonMutex);
    if (g_singletonMutex == nullptr) return true; // best-effort -- don't block startup

    DWORD err = GetLastError();
    if (err != ERROR_ALREADY_EXISTS) return true; // we won the race

    // Nudge the existing instance's tray window so a second double-click isn't
    // met with silence. WM_USER+100 -- tray.cpp can ignore it safely.
    HWND existing = FindWindowExA(HWND_MESSAGE, nullptr, "ControllerForwardTray", appTitle);
    if (existing != nullptr) { PostMessageA(existing, WM_USER + 100, 0, 0); }
    return false;
}

void installCrashHandler() {
    static bool installed = false;
    if (installed) return;
    installed = true;

    // Suppress WER's default UI so the user doesn't see both our dump and the
    // standard "Satellite has stopped working" dialog.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    std::wstring dumps = ensureSubdirW(L"dumps");
    StringCchCopyW(g_dumpDirW, ARRAYSIZE(g_dumpDirW), dumps.c_str());

    SetUnhandledExceptionFilter(dumpFilter);

    // One-shot trim now, since rotation otherwise waits for the next crash.
    retainNewestN(dumps, L".dmp", kMaxDumpFiles);
}

void registerForRestart() {
    // /restart lets a recovery relaunch (Update reboot, Restart Manager, etc.)
    // be distinguished from a user double-click. GetProcAddress: Vista+.
    typedef HRESULT(WINAPI * RAR)(PCWSTR, DWORD);
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (!k32) return;
    RAR rar = reinterpret_cast<RAR>(GetProcAddress(k32, "RegisterApplicationRestart"));
    if (!rar) return;
    rar(L"/restart", RESTART_NO_CRASH | RESTART_NO_HANG);
}

void hardenDllSearchPath() {
    // SetDefaultDllDirectories(SYSTEM32 | APPLICATION_DIR). Win8+, so GetProcAddress.
    typedef BOOL(WINAPI * SDDD)(DWORD);
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (!k32) return;
    SDDD sddd = reinterpret_cast<SDDD>(GetProcAddress(k32, "SetDefaultDllDirectories"));
    if (sddd) {
        // 0x00000800 = LOAD_LIBRARY_SEARCH_SYSTEM32
        // 0x00000200 = LOAD_LIBRARY_SEARCH_APPLICATION_DIR
        sddd(0x00000800 | 0x00000200);
    }
}

void applyRuntimeMitigations() {
    // GetProcAddress because the policy enums grew across Win10 servicing
    // branches and older SDK headers may lack the constants.
    typedef BOOL(WINAPI * SPMP)(int, PVOID, SIZE_T);
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (!k32) return;
    SPMP set = reinterpret_cast<SPMP>(GetProcAddress(k32, "SetProcessMitigationPolicy"));
    if (!set) return;

    // ProcessImageLoadPolicy (4): refuse remote / low-IL image loads (stops the
    // "drop DLL in UNC share + LoadLibrary" attack); prefer System32.
    struct ImageLoad {
        DWORD flags;
    } il{};
    il.flags =
        0x1 /*NoRemoteImages*/ | 0x2 /*NoLowMandatoryLabelImages*/ | 0x4 /*PreferSystem32Images*/;
    set(4, &il, sizeof(il));

    // ProcessExtensionPointDisablePolicy (5): block legacy AppInit_DLLs / IME
    // extension-point injection (we host no extensions).
    struct ExtPoint {
        DWORD flags;
    } ep{};
    ep.flags = 0x1; // DisableExtensionPoints
    set(5, &ep, sizeof(ep));

    // ProcessSignaturePolicy (8) and ProcessDynamicCodePolicy (2) are
    // deliberately NOT enabled: signature policy silently exits at LoadLibrary
    // time on OEM cross-signed driver shims / AV hooks; dynamic-code policy
    // crashes us when third-party hook DLLs (Discord overlay, RTSS) trampoline.
    // We have to coexist with unsigned in-process tooling.
}

static std::string stripQuotes(const std::string& s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') return s.substr(1, s.size() - 2);
    return s;
}

void reconcileAutoStart() {
    // Never deletes (user toggle does that) and never overwrites an entry
    // pointing at a DIFFERENT real exe (lets a side-loaded build coexist).
    // Writes if absent, rewrites if the target is gone (self-heal), or
    // re-normalises quoting/case if it already points at this exe.
    if (!g_config.autoStart) return;

    char exe[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, exe, MAX_PATH);
    if (len == 0 || len == MAX_PATH) return;

    HKEY key = nullptr;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;

    DWORD type = 0, size = 0;
    std::string existing;
    if (RegQueryValueExA(key, kRunValueName, nullptr, &type, nullptr, &size) == ERROR_SUCCESS &&
        type == REG_SZ && size > 0) {
        existing.resize(size);
        if (RegQueryValueExA(key, kRunValueName, nullptr, &type,
                             reinterpret_cast<BYTE*>(&existing[0]), &size) == ERROR_SUCCESS) {
            while (!existing.empty() && existing.back() == '\0') existing.pop_back();
        } else {
            existing.clear();
        }
    }

    bool shouldWrite = false;
    if (existing.empty()) {
        shouldWrite = true;
    } else {
        std::string existingPath = stripQuotes(existing);
        if (_stricmp(existingPath.c_str(), exe) == 0) {
            shouldWrite = true;
        } else if (GetFileAttributesA(existingPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            shouldWrite = true;
        } else {
            shouldWrite = false;
        }
    }

    if (shouldWrite) {
        std::string quoted = std::string("\"") + exe + "\"";
        RegSetValueExA(key, kRunValueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(quoted.c_str()),
                       static_cast<DWORD>(quoted.size() + 1));
    }
    RegCloseKey(key);
}

void startFileLogger() {
    if (g_loggerStarted.exchange(true)) return; // idempotent
    g_loggerThread = std::thread(loggerLoop);
}

void stopFileLogger() {
    if (g_loggerThread.joinable()) g_loggerThread.join();
}

} // namespace lifecycle

#pragma GCC diagnostic pop
