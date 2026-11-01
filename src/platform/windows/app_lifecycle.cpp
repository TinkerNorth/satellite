// SPDX-License-Identifier: LGPL-3.0-or-later
#include "app_lifecycle.h"
#include "dir_tree.h"
#include "config.h"

#include "adapters/crash_adapter.h"
#include "autostart_rule.h"
#include "core/log_writer.h"
#include "core/tracked_mutex.h"

#include <DbgHelp.h>
#include <knownfolders.h>
#include <processthreadsapi.h>
#include <shlobj.h>
#include <strsafe.h>
#include <werapi.h>

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <typeinfo>
#include <vector>

namespace {

// GetProcAddress hands back FARPROC, and casting that straight to the real
// signature is what -Wcast-function-type exists to flag. GCC documents
// `void (*)()` as the one function type that matches everything, so the
// conversion goes through it: no pragma, and the warning stays armed for the
// rest of the file.
template <typename Fn> Fn procAddress(HMODULE module, const char* name) {
    using Anything = void (*)();
    return reinterpret_cast<Fn>(reinterpret_cast<Anything>(GetProcAddress(module, name)));
}

} // namespace

namespace lifecycle {

namespace {

constexpr const wchar_t* kAppFolder = L"TinkerNorth\\Satellite";
constexpr const char* kSingletonMutex = "Local\\TinkerNorth.Satellite.Singleton.v1";
constexpr const char* kRunKey = "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run";
// Must match the literal Inno Setup writes (case-insensitive rename otherwise
// collides); changing it needs a migration step.
constexpr const char* kRunValueName = "Satellite";

// Retention cap: a leaky build can drop a 5-20MB .dmp per minute.
constexpr size_t kMaxDumpFiles = 10;

// Whoever held the top-level filter before us. Sentry installs its own during
// sentry_init(), so swallowing the exception here would mean a crash is only
// ever recorded in one of the two places. We chain instead.
LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;

HANDLE g_singletonMutex = nullptr;

wchar_t g_dumpDirW[MAX_PATH] = {};
wchar_t g_dumpPathW[MAX_PATH] = {};

using AbortHandler = void (*)(int);
AbortHandler g_prevAbortHandler = nullptr;

constexpr DWORD kStatusFatalAppExit = 0x40000015;

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
    out += L"\\";
    out += sub;
    ensureDirectoryTree(out);
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

// Passes the exception on to whoever held the filter before us, or to the OS
// when nobody did. Every exit from dumpFilter goes through here so a later
// early return cannot silently drop the chain.
LONG chainOrDefault(EXCEPTION_POINTERS* ep) {
    if (g_prevFilter != nullptr) return g_prevFilter(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

struct DumpJob {
    EXCEPTION_POINTERS* ep;
    DWORD threadId;
};

DWORD WINAPI writeDumpThread(LPVOID param) {
    const DumpJob* job = static_cast<const DumpJob*>(param);
    HANDLE f = CreateFileW(g_dumpPathW, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 1;
    MINIDUMP_EXCEPTION_INFORMATION mei{job->threadId, job->ep, FALSE};
    // Small dumps that still capture locals + per-thread state.
    MINIDUMP_TYPE type =
        static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithIndirectlyReferencedMemory |
                                   MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f, type,
                      job->ep != nullptr ? &mei : nullptr, nullptr, nullptr);
    CloseHandle(f);
    return 0;
}

void writeLocalDump(EXCEPTION_POINTERS* ep) {
    if (g_dumpDirW[0] == L'\0') return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    if (FAILED(StringCchPrintfW(g_dumpPathW, ARRAYSIZE(g_dumpPathW),
                                L"%s\\satellite-%04u%02u%02u-%02u%02u%02u.dmp", g_dumpDirW,
                                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond))) {
        return;
    }

    DumpJob job{ep, GetCurrentThreadId()};
    HANDLE t = CreateThread(nullptr, 0, writeDumpThread, &job, 0, nullptr);
    if (t == nullptr) {
        writeDumpThread(&job);
        return;
    }
    WaitForSingleObject(t, 60000);
    CloseHandle(t);
}

LONG WINAPI dumpFilter(EXCEPTION_POINTERS* ep) {
    flushFileLog(kLogFlushTimeoutMs);
    writeLocalDump(ep);

    // Hand off so WER, and Sentry's filter when crash reporting is armed,
    // still run.
    return chainOrDefault(ep);
}

void logCrashLine(const char* message) {
    satellite::LogRecord record =
        satellite::makeLogRecord(satellite::logClockNowNs(), LogLevel::ERR, "crash", message);
    char line[satellite::LOG_LINE_CAPACITY];
    satellite::LogLineFormatter formatter;
    const size_t length = formatter.format(record, line, sizeof(line));
    if (writeLogLineDirect(line, length)) record.flags = satellite::LOG_RECORD_RING_ONLY;
    logWriter().submit(record);
}

void onAbortSignal(int signum) {
    char message[64];
    std::snprintf(message, sizeof(message), "abort() on thread %lu",
                  static_cast<unsigned long>(GetCurrentThreadId()));
    logCrashLine(message);
    satellite::crash::breadcrumb("crash", message);
    flushFileLog(kLogFlushTimeoutMs);

    CONTEXT ctx{};
    RtlCaptureContext(&ctx);
    EXCEPTION_RECORD rec{};
    rec.ExceptionCode = kStatusFatalAppExit;
    rec.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
#if defined(_M_ARM64) || defined(__aarch64__)
    rec.ExceptionAddress = reinterpret_cast<PVOID>(ctx.Pc);
#else
    rec.ExceptionAddress = reinterpret_cast<PVOID>(ctx.Rip);
#endif
    EXCEPTION_POINTERS ep{&rec, &ctx};
    writeLocalDump(&ep);

    if (g_prevAbortHandler != nullptr && g_prevAbortHandler != SIG_IGN) {
        g_prevAbortHandler(signum);
    }
}

void onTerminate() {
    char reason[satellite::LOG_MESSAGE_CAPACITY] = "no active exception";
    if (std::exception_ptr active = std::current_exception()) {
        try {
            std::rethrow_exception(active);
        } catch (const std::exception& e) {
            std::snprintf(reason, sizeof(reason), "%s: %s", typeid(e).name(), e.what());
        } catch (...) { std::snprintf(reason, sizeof(reason), "%s", "non-standard exception"); }
    }
    char message[satellite::LOG_MESSAGE_CAPACITY + 64];
    std::snprintf(message, sizeof(message), "std::terminate on thread %lu: %s",
                  static_cast<unsigned long>(GetCurrentThreadId()), reason);
    logCrashLine(message);
    satellite::crash::breadcrumb("crash", message);
    std::abort();
}

} // namespace

std::string dumpDir() { return ensureSubdir(L"dumps"); }
std::string logDir() { return ensureSubdir(L"logs"); }
std::string sentryDir() { return ensureSubdir(L"sentry"); }

bool acquireSingleInstance(const char* appTitle) {
    // Local\ namespace = per-session, so RDP sessions/fast user switching get
    // independent instances.
    g_singletonMutex = CreateMutexA(nullptr, FALSE, kSingletonMutex);
    if (g_singletonMutex == nullptr) return true; // best-effort; don't block startup

    DWORD err = GetLastError();
    if (err != ERROR_ALREADY_EXISTS) return true; // we won the race

    // Nudge the existing instance's tray window so a second double-click isn't
    // met with silence. tray.cpp can ignore WM_USER+100 safely.
    HWND existing = FindWindowExA(nullptr, nullptr, "ControllerForwardTray", appTitle);
    if (existing != nullptr) { PostMessageA(existing, WM_USER + 100, 0, 0); }
    return false;
}

void installCrashHandler() {
    static bool installed = false;
    if (installed) return;
    installed = true;

    // WER stays armed: it is the only recorder left for a fast-fail (stack
    // cookie, CFG, CET, abort with no handler), which never reaches a filter.
    // Only its "Satellite has stopped working" dialog is suppressed.
    SetErrorMode(SEM_FAILCRITICALERRORS);
    if (HMODULE k32 = GetModuleHandleW(L"kernel32.dll")) {
        typedef HRESULT(WINAPI * WSF)(DWORD);
        if (WSF werSetFlags = procAddress<WSF>(k32, "WerSetFlags")) {
            werSetFlags(WER_FAULT_REPORTING_NO_UI);
        }
    }

    std::wstring dumps = ensureSubdirW(L"dumps");
    StringCchCopyW(g_dumpDirW, ARRAYSIZE(g_dumpDirW), dumps.c_str());

    // Keep whatever was installed before us. crash::init() runs first in
    // WinMain, so when reporting is armed this is Sentry's filter and both it
    // and the local dumps\ artifact see the crash.
    g_prevFilter = SetUnhandledExceptionFilter(dumpFilter);

    // One-shot trim now, since rotation otherwise waits for the next crash.
    retainNewestN(dumps, L".dmp", kMaxDumpFiles);
}

void rearmCrashFilterChain() {
    LPTOP_LEVEL_EXCEPTION_FILTER prev = SetUnhandledExceptionFilter(dumpFilter);
    // Guard against chaining to ourselves, which would recurse until the stack
    // is gone. prev == dumpFilter means nothing installed after us and there is
    // nothing new to chain to.
    if (prev != dumpFilter) { g_prevFilter = prev; }

    AbortHandler prevAbort = signal(SIGABRT, onAbortSignal);
    if (prevAbort != SIG_ERR && prevAbort != onAbortSignal) { g_prevAbortHandler = prevAbort; }
}

void installTerminateHandler() { std::set_terminate(onTerminate); }

bool parseCrashTestSwitch(const std::string& cmdLine, CrashTestKind& kind) {
    const std::size_t at = cmdLine.find("/crash-test");
    if (at == std::string::npos) return false;
    if (cmdLine.compare(at, 20, "/crash-test=fastfail") == 0) {
        kind = CrashTestKind::FastFail;
    } else if (cmdLine.compare(at, 17, "/crash-test=abort") == 0) {
        kind = CrashTestKind::Abort;
    } else {
        kind = CrashTestKind::Exception;
    }
    return true;
}

void crashForTest(CrashTestKind kind) {
    if (kind == CrashTestKind::FastFail) {
        RaiseFailFastException(nullptr, nullptr, FAIL_FAST_GENERATE_EXCEPTION_ADDRESS);
    }
    if (kind == CrashTestKind::Abort) std::abort();
    RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    TerminateProcess(GetCurrentProcess(), 3);
}

void registerForRestart() {
    // /restart lets a recovery relaunch (Update reboot, Restart Manager, etc.)
    // be distinguished from a user double-click. GetProcAddress: Vista+.
    typedef HRESULT(WINAPI * RAR)(PCWSTR, DWORD);
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (!k32) return;
    RAR rar = procAddress<RAR>(k32, "RegisterApplicationRestart");
    if (!rar) return;
    rar(L"/restart", RESTART_NO_CRASH | RESTART_NO_HANG);
}

void hardenDllSearchPath() {
    // SetDefaultDllDirectories(SYSTEM32 | APPLICATION_DIR). Win8+, so GetProcAddress.
    typedef BOOL(WINAPI * SDDD)(DWORD);
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (!k32) return;
    SDDD sddd = procAddress<SDDD>(k32, "SetDefaultDllDirectories");
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
    SPMP set = procAddress<SPMP>(k32, "SetProcessMitigationPolicy");
    if (!set) return;

    // ProcessImageLoadPolicy (4): refuse remote/low-IL image loads (stops the
    // "drop DLL in UNC share + LoadLibrary" attack); prefer System32.
    struct ImageLoad {
        DWORD flags;
    } il{};
    il.flags =
        0x1 /*NoRemoteImages*/ | 0x2 /*NoLowMandatoryLabelImages*/ | 0x4 /*PreferSystem32Images*/;
    set(4, &il, sizeof(il));

    // ProcessExtensionPointDisablePolicy (5): block legacy AppInit_DLLs/IME
    // extension-point injection (we host no extensions).
    struct ExtPoint {
        DWORD flags;
    } ep{};
    ep.flags = 0x1; // DisableExtensionPoints
    set(5, &ep, sizeof(ep));

    // ProcessSignaturePolicy (8) and ProcessDynamicCodePolicy (2) are
    // deliberately NOT enabled: signature policy silently exits at LoadLibrary
    // time on OEM cross-signed driver shims/AV hooks; dynamic-code policy crashes
    // us when third-party hook DLLs (Discord overlay, RTSS) trampoline. We must
    // coexist with unsigned in-process tooling.
}

static

    // The Run entry as stored, or empty when there is none.
    std::string readRunEntry(HKEY key) {
    DWORD type = 0, size = 0;
    if (RegQueryValueExA(key, kRunValueName, nullptr, &type, nullptr, &size) != ERROR_SUCCESS ||
        type != REG_SZ || size == 0) {
        return "";
    }
    std::string existing;
    existing.resize(size);
    if (RegQueryValueExA(key, kRunValueName, nullptr, &type, reinterpret_cast<BYTE*>(&existing[0]),
                         &size) != ERROR_SUCCESS) {
        return "";
    }
    while (!existing.empty() && existing.back() == '\0') existing.pop_back();
    return existing;
}

// Never deletes (the user toggle does that); the write rule is
// runEntryNeedsWrite, which keeps an entry pointing at a DIFFERENT real exe.
void reconcileAutoStart() {
    if (!g_configStore->current()->autoStart) return;

    char exe[MAX_PATH];
    const DWORD len = GetModuleFileNameA(nullptr, exe, MAX_PATH);
    if (len == 0 || len == MAX_PATH) return;

    HKEY key = nullptr;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;

    const std::string existing = readRunEntry(key);
    const std::string existingPath = stripQuotes(existing);
    const bool existingTargetExists =
        !existingPath.empty() &&
        GetFileAttributesA(existingPath.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (runEntryNeedsWrite(existing, exe, existingTargetExists)) {
        const std::string quoted = std::string("\"") + exe + "\"";
        RegSetValueExA(key, kRunValueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(quoted.c_str()),
                       static_cast<DWORD>(quoted.size() + 1));
    }
    RegCloseKey(key);
}

} // namespace lifecycle
