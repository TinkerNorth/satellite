// SPDX-License-Identifier: LGPL-3.0-or-later
#include "tray_icon.h"
#include "resource.h"
#include "shell_integration.h"
#include "core/tracked_mutex.h"

#include <strsafe.h>

#include <atomic>
#include <memory>
#include <string>

namespace trayicon {

namespace {

struct Balloon {
    std::wstring title;
    std::wstring body;
};

NOTIFYICONDATAW g_nid{};
bool g_guidActive = false;
bool g_present = false;
std::wstring g_lastTooltip;
UINT g_taskbarCreatedMsg = 0;
std::atomic<HWND> g_owner{nullptr};
ShellNotifyFn g_shellNotify = Shell_NotifyIconW;

BOOL shellNotify(DWORD op, PNOTIFYICONDATAW data) {
    blockingRegion("tray.shell");
    return g_shellNotify(op, data);
}

std::wstring composeTooltip() {
    const std::shared_ptr<const Config> cfg = g_configStore->current();
    const int udpPort = cfg->udpPort;
    const int webPort = cfg->webPort;
    bool listening = g_listening.load(std::memory_order_relaxed);

    wchar_t buf[128];
    if (listening) {
        StringCchPrintfW(buf, ARRAYSIZE(buf), L"Satellite -- listening on :%d  (web UI :%d)",
                         udpPort, webPort);
    } else {
        StringCchPrintfW(buf, ARRAYSIZE(buf), L"Satellite -- idle  (web UI :%d)", webPort);
    }
    return std::wstring(buf);
}

void fillIdentity(NOTIFYICONDATAW& n, HWND hwnd) {
    n.cbSize = sizeof(n);
    n.hWnd = hwnd;
    if (g_guidActive) {
        n.uFlags |= NIF_GUID;
        n.guidItem = shell_integration::kTrayIconGuid;
    } else {
        n.uID = 1;
    }
}

void registerIcon(HWND hwnd) {
    g_nid = {};
    g_guidActive = true;
    fillIdentity(g_nid, hwnd);
    g_nid.uFlags |= NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    g_nid.uCallbackMessage = WM_TRAYICON;

    HICON hIcon = static_cast<HICON>(
        LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    g_nid.hIcon =
        (hIcon != nullptr) ? hIcon : LoadIconW(nullptr, reinterpret_cast<LPCWSTR>(IDI_APPLICATION));

    g_lastTooltip = composeTooltip();
    StringCchCopyW(g_nid.szTip, ARRAYSIZE(g_nid.szTip), g_lastTooltip.c_str());

    if (!shellNotify(NIM_ADD, &g_nid)) {
        NOTIFYICONDATAW del{};
        del.cbSize = sizeof(del);
        del.uFlags = NIF_GUID;
        del.guidItem = shell_integration::kTrayIconGuid;
        shellNotify(NIM_DELETE, &del);
        if (!shellNotify(NIM_ADD, &g_nid)) {
            g_guidActive = false;
            g_nid.uFlags &= ~NIF_GUID;
            g_nid.uID = 1;
            shellNotify(NIM_ADD, &g_nid);
        }
    }

    g_nid.uVersion = NOTIFYICON_VERSION_4;
    shellNotify(NIM_SETVERSION, &g_nid);
    g_present = true;
}

void refreshTooltip() {
    if (!g_present) return;
    std::wstring fresh = composeTooltip();
    if (fresh == g_lastTooltip) return;
    g_lastTooltip = std::move(fresh);

    NOTIFYICONDATAW upd{};
    fillIdentity(upd, g_nid.hWnd);
    upd.uFlags |= NIF_TIP | NIF_SHOWTIP;
    StringCchCopyW(upd.szTip, ARRAYSIZE(upd.szTip), g_lastTooltip.c_str());
    shellNotify(NIM_MODIFY, &upd);

    StringCchCopyW(g_nid.szTip, ARRAYSIZE(g_nid.szTip), g_lastTooltip.c_str());
}

void showBalloonNow(const Balloon& balloon) {
    if (!g_present) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_nid.hWnd;
    nid.uFlags = NIF_INFO | NIF_GUID;
    nid.guidItem = shell_integration::kTrayIconGuid;
    nid.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON | NIIF_RESPECT_QUIET_TIME;

    HICON hLarge = static_cast<HICON>(
        LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
                   GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    if (hLarge) nid.hBalloonIcon = hLarge;

    StringCchCopyW(nid.szInfoTitle, ARRAYSIZE(nid.szInfoTitle), balloon.title.c_str());
    StringCchCopyW(nid.szInfo, ARRAYSIZE(nid.szInfo), balloon.body.c_str());

    shellNotify(NIM_MODIFY, &nid);

    if (hLarge) DestroyIcon(hLarge);
}

} // namespace

UINT taskbarCreatedMessage() {
    if (g_taskbarCreatedMsg == 0) g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
    return g_taskbarCreatedMsg;
}

void add(HWND hwnd, UINT refreshMs) {
    taskbarCreatedMessage();
    registerIcon(hwnd);
    g_owner.store(hwnd, std::memory_order_release);
    SetTimer(hwnd, REFRESH_TIMER_ID, refreshMs, nullptr);
}

void remove() {
    const HWND owner = g_owner.exchange(nullptr, std::memory_order_acq_rel);
    if (owner != nullptr) KillTimer(owner, REFRESH_TIMER_ID);
    if (!g_present) return;
    g_present = false;
    shellNotify(NIM_DELETE, &g_nid);
}

void requestRefresh() {
    const HWND owner = g_owner.load(std::memory_order_acquire);
    if (owner != nullptr) PostMessageW(owner, WM_REFRESH, 0, 0);
}

void showBalloon(std::wstring title, std::wstring body) {
    const HWND owner = g_owner.load(std::memory_order_acquire);
    if (owner == nullptr) return;
    auto balloon = std::make_unique<Balloon>(Balloon{std::move(title), std::move(body)});
    if (PostMessageW(owner, WM_BALLOON, 0, reinterpret_cast<LPARAM>(balloon.get()))) {
        balloon.release();
    }
}

bool handleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT& result) {
    result = 0;
    if (g_taskbarCreatedMsg != 0 && msg == g_taskbarCreatedMsg) {
        registerIcon(hwnd);
        return true;
    }
    switch (msg) {
    case WM_TIMER:
        if (wp != REFRESH_TIMER_ID) return false;
        refreshTooltip();
        return true;
    case WM_REFRESH:
        refreshTooltip();
        return true;
    case WM_BALLOON: {
        const std::unique_ptr<Balloon> balloon(reinterpret_cast<Balloon*>(lp));
        if (balloon) showBalloonNow(*balloon);
        return true;
    }
    }
    return false;
}

#ifdef SATELLITE_BUILD_TESTS
void setShellNotifyForTest(ShellNotifyFn fn) {
    g_shellNotify = fn != nullptr ? fn : Shell_NotifyIconW;
}
#endif

} // namespace trayicon
