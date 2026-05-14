// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * tray.cpp — System tray icon, menu, WndProc
 */
#include "tray.h"
#include "config.h"
#include "resource.h"
#include "core/update_service.h"

static NOTIFYICONDATAA g_nid{};

void addTrayIcon(HWND hwnd) {
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    // Load icon from embedded resource, fall back to default
    HICON hCustom = LoadIcon(GetModuleHandle(nullptr), MAKEINTRESOURCE(IDI_APP_ICON));
    g_nid.hIcon = (hCustom != nullptr) ? hCustom : LoadIcon(nullptr, IDI_APPLICATION);
    strncpy(g_nid.szTip, APP_TITLE, sizeof(g_nid.szTip) - 1);
    Shell_NotifyIconA(NIM_ADD, &g_nid);
}

void removeTrayIcon() { Shell_NotifyIconA(NIM_DELETE, &g_nid); }

void showTrayMenu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuA(menu, MF_STRING, IDM_OPEN_UI, "Open Web UI");
    AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(menu, MF_STRING, IDM_TOGGLE,
                g_listening.load() ? "Stop Listener" : "Start Listener");
    AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);

    // Updater entry — rebuilt each open so the label reflects current
    // state ("Install Update v1.2.3" vs "Check for Updates…"). When the
    // updater isn't wired (g_updateService==nullptr), fall back to a
    // disabled "Check for Updates…" item so the menu shape stays stable.
    if (g_updateService) {
        UpdateStatusSnapshot snap = g_updateService->snapshot();
        if (snap.state == UpdateState::Downloaded && snap.info.available) {
            std::string label = "Install Update v" + snap.info.version;
            AppendMenuA(menu, MF_STRING, IDM_INSTALL_UPDATE, label.c_str());
        } else if (snap.state == UpdateState::UpdateAvailable && snap.info.available) {
            std::string label = "Download Update v" + snap.info.version + "..."; // ellipsis
            AppendMenuA(menu, MF_STRING, IDM_INSTALL_UPDATE, label.c_str());
        } else if (snap.state == UpdateState::Downloading || snap.state == UpdateState::Verifying) {
            AppendMenuA(menu, MF_STRING | MF_GRAYED, IDM_CHECK_UPDATES, "Downloading update...");
        } else if (snap.state == UpdateState::Checking) {
            AppendMenuA(menu, MF_STRING | MF_GRAYED, IDM_CHECK_UPDATES, "Checking for updates...");
        } else {
            AppendMenuA(menu, MF_STRING, IDM_CHECK_UPDATES, "Check for Updates...");
        }
    } else {
        AppendMenuA(menu, MF_STRING | MF_GRAYED, IDM_CHECK_UPDATES, "Check for Updates...");
    }

    AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(menu, MF_STRING, IDM_EXIT, "Exit");
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TRAYICON:
        if (lp == WM_LBUTTONDBLCLK) {
            char url[64];
            snprintf(url, sizeof(url), "http://localhost:%d", g_config.webPort);
            ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL);
        } else if (lp == WM_RBUTTONUP) {
            showTrayMenu(hwnd);
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        default:
            break;
        case IDM_OPEN_UI: {
            char url[64];
            snprintf(url, sizeof(url), "http://localhost:%d", g_config.webPort);
            ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL);
            break;
        }
        case IDM_TOGGLE:
            if (g_listening.load()) {
                g_wantListen = false;
            } else {
                g_wantListen = true;
            }
            break;
        case IDM_CHECK_UPDATES:
            if (g_updateService) g_updateService->requestCheck(/*userInitiated=*/true);
            // Open the web UI so the user sees the result — same affordance
            // a Mac/Linux user gets from the in-app banner.
            {
                char url[80];
                snprintf(url, sizeof(url), "http://localhost:%d/settings", g_config.webPort);
                ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL);
            }
            break;
        case IDM_INSTALL_UPDATE:
            if (g_updateService) {
                UpdateStatusSnapshot s = g_updateService->snapshot();
                if (s.state == UpdateState::Downloaded) {
                    g_updateService->requestInstall();
                } else {
                    g_updateService->requestDownload();
                }
                char url[80];
                snprintf(url, sizeof(url), "http://localhost:%d/settings", g_config.webPort);
                ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL);
            }
            break;
        case IDM_EXIT:
            PostQuitMessage(0);
            break;
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}
