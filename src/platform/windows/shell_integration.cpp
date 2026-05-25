// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * shell_integration.cpp -- AppUserModelID, jump list, toast helpers.
 *
 * COM is the through-line: jump lists and shortcut-property writes
 * both go through CoCreateInstance, so callers MUST have called
 * CoInitializeEx (apartment-threaded is fine) before invoking
 * refreshJumpList. main.cpp does this once at startup -- see the
 * CoInitializeEx call alongside lifecycle::registerForRestart.
 *
 * Toasts deliberately avoid the WinRT path. On Win10+, Explorer
 * auto-promotes Shell_NotifyIcon(NIM_MODIFY, NIF_INFO) notifications
 * to Action Center toasts *if and only if* the calling process has
 * a registered AUMID. That's one call + one struct vs. the WinRT
 * dance (RoInitialize, GetActivationFactory, XmlDocument, IToastNotifier);
 * the legacy API works fine for our use case (status nudges, not
 * interactive notifications).
 */
#include "shell_integration.h"

#include "config.h"
#include "globals.h"
#include "resource.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propkey.h>
#include <propvarutil.h>
#include <strsafe.h>

#include <string>

extern void logMsg(LogLevel level, const std::string& source, const std::string& message);

namespace shell_integration {

namespace {

std::wstring toWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// Pulled from app_lifecycle.cpp but inlined to keep the dependency
// graph clean (shell_integration is one layer below app_lifecycle).
std::wstring exePathWide() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return {};
    return std::wstring(buf, n);
}

// Create an IShellLinkW representing one jump-list task and stamp it
// with the AppUserModelID so Explorer routes it through our group.
// On failure returns nullptr; caller must Release().
IShellLinkW* makeShellLink(const std::wstring& target, const std::wstring& args,
                           const std::wstring& title, const std::wstring& description,
                           const std::wstring& iconPath, int iconIndex) {
    IShellLinkW* link = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                  reinterpret_cast<void**>(&link));
    if (FAILED(hr) || link == nullptr) return nullptr;

    link->SetPath(target.c_str());
    if (!args.empty()) link->SetArguments(args.c_str());
    link->SetDescription(description.c_str());
    if (!iconPath.empty()) link->SetIconLocation(iconPath.c_str(), iconIndex);

    // The IPropertyStore dance is mandatory: jump-list user tasks
    // without System.Title don't render. We also stamp the AUMID so
    // even out-of-context launches (Win+R, etc.) group correctly.
    IPropertyStore* props = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&props))) && props) {
        PROPVARIANT pv{};
        if (SUCCEEDED(InitPropVariantFromString(title.c_str(), &pv))) {
            props->SetValue(PKEY_Title, pv);
            PropVariantClear(&pv);
        }
        if (SUCCEEDED(InitPropVariantFromString(kAppUserModelID, &pv))) {
            props->SetValue(PKEY_AppUserModel_ID, pv);
            PropVariantClear(&pv);
        }
        props->Commit();
        props->Release();
    }
    return link;
}

} // namespace

bool registerAppUserModelID() {
    HRESULT hr = SetCurrentProcessExplicitAppUserModelID(kAppUserModelID);
    if (FAILED(hr)) {
        logMsg(LogLevel::WARN, "shell",
               "SetCurrentProcessExplicitAppUserModelID failed (hr=" +
                   std::to_string(static_cast<long>(hr)) + ")");
        return false;
    }
    return true;
}

bool refreshJumpList() {
    ICustomDestinationList* dst = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_DestinationList, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_ICustomDestinationList, reinterpret_cast<void**>(&dst));
    if (FAILED(hr) || dst == nullptr) {
        logMsg(LogLevel::WARN, "shell", "Jump list unavailable (legacy OS or COM not initialized)");
        return false;
    }

    // Stamp the destination list with our AUMID so it lands in the
    // pinned-app group rather than the generic "satellite.exe" bucket.
    dst->SetAppID(kAppUserModelID);

    UINT slots = 0;
    IObjectArray* removed = nullptr; // user-removed entries we must respect
    hr = dst->BeginList(&slots, IID_PPV_ARGS(&removed));
    if (FAILED(hr)) {
        dst->Release();
        return false;
    }
    if (removed) removed->Release();

    IObjectCollection* coll = nullptr;
    hr = CoCreateInstance(CLSID_EnumerableObjectCollection, nullptr, CLSCTX_INPROC_SERVER,
                          IID_IObjectCollection, reinterpret_cast<void**>(&coll));
    if (FAILED(hr) || coll == nullptr) {
        dst->Release();
        return false;
    }

    std::wstring exe = exePathWide();
    int webPort;
    {
        std::lock_guard<std::mutex> lk(g_configMtx);
        webPort = g_config.webPort;
    }
    wchar_t urlBuf[64];
    StringCchPrintfW(urlBuf, 64, L"http://localhost:%d", webPort);

    // Task 1: Open Web UI -- ShellExecute the URL via explorer.exe so
    // it lands in the user's default browser. We use the explorer
    // launcher (not satellite.exe) so the launcher's "Run" verb is
    // implicit; SetPath requires an .exe.
    wchar_t explorerExe[MAX_PATH];
    GetWindowsDirectoryW(explorerExe, MAX_PATH);
    StringCchCatW(explorerExe, MAX_PATH, L"\\explorer.exe");

    IShellLinkW* openUi = makeShellLink(explorerExe, urlBuf, L"Open Web UI",
                                        L"Open the Satellite dashboard in your browser", exe, 0);
    if (openUi) {
        coll->AddObject(openUi);
        openUi->Release();
    }

    // Task 2: Open Logs Folder. Path resolved at runtime so it tracks
    // the user's actual %LOCALAPPDATA% (matters for roaming profiles).
    PWSTR localAppData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData))) {
        std::wstring logsDir = std::wstring(localAppData) + L"\\TinkerNorth\\Satellite\\logs";
        CoTaskMemFree(localAppData);
        IShellLinkW* openLogs = makeShellLink(explorerExe, logsDir, L"Open Logs Folder",
                                              L"Show recent log files in Explorer", exe, 0);
        if (openLogs) {
            coll->AddObject(openLogs);
            openLogs->Release();
        }
    }

    // Task 3: Check for Updates. Launches a second satellite.exe
    // instance with --check-updates; the singleton mutex (see
    // app_lifecycle::acquireSingleInstance) catches that and the
    // already-running instance picks up the request via its
    // second-instance handler. Future: pass via shared memory or a
    // named pipe so the click does the right thing reliably.
    IShellLinkW* checkUpdates =
        makeShellLink(exe, L"--check-updates", L"Check for Updates",
                      L"Look for a newer Satellite release on GitHub", exe, 0);
    if (checkUpdates) {
        coll->AddObject(checkUpdates);
        checkUpdates->Release();
    }

    IObjectArray* arr = nullptr;
    if (SUCCEEDED(coll->QueryInterface(IID_PPV_ARGS(&arr))) && arr) {
        hr = dst->AddUserTasks(arr);
        arr->Release();
    }
    coll->Release();

    hr = dst->CommitList();
    dst->Release();
    return SUCCEEDED(hr);
}

void showToast(const std::string& title, const std::string& body) {
    if (g_hwnd == nullptr) return;

    // NOTIFYICONDATAW so the wide-string fields don't truncate at the
    // ANSI code page boundary for non-ASCII titles/bodies.
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uFlags = NIF_INFO | NIF_GUID;
    nid.guidItem = kTrayIconGuid;
    nid.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON | NIIF_RESPECT_QUIET_TIME;

    HICON hLarge = static_cast<HICON>(
        LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
                   GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    if (hLarge) nid.hBalloonIcon = hLarge;

    std::wstring wt = toWide(title);
    std::wstring wb = toWide(body);
    StringCchCopyW(nid.szInfoTitle, ARRAYSIZE(nid.szInfoTitle), wt.c_str());
    StringCchCopyW(nid.szInfo, ARRAYSIZE(nid.szInfo), wb.c_str());

    Shell_NotifyIconW(NIM_MODIFY, &nid);

    if (hLarge) DestroyIcon(hLarge);
}

} // namespace shell_integration
