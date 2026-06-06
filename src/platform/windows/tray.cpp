// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * tray.cpp -- System tray icon, menu, WndProc
 *
 * Modern Win10/11 tray surface. Three things make this "first-class":
 *   * NOTIFYICONDATAW + NOTIFYICON_VERSION_4 -- the only protocol
 *     Explorer actively maintains. v3 still works but loses out on
 *     correct mouse coords and balloon-callback fidelity.
 *   * NIF_GUID identity -- Explorer keys tray-hide / promotion choices
 *     on this GUID, so they survive reinstalls and exe-path changes.
 *     Without it, every update re-prompts the user to allow the icon.
 *   * Dynamic tooltip via NIM_MODIFY -- the hover string reflects live
 *     state (listening / idle / session count) rather than just the
 *     app name. tray::updateTooltip() pushes the latest snapshot.
 *
 * The class also owns:
 *   * WM_TASKBARCREATED re-registration so Explorer crashes / RDP
 *     reconnects don't leave us with an invisible icon.
 *   * Restart Manager + WM_QUERYENDSESSION clean shutdown so installer
 *     OTA upgrades let saveConfig run.
 *   * Single-instance ping handling (WM_SECOND_INSTANCE) so a stray
 *     double-launch from the desktop just re-opens the web UI.
 */
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

// index → deviceId backing the current tray-menu pairing items. Touched only on
// the GUI thread (menu build + WM_COMMAND), so it needs no lock.
static std::vector<std::string> g_menuPairIds;

// Requests arrive on the HTTP thread, but the WinRT toast (+ COM) must run on the
// COM-initialised GUI thread. The listener queues the deviceId here and posts
// WM_PAIR_NOTIFY for the message loop to drain.
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

// Compose the live status string used as the tray tooltip. Kept short
// because szTip is capped at 128 wchars and most shells truncate around
// 64 for hover layout reasons.
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

    // First attempt: with NIF_GUID. Common failure mode: the GUID is
    // already registered to a different (stale) exe path -- Explorer
    // refuses to remap silently. Recover by deleting the stale
    // registration and trying again. If even that fails (corrupted
    // tray cache, Win7 fallback), drop NIF_GUID entirely and live
    // without the cross-install identity.
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

    // NIM_SETVERSION enables modern behaviour (correct mouse coords in
    // WM_TRAYICON LPARAM, balloon-callback notifications via NIN_*).
    // Must come after NIM_ADD. The message format becomes:
    //   LOWORD(lp) = event (WM_LBUTTONDBLCLK, WM_CONTEXTMENU, NIN_*)
    //   HIWORD(lp) = icon uID
    //   WPARAM    = packed cursor x/y
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

    // Keep the cached struct in sync for future NIM_MODIFY calls.
    StringCchCopyW(g_nid.szTip, ARRAYSIZE(g_nid.szTip), g_lastTooltip.c_str());
}

void showTrayMenu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();

    // Pending reverse-pairing requests sit at the top — clicking one reopens its
    // Accept/Reject dialog. Rebuilt every open from the live registry; the
    // id→deviceId map is snapshotted so WM_COMMAND can resolve the click.
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
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // Updater entry -- rebuilt each open so the label reflects current
    // state ("Install Update v1.2.3" vs "Check for Updates..."). When
    // the updater isn't wired (g_updateService==nullptr), fall back to
    // a disabled "Check for Updates..." item so the menu shape stays
    // stable.
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

    // SetForegroundWindow is required or the menu can fail to dismiss
    // when the user clicks outside it -- a classic Win32 footgun.
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

// Second-instance ping from app_lifecycle::acquireSingleInstance.
// We use it as the "user tried to launch a second copy" cue and open
// the web UI for them -- their probable intent.
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

// ── Reverse-pairing native prompt ───────────────────────────────────────────
// A dish that shows its own PIN raises a request; we toast the operator, and
// the toast click opens a native Accept/Reject dialog — no browser. deviceIds
// awaiting that click are queued here (the toast carries no payload of its own).
static std::mutex g_pairQueueMtx;
static std::deque<std::string> g_pendingPrompts;

// Native accept/reject dialog for one request. Shows the dish's PIN so the
// operator can confirm it matches the device (the auth is that visual match —
// see net/pairing.h), then confirm/decline through pairing_service.
static void showPairingDialogWindows(HWND hwnd, const std::string& deviceId) {
    std::string name, ip, pin;
    int secs = 0;
    // Re-snapshot at click time: the request may have expired or been handled
    // from the dashboard between the toast and the click.
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

// pairing.cpp listener (registered from main). Fires on the HTTP thread, so it
// only touches the thread-safe queue + Shell_NotifyIcon (toast) here; the modal
// dialog waits for the GUI thread's balloon-click handler.
void notifyPairRequestWindows(const std::string& deviceId) {
    // Fires on the HTTP thread; the WinRT toast must be raised on the
    // COM-initialised GUI thread, so hand off and wake the message loop.
    {
        std::lock_guard<std::mutex> lk(g_incomingMtx);
        g_incoming.push_back(deviceId);
    }
    if (g_hwnd != nullptr) PostMessageW(g_hwnd, WM_PAIR_NOTIFY, 0, 0);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // Explorer (re)created its taskbar. Re-add our icon so the user
    // doesn't end up with a running-but-invisible tray app.
    if (g_taskbarCreatedMsg != 0 && msg == g_taskbarCreatedMsg) {
        registerTrayIcon(hwnd);
        return 0;
    }

    switch (msg) {
    case WM_TRAYICON: {
        // NOTIFYICON_VERSION_4 packs the mouse/keyboard event into
        // LOWORD(lp); HIWORD(lp) is the icon uID. WPARAM carries
        // the cursor's screen coordinates. The legacy v3 protocol put
        // the event directly into lp -- comparing the whole lp against
        // WM_LBUTTONDBLCLK never matches under v4.
        UINT event = LOWORD(lp);
        if (event == WM_LBUTTONDBLCLK || event == NIN_KEYSELECT) {
            openUrl(webUiUrl().c_str());
        } else if (event == WM_CONTEXTMENU || event == WM_RBUTTONUP) {
            showTrayMenu(hwnd);
        } else if (event == NIN_BALLOONUSERCLICK) {
            // A queued pairing prompt means the toast the user just clicked was
            // a pairing request — open the native Accept/Reject dialog(s).
            std::deque<std::string> todo;
            {
                std::lock_guard<std::mutex> lk(g_pairQueueMtx);
                todo.swap(g_pendingPrompts);
            }
            if (!todo.empty()) {
                for (const auto& id : todo) showPairingDialogWindows(hwnd, id);
                return 0;
            }
            // Otherwise: open whichever surface is most useful. Update staged →
            // settings page; else the dashboard.
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
        // A protocol-activated toast button (satellite-pair:accept|reject/<id>)
        // launched a second process, which forwarded the URI here.
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
            // Prefer the actionable toast; fall back to a balloon whose click
            // opens the Accept/Reject dialog (the deviceId is then queued for
            // NIN_BALLOONUSERCLICK above).
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
        // Dynamic pairing-review items resolve through the snapshot vector.
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
        case IDM_OPEN_LOGS: {
            std::string dir = lifecycle::logDir();
            if (!dir.empty()) {
                std::wstring wd = toWide(dir);
                ShellExecuteW(nullptr, L"open", wd.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            break;
        }
        case IDM_REPORT_PROBLEM: {
            // Open the GitHub issue tracker with a body that pre-fills
            // the version + log/dump paths the user should attach.
            // URL-encode the body so newlines and brackets survive.
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

    // Restart Manager (the installer's CloseApplications path) sends
    // WM_QUERYENDSESSION with ENDSESSION_CLOSEAPP set. Returning TRUE
    // and then exiting on WM_ENDSESSION lets the installer (or a
    // system shutdown) close us cleanly so saveConfig actually runs.
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
