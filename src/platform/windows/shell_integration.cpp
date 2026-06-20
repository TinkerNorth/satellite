// SPDX-License-Identifier: LGPL-3.0-or-later
// Callers MUST have called CoInitializeEx before refreshJumpList: jump lists
// and shortcut-property writes go through CoCreateInstance.
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

// Inlined rather than shared from app_lifecycle: shell_integration is the lower layer.
std::wstring exePathWide() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return {};
    return std::wstring(buf, n);
}

// Caller must Release(). nullptr on failure.
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

    // Jump-list user tasks without System.Title don't render. AUMID stamp groups
    // even out-of-context launches (Win+R).
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

    // AUMID so the list lands in the pinned-app group, not the generic bucket.
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

    // Via explorer.exe because SetPath requires an .exe, not a URL; this opens
    // the default browser.
    wchar_t explorerExe[MAX_PATH];
    GetWindowsDirectoryW(explorerExe, MAX_PATH);
    StringCchCatW(explorerExe, MAX_PATH, L"\\explorer.exe");

    IShellLinkW* openUi = makeShellLink(explorerExe, urlBuf, L"Open Web UI",
                                        L"Open the Satellite dashboard in your browser", exe, 0);
    if (openUi) {
        coll->AddObject(openUi);
        openUi->Release();
    }

    // Resolve at runtime so it tracks the actual %LOCALAPPDATA% (roaming profiles).
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

    // Launches a second satellite.exe with --check-updates; the singleton mutex
    // routes it to the running instance's second-instance handler.
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

    // Wide variant so non-ASCII titles/bodies don't truncate at the ANSI boundary.
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
