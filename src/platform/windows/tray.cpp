// SPDX-License-Identifier: LGPL-3.0-or-later
#include "tray.h"
#include "app_lifecycle.h"
#include "config.h"
#include "resource.h"
#include "shell_integration.h"
#include "core/update_service.h"
#include "net/pairing.h"
#include "net/pairing_service.h"
#include "toast.h"

#include <strsafe.h>

#include <algorithm>
#include <commctrl.h>
#include <deque>
#include <mutex>
#include <string>

static NOTIFYICONDATAW g_nid{};
static std::mutex g_nidMtx;          // updateTooltip + WM_TRAYICON contend
static bool g_nidGuidActive = false; // true when NIF_GUID succeeded
static std::wstring g_lastTooltip;   // remember last text so we don't churn NIM_MODIFY

// Registered window message broadcast by Explorer when the taskbar is
// (re)created. Identifier is initialised lazily on first add.
static UINT g_taskbarCreatedMsg = 0;

// index → deviceId for tray-menu pairing items. GUI-thread only; no lock.
static std::vector<std::string> g_menuPairIds;

// Requests arrive on the HTTP thread but the WinRT toast (+ COM) must run on the
// COM-initialised GUI thread, so queue the deviceId and post WM_PAIR_NOTIFY.
static const UINT WM_PAIR_NOTIFY = WM_USER + 101;
static std::mutex g_incomingMtx;
static std::deque<std::string> g_incoming;

static std::wstring toWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// szTip caps at 128 wchars and most shells truncate hover text near 64.
static std::wstring composeTooltip() {
    int udpPort, webPort;
    {
        std::lock_guard<std::mutex> lk(g_configMtx);
        udpPort = g_config.udpPort;
        webPort = g_config.webPort;
    }
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

static void fillIdentity(NOTIFYICONDATAW& n, HWND hwnd) {
    n.cbSize = sizeof(n);
    n.hWnd = hwnd;
    if (g_nidGuidActive) {
        n.uFlags |= NIF_GUID;
        n.guidItem = shell_integration::kTrayIconGuid;
    } else {
        n.uID = 1;
    }
}

static void registerTrayIcon(HWND hwnd) {
    std::lock_guard<std::mutex> lk(g_nidMtx);

    g_nid = {};
    g_nidGuidActive = true; // optimistic; cleared on failure below
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

    // NIF_GUID add can fail when the GUID is still registered to a stale exe
    // path (Explorer won't remap silently). Delete + retry; if that also fails
    // (corrupt tray cache, Win7), drop NIF_GUID and lose cross-install identity.
    if (!Shell_NotifyIconW(NIM_ADD, &g_nid)) {
        NOTIFYICONDATAW del{};
        del.cbSize = sizeof(del);
        del.uFlags = NIF_GUID;
        del.guidItem = shell_integration::kTrayIconGuid;
        Shell_NotifyIconW(NIM_DELETE, &del);
        if (!Shell_NotifyIconW(NIM_ADD, &g_nid)) {
            g_nidGuidActive = false;
            g_nid.uFlags &= ~NIF_GUID;
            g_nid.uID = 1;
            Shell_NotifyIconW(NIM_ADD, &g_nid); // last-resort legacy mode
        }
    }

    // Must follow NIM_ADD. v4 changes WM_TRAYICON layout: LOWORD(lp)=event,
    // HIWORD(lp)=uID, WPARAM=packed cursor x/y (see WM_TRAYICON handler).
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
}

void addTrayIcon(HWND hwnd) {
    if (g_taskbarCreatedMsg == 0) g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
    registerTrayIcon(hwnd);
}

void removeTrayIcon() {
    std::lock_guard<std::mutex> lk(g_nidMtx);
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

void updateTrayTooltip() {
    std::lock_guard<std::mutex> lk(g_nidMtx);
    std::wstring fresh = composeTooltip();
    if (fresh == g_lastTooltip) return; // no Explorer round-trip if unchanged
    g_lastTooltip = std::move(fresh);

    NOTIFYICONDATAW upd{};
    fillIdentity(upd, g_nid.hWnd);
    upd.uFlags |= NIF_TIP | NIF_SHOWTIP;
    StringCchCopyW(upd.szTip, ARRAYSIZE(upd.szTip), g_lastTooltip.c_str());
    Shell_NotifyIconW(NIM_MODIFY, &upd);

    StringCchCopyW(g_nid.szTip, ARRAYSIZE(g_nid.szTip), g_lastTooltip.c_str());
}

void showTrayMenu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();

    // Pending pairing requests at the top; the id→deviceId map is snapshotted so
    // WM_COMMAND can resolve the click.
    auto pairReqs = pendingPairRequests();
    g_menuPairIds.clear();
    for (size_t i = 0; i < pairReqs.size(); i++) {
        const auto& r = pairReqs[i];
        std::wstring label = L"Pairing: " +
                             toWide(r.deviceName.empty() ? r.clientIP : r.deviceName) + L" (" +
                             toWide(r.clientIP) + L")…";
        AppendMenuW(menu, MF_STRING, IDM_PAIR_REVIEW_BASE + static_cast<UINT>(i), label.c_str());
        g_menuPairIds.push_back(r.deviceId);
    }
    if (!g_menuPairIds.empty()) AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(menu, MF_STRING, IDM_OPEN_UI, L"Open Web UI");
    AppendMenuW(menu, MF_STRING, IDM_DONATE, L"Donate");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // Rebuilt each open so the label reflects current update state. Disabled
    // fallback when the updater isn't wired keeps the menu shape stable.
    if (g_updateService) {
        UpdateStatusSnapshot snap = g_updateService->snapshot();
        if (snap.state == UpdateState::Downloaded && snap.info.available) {
            std::wstring label = L"Install Update v" + toWide(snap.info.version);
            AppendMenuW(menu, MF_STRING, IDM_INSTALL_UPDATE, label.c_str());
        } else if (snap.state == UpdateState::UpdateAvailable && snap.info.available) {
            std::wstring label = L"Download Update v" + toWide(snap.info.version) + L"...";
            AppendMenuW(menu, MF_STRING, IDM_INSTALL_UPDATE, label.c_str());
        } else if (snap.state == UpdateState::Downloading || snap.state == UpdateState::Verifying) {
            AppendMenuW(menu, MF_STRING | MF_GRAYED, IDM_CHECK_UPDATES, L"Downloading update...");
        } else if (snap.state == UpdateState::Checking) {
            AppendMenuW(menu, MF_STRING | MF_GRAYED, IDM_CHECK_UPDATES, L"Checking for updates...");
        } else {
            AppendMenuW(menu, MF_STRING, IDM_CHECK_UPDATES, L"Check for Updates...");
        }
    } else {
        AppendMenuW(menu, MF_STRING | MF_GRAYED, IDM_CHECK_UPDATES, L"Check for Updates...");
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_OPEN_LOGS, L"Open Logs Folder");
    AppendMenuW(menu, MF_STRING, IDM_REPORT_PROBLEM, L"Report a Problem...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");

    // Required or the menu fails to dismiss on click-outside (Win32 footgun).
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

// Ping from acquireSingleInstance: a second launch attempt; we open the web UI.
static const UINT WM_SECOND_INSTANCE = WM_USER + 100;

static void openUrl(const wchar_t* url) {
    ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
}

static std::wstring webUiUrl(const wchar_t* path = L"") {
    int webPort;
    {
        std::lock_guard<std::mutex> lk(g_configMtx);
        webPort = g_config.webPort;
    }
    wchar_t buf[128];
    StringCchPrintfW(buf, ARRAYSIZE(buf), L"http://localhost:%d%s", webPort, path);
    return std::wstring(buf);
}

// deviceIds awaiting a balloon click queue here (the toast carries no payload).
static std::mutex g_pairQueueMtx;
static std::deque<std::string> g_pendingPrompts;

// Accept/reject dialog for one request. The auth is the operator's visual match
// of the displayed PIN against the device (see net/pairing.h).
static void showPairingDialogWindows(HWND hwnd, const std::string& deviceId) {
    std::string name, ip, pin;
    int secs = 0;
    // Re-snapshot at click time: the request may have expired or been handled
    // from the dashboard since the toast.
    if (!pairRequestSnapshot(deviceId, name, ip, pin, secs)) return;

    const std::wstring wInstr =
        toWide((name.empty() ? std::string("A device") : name) + " wants to pair");
    const std::wstring wContent =
        toWide("From " + ip + "\n\nPIN on the device:  " + pin +
               "\n\nConfirm this matches the PIN shown on the device, then choose Accept.");

    const int kAccept = 1001;
    const int kReject = 1002;
    const TASKDIALOG_BUTTON buttons[] = {{kAccept, L"Accept"}, {kReject, L"Reject"}};
    TASKDIALOGCONFIG cfg{};
    cfg.cbSize = sizeof(cfg);
    cfg.hwndParent = hwnd;
    cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    cfg.pszWindowTitle = L"Satellite — pairing request";
    cfg.pszMainIcon = TD_INFORMATION_ICON;
    cfg.pszMainInstruction = wInstr.c_str();
    cfg.pszContent = wContent.c_str();
    cfg.cButtons = ARRAYSIZE(buttons);
    cfg.pButtons = buttons;
    cfg.nDefaultButton = kReject; // safe default if the operator just hits Enter

    int pressed = 0;
    if (TaskDialogIndirect(&cfg, &pressed, nullptr, nullptr) != S_OK) return;
    if (pressed == kAccept) {
        confirmPairing(deviceId);
    } else if (pressed == kReject) {
        declinePairing(deviceId);
    }
    // Esc / cancel: leave it pending — the dashboard or the TTL handles it.
}

// pairing.cpp listener (registered from main). Fires on the HTTP thread; the
// WinRT toast must be raised on the COM-initialised GUI thread, so hand off via
// the queue and wake the message loop.
void notifyPairRequestWindows(const std::string& deviceId) {
    {
        std::lock_guard<std::mutex> lk(g_incomingMtx);
        g_incoming.push_back(deviceId);
    }
    if (g_hwnd != nullptr) PostMessageW(g_hwnd, WM_PAIR_NOTIFY, 0, 0);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // Explorer (re)created its taskbar (crash / RDP reconnect); re-add our icon
    // so we're not running-but-invisible.
    if (g_taskbarCreatedMsg != 0 && msg == g_taskbarCreatedMsg) {
        registerTrayIcon(hwnd);
        return 0;
    }

    switch (msg) {
    case WM_TRAYICON: {
        // v4: event is LOWORD(lp), not the whole lp (v3) -- comparing the whole
        // lp against WM_LBUTTONDBLCLK never matches under v4.
        UINT event = LOWORD(lp);
        if (event == WM_LBUTTONDBLCLK || event == NIN_KEYSELECT) {
            openUrl(webUiUrl().c_str());
        } else if (event == WM_CONTEXTMENU || event == WM_RBUTTONUP) {
            showTrayMenu(hwnd);
        } else if (event == NIN_BALLOONUSERCLICK) {
            // A queued prompt means the clicked toast was a pairing request.
            std::deque<std::string> todo;
            {
                std::lock_guard<std::mutex> lk(g_pairQueueMtx);
                todo.swap(g_pendingPrompts);
            }
            if (!todo.empty()) {
                for (const auto& id : todo) showPairingDialogWindows(hwnd, id);
                return 0;
            }
            // Else: staged update → settings page; otherwise the dashboard.
            if (g_updateService) {
                UpdateStatusSnapshot s = g_updateService->snapshot();
                if (s.info.available) {
                    openUrl(webUiUrl(L"/settings").c_str());
                    return 0;
                }
            }
            openUrl(webUiUrl().c_str());
        }
        return 0;
    }

    case WM_SECOND_INSTANCE: {
        openUrl(webUiUrl().c_str());
        return 0;
    }

    case WM_COPYDATA: {
        // A protocol-activated toast button launched a second process that
        // forwarded the satellite-pair: URI here.
        auto* cds = reinterpret_cast<COPYDATASTRUCT*>(lp);
        if (cds != nullptr && cds->dwData == PAIR_URI_COPYDATA && cds->lpData != nullptr) {
            const size_t n = cds->cbData > 0 ? cds->cbData - 1 : 0;
            handlePairProtocolUri(std::string(static_cast<const char*>(cds->lpData), n));
        }
        return TRUE;
    }

    case WM_PAIR_NOTIFY: {
        // Drained on the GUI thread (COM-initialised), so the WinRT toast works.
        std::deque<std::string> todo;
        {
            std::lock_guard<std::mutex> lk(g_incomingMtx);
            todo.swap(g_incoming);
        }
        for (const auto& id : todo) {
            std::string name, ip, pin;
            int secs = 0;
            if (!pairRequestSnapshot(id, name, ip, pin, secs)) continue;
            // Prefer the actionable toast; else a balloon whose click is handled
            // via the g_pendingPrompts queue under NIN_BALLOONUSERCLICK.
            if (showActionablePairToast(id, name, ip, pin)) continue;
            {
                std::lock_guard<std::mutex> lk(g_pairQueueMtx);
                if (std::find(g_pendingPrompts.begin(), g_pendingPrompts.end(), id) ==
                    g_pendingPrompts.end()) {
                    g_pendingPrompts.push_back(id);
                }
            }
            shell_integration::showToast("Pairing request",
                                         (name.empty() ? std::string("A device") : name) + " (" +
                                             ip + ") wants to pair. Click to accept or reject.");
        }
        return 0;
    }

    case WM_COMMAND: {
        const UINT cmd = LOWORD(wp);
        if (cmd >= IDM_PAIR_REVIEW_BASE &&
            cmd < IDM_PAIR_REVIEW_BASE + static_cast<UINT>(g_menuPairIds.size())) {
            showPairingDialogWindows(hwnd, g_menuPairIds[cmd - IDM_PAIR_REVIEW_BASE]);
            return 0;
        }
        switch (cmd) {
        default:
            break;
        case IDM_OPEN_UI:
            openUrl(webUiUrl().c_str());
            break;
        case IDM_DONATE:
            openUrl(webUiUrl(L"/donate").c_str());
            break;
        case IDM_OPEN_LOGS: {
            std::string dir = lifecycle::logDir();
            if (!dir.empty()) {
                std::wstring wd = toWide(dir);
                ShellExecuteW(nullptr, L"open", wd.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            break;
        }
        case IDM_REPORT_PROBLEM: {
            // GitHub issue with a URL-encoded body pre-filling the log/dump paths.
            std::wstring url =
                L"https://github.com/TinkerNorth/satellite/issues/new"
                L"?labels=bug"
                L"&title=&body="
                L"Describe%20the%20problem%3A%0A%0A%0A---%0A"
                L"Logs%3A%20%25LOCALAPPDATA%25%5CTinkerNorth%5CSatellite%5Clogs%0A"
                L"Crash%20dumps%3A%20%25LOCALAPPDATA%25%5CTinkerNorth%5CSatellite%5Cdumps%0A"
                L"Please%20zip%20any%20relevant%20.log%20or%20.dmp%20files%20and%20attach.";
            openUrl(url.c_str());
            break;
        }
        case IDM_CHECK_UPDATES:
            if (g_updateService) g_updateService->requestCheck(/*userInitiated=*/true);
            openUrl(webUiUrl(L"/settings").c_str());
            break;
        case IDM_INSTALL_UPDATE:
            if (g_updateService) {
                UpdateStatusSnapshot s = g_updateService->snapshot();
                if (s.state == UpdateState::Downloaded) {
                    g_updateService->requestInstall();
                } else {
                    g_updateService->requestDownload();
                }
                openUrl(webUiUrl(L"/settings").c_str());
            }
            break;
        case IDM_EXIT:
            PostQuitMessage(0);
            break;
        }
        return 0;
    }

    // Restart Manager (installer CloseApplications) and system shutdown go
    // through here; replying TRUE then exiting on WM_ENDSESSION lets saveConfig run.
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (wp) {
            g_appRunning = false;
            removeTrayIcon();
            saveConfig(g_config);
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
