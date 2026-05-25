// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * app_lifecycle.cpp -- Implementation of the process-lifecycle helpers.
 *
 * MiniDumpWriteDump is in DbgHelp.dll -- linked via -lDbgHelp in
 * build-satellite.bat. RegisterApplicationRestart is in kernel32 and
 * available since Windows Vista. SetProcessMitigationPolicy ships in
 * kernel32 on Win8+; the specific policies we set are Win10-era and
 * we GetProcAddress them so a kernel32 that pre-dates the policy enum
 * doesn't fail to link.
 */
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

constexpr const wchar_t* kAppFolder        = L"TinkerNorth\\Satellite";
constexpr const char*    kSingletonMutex   = "Local\\TinkerNorth.Satellite.Singleton.v1";
constexpr const char*    kRunKey           = "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run";
// Stable, install-set value name. The Inno Setup script writes the same
// literal string so installer + app never collide via case-insensitive
// rename. Don't change this without a migration step.
constexpr const char*    kRunValueName     = "Satellite";

// Crash-dump retention cap. A leaky build can otherwise drop a 5-20MB
// .dmp every minute and fill the user's disk. We keep this generous
// enough for any realistic debugging session but bounded.
constexpr size_t kMaxDumpFiles = 10;

// Log-file rotation. Rolls on size *or* date change -- whichever
// trips first. Daily files make it easy for a user to attach "today's
// log" without manually splicing a giant file.
constexpr size_t kMaxLogFileBytes = 5 * 1024 * 1024;
constexpr int    kLogRetentionDays = 7;

HANDLE g_singletonMutex = nullptr;

// File-logger state. Started by startFileLogger(); the thread drains
// the in-memory ring tail every second and flushes to disk.
std::atomic<bool> g_loggerStarted{false};
std::thread g_loggerThread;

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

// Resolve %LOCALAPPDATA%\TinkerNorth\Satellite\<sub>, creating each
// directory on the way. Modern SHGetKnownFolderPath replaces the
// deprecated SHGetFolderPath (which has been deprecated since Vista
// in favour of the GUID-based KNOWNFOLDERID surface).
std::string ensureSubdir(const wchar_t* sub) {
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
    return wideToUtf8(out);
}

// Trim a directory down to the N most recently modified files matching
// the given extension. Older files are deleted. Best-effort: failures
// (locked file, permission denied) are silently skipped.
void retainNewestN(const std::wstring& dir, const wchar_t* ext, size_t keep) {
    std::wstring pattern = dir + L"\\*" + ext;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    struct Entry {
        std::wstring name;
        ULONGLONG    mtime;
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

// Same idea but cut by age (days). Used for the log rotation cap so
// keep=days yields "delete anything older than N days regardless of count".
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
    std::string dir = dumpDir();
    if (dir.empty()) return EXCEPTION_EXECUTE_HANDLER;

    char ts[32];
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now);
    std::strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", &tm);

    std::string path = dir + "\\satellite-" + ts + ".dmp";
    std::wstring wpath = utf8ToWide(path);
    HANDLE f = CreateFileW(wpath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), ep, FALSE};
        // MiniDumpWithIndirectlyReferencedMemory keeps dumps small but
        // gives us enough to inspect locals. MiniDumpWithThreadInfo
        // captures per-thread state which is useful for our 5-thread
        // tray app.
        MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
            MiniDumpNormal | MiniDumpWithIndirectlyReferencedMemory |
            MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f, type,
                          ep ? &mei : nullptr, nullptr, nullptr);
        CloseHandle(f);
    }

    // Best-effort cap so leaks don't fill the user's disk. We do this
    // inside the dump filter (after writing) so the cap survives even
    // when normal shutdown never runs.
    retainNewestN(utf8ToWide(dir), L".dmp", kMaxDumpFiles);

    // Hand off to default handler so WER still runs (and the user sees
    // the standard crash dialog if they want to send to Microsoft).
    return EXCEPTION_EXECUTE_HANDLER;
}

// ── Persistent file logger ─────────────────────────────────────────
// Reads the log ring from the seq cursor we left off at and appends
// new entries to a daily file. Rotates on date change and at the size
// cap (suffix ".1", ".2", etc.).

const char* levelStr(LogLevel l) {
    switch (l) {
    case LogLevel::INFO: return "INFO ";
    case LogLevel::WARN: return "WARN ";
    case LogLevel::ERR:  return "ERROR";
    default:             return "INFO ";
    }
}

std::string isoTimestamp(std::chrono::system_clock::time_point tp) {
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
    localtime_s(&tm, &t);
    auto us = std::chrono::duration_cast<std::chrono::milliseconds>(
                  tp.time_since_epoch()) % 1000;
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
    StringCchPrintfW(name, ARRAYSIZE(name), L"satellite-%04d%02d%02d.log",
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return dir + L"\\" + name;
}

void loggerLoop() {
    using namespace std::chrono_literals;
    std::wstring dir = utf8ToWide(logDir());
    if (dir.empty()) return;

    // Trim ancient logs once on start (covers cases where the cap was
    // dialled down or the user opened the folder after a long absence).
    deleteOlderThan(dir, L".log", kLogRetentionDays);

    uint64_t lastSeq = 0;
    std::wstring currentPath = todaysLogPath(dir);
    HANDLE h = CreateFileW(currentPath.c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    while (g_appRunning.load(std::memory_order_relaxed)) {
        // Snapshot all new ring entries under the log mutex so logMsg()
        // and the writer can't race on g_logHead / g_logSeq.
        std::vector<LogEntry> drained;
        uint64_t snapshotSeq;
        {
            std::lock_guard<std::mutex> lk(g_logMtx);
            snapshotSeq = g_logSeq;
            if (snapshotSeq > lastSeq) {
                uint64_t count = std::min<uint64_t>(snapshotSeq - lastSeq, LOG_RING_SIZE);
                drained.reserve(static_cast<size_t>(count));
                int size = static_cast<int>(g_logRing.size());
                // Ring is written at g_logHead; the newest entry is
                // g_logHead-1. Replay the oldest `count` entries first.
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
            // Append a counter suffix so we don't clobber the day's
            // earlier log -- "satellite-20260525.log" rotated to
            // "satellite-20260525.1.log".
            for (int n = 1; n < 100; n++) {
                wchar_t tail[16];
                StringCchPrintfW(tail, ARRAYSIZE(tail), L".%d.log", n);
                std::wstring rotated = currentPath.substr(0, currentPath.size() - 4) + tail;
                if (MoveFileExW(currentPath.c_str(), rotated.c_str(), MOVEFILE_REPLACE_EXISTING))
                    break;
            }
            h = CreateFileW(currentPath.c_str(), FILE_APPEND_DATA,
                            FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) return;
            deleteOlderThan(dir, L".log", kLogRetentionDays);
        } else {
            std::wstring fresh = todaysLogPath(dir);
            if (fresh != currentPath) {
                CloseHandle(h);
                currentPath = fresh;
                h = CreateFileW(currentPath.c_str(), FILE_APPEND_DATA,
                                FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
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
std::string logDir()  { return ensureSubdir(L"logs"); }

bool acquireSingleInstance(const char* appTitle) {
    // Local\ namespace = per-session, so different RDP sessions or fast
    // user switching get independent instances (which is what a tray
    // app wants).
    g_singletonMutex = CreateMutexA(nullptr, FALSE, kSingletonMutex);
    if (g_singletonMutex == nullptr) return true; // best-effort -- don't block startup on this

    DWORD err = GetLastError();
    if (err != ERROR_ALREADY_EXISTS) return true; // we won the race

    // Another instance owns the mutex. Try to nudge its tray window so
    // the user sees a response to their double-click instead of silence.
    HWND existing = FindWindowExA(HWND_MESSAGE, nullptr, "ControllerForwardTray", appTitle);
    if (existing != nullptr) {
        // Custom message: existing instance's WndProc can interpret it
        // however it likes (show balloon, open web UI...). For now we
        // just send WM_USER+100 which tray.cpp can ignore safely.
        PostMessageA(existing, WM_USER + 100, 0, 0);
    }
    return false;
}

void installCrashHandler() {
    static bool installed = false;
    if (installed) return;
    installed = true;

    // Suppress Windows Error Reporting's default UI for unhandled
    // exceptions -- our filter takes over. Without this, the user sees
    // BOTH our dump-on-disk AND the standard "Satellite has stopped
    // working" dialog, which is confusing.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    SetUnhandledExceptionFilter(dumpFilter);

    // Drive a one-shot trim now (covers the common "user just rebooted
    // and reopened" path where rotation otherwise waits for the next
    // crash).
    retainNewestN(utf8ToWide(dumpDir()), L".dmp", kMaxDumpFiles);
}

void registerForRestart() {
    // Tell the shell to relaunch us with /restart after an OS-initiated
    // termination (Windows Update reboot, hibernate-induced kill,
    // Restart Manager close-for-installer). The /restart switch lets us
    // distinguish a recovery launch from a user double-click later.
    typedef HRESULT(WINAPI * RAR)(PCWSTR, DWORD);
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (!k32) return;
    RAR rar = reinterpret_cast<RAR>(GetProcAddress(k32, "RegisterApplicationRestart"));
    if (!rar) return;
    rar(L"/restart", RESTART_NO_CRASH | RESTART_NO_HANG);
}

void hardenDllSearchPath() {
    // Equivalent to SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32
    // | LOAD_LIBRARY_SEARCH_APPLICATION_DIR). Available on Win8+.
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
    // SetProcessMitigationPolicy lets us enforce mitigations that
    // were either too risky to embed at link time (binary signing
    // expectations, dynamic-code prohibition) or that aren't
    // expressible at link time at all. We GetProcAddress because the
    // policy enums grew across Win10 servicing branches and we don't
    // want a missing constant in older SDK headers to break the build.
    typedef BOOL(WINAPI * SPMP)(int, PVOID, SIZE_T);
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (!k32) return;
    SPMP set = reinterpret_cast<SPMP>(GetProcAddress(k32, "SetProcessMitigationPolicy"));
    if (!set) return;

    // ProcessImageLoadPolicy (4): refuse remote / low-IL image loads.
    // Stops the textbook "drop DLL in UNC share + LoadLibrary" attack.
    // PreferSystem32Images biases the loader toward System32 over the
    // application directory, narrowing the DLL-planting attack surface.
    struct ImageLoad {
        DWORD flags;
    } il{};
    il.flags = 0x1 /*NoRemoteImages*/ | 0x2 /*NoLowMandatoryLabelImages*/ |
               0x4 /*PreferSystem32Images*/;
    set(4, &il, sizeof(il));

    // ProcessExtensionPointDisablePolicy (5): block legacy AppInit_DLLs
    // and IME extension-point injection. We don't host any extensions.
    struct ExtPoint {
        DWORD flags;
    } ep{};
    ep.flags = 0x1; // DisableExtensionPoints
    set(5, &ep, sizeof(ep));

    // ProcessSignaturePolicy (8) and ProcessDynamicCodePolicy (2) are
    // deliberately NOT enabled here:
    //   * Signature policy refuses unsigned DLL loads. Graphics drivers'
    //     shim layers and many AV hooks ship signed but cross-signed via
    //     OEM chains the policy refuses. Tripped this in initial testing
    //     -- process silently exits at LoadLibrary time.
    //   * Dynamic-code policy blocks runtime trampolines. We don't JIT,
    //     but third-party hook DLLs (Discord overlay, RTSS, etc.) do --
    //     and they crash us early if we forbid it.
    //
    // The link-time mitigations (--dynamicbase, --nxcompat,
    // --high-entropy-va) plus the per-image policies above are the
    // pragmatic ceiling for a desktop tray app that has to coexist with
    // unsigned third-party in-process tooling.
}

// Strip surrounding quotes from a Run-value string. The shell tolerates
// either form; we tolerate either for comparison purposes.
static std::string stripQuotes(const std::string& s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        return s.substr(1, s.size() - 2);
    return s;
}

void reconcileAutoStart() {
    // Non-destructive AND non-presumptuous:
    //   * We never delete here (explicit user toggle does that).
    //   * We never overwrite an existing valid entry that points at
    //     a DIFFERENT real satellite.exe -- that lets a side-loaded
    //     dev/portable build run without trampling a regular install.
    //   * We DO write a new entry if none exists.
    //   * We DO rewrite an entry whose target file no longer exists
    //     (stale install path), since that's a self-healing case.
    //   * We DO re-normalise the entry (quoted path) if it already
    //     points at THIS exe, so case/quoting differences with what
    //     the installer wrote get cleaned up.
    if (!g_config.autoStart) return;

    char exe[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, exe, MAX_PATH);
    if (len == 0 || len == MAX_PATH) return;

    HKEY key = nullptr;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0,
                        KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS)
        return;

    // Read current value (if any) so we can decide whether to touch it.
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
        RegSetValueExA(key, kRunValueName, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(quoted.c_str()),
                       static_cast<DWORD>(quoted.size() + 1));
    }
    RegCloseKey(key);
}

void startFileLogger() {
    if (g_loggerStarted.exchange(true)) return; // idempotent
    g_loggerThread = std::thread(loggerLoop);
    g_loggerThread.detach(); // bounded by g_appRunning; OS reclaims at exit
}

} // namespace lifecycle

#pragma GCC diagnostic pop
