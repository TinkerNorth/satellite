// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * main.cpp -- WinMain entry point / Composition Root.
 *
 * Instantiates all adapters and the SessionService, then passes
 * references to the threads that need them. No business logic lives
 * here -- only platform lifecycle, dependency wiring, and the Win32
 * message loop.
 */
#include "globals.h"
#include "config.h"
#include "crypto.h"
#include "net/receiver.h"
#include "net/webserver.h"
#include "net/discovery.h"
#include "net/mdns_responder.h"
#include "tray.h"
#include "app_lifecycle.h"
#include "shell_integration.h"

// Adapters (outbound ports)
#include "vigem_adapter.h"
#include "updater_adapter.h"
#include "adapters/client_adapter.h"
#include "adapters/log_adapter.h"

// Domain services
#include "core/session_service.h"
#include "core/update_service.h"

#include <objbase.h>
#include <commctrl.h> // TaskDialogIndirect

#include <atomic>
#include <chrono>
#include <thread>

namespace {

// Show a modern Win10/11 styled error dialog instead of the legacy
// MessageBox slab. TaskDialogIndirect: proper icon, optional hyperlink
// support, expandable detail area, and visual style consistent with
// every other system dialog the user sees.
void showFatalError(const wchar_t* title, const wchar_t* heading, const wchar_t* details) {
    TASKDIALOGCONFIG cfg{};
    cfg.cbSize = sizeof(cfg);
    cfg.dwFlags = TDF_USE_HICON_MAIN | TDF_ALLOW_DIALOG_CANCELLATION;
    cfg.pszWindowTitle = title;
    cfg.pszMainInstruction = heading;
    cfg.pszContent = details;
    // IDI_ERROR is MAKEINTRESOURCE-wrapped; cast to LPCWSTR for the
    // wide LoadIconW (the A/W split keys off the resource name pointer).
    cfg.hMainIcon = LoadIconW(nullptr, reinterpret_cast<LPCWSTR>(IDI_ERROR));
    cfg.dwCommonButtons = TDCBF_CLOSE_BUTTON;
    TaskDialogIndirect(&cfg, nullptr, nullptr, nullptr);
}

// Background thread: refreshes the tray hover tooltip every ~2s so the
// user always sees up-to-date status without having to open the menu.
// Cheap because updateTrayTooltip skips the NIM_MODIFY round-trip if
// the composed string is unchanged.
void tooltipTickerThread() {
    using namespace std::chrono_literals;
    while (g_appRunning.load(std::memory_order_relaxed)) {
        updateTrayTooltip();
        std::this_thread::sleep_for(2s);
    }
}

} // namespace

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    // ── Pre-init hardening (must happen before any LoadLibrary / file I/O) ──
    lifecycle::hardenDllSearchPath();
    lifecycle::applyRuntimeMitigations();
    lifecycle::installCrashHandler();
    lifecycle::registerForRestart();

    // Refuse to start a second copy. Same-session double-launch is a
    // common footgun for tray apps (the user double-clicks the desktop
    // icon while we're already running and ports fight). We tap the
    // existing instance's window so it can flash a balloon, then exit.
    if (!lifecycle::acquireSingleInstance(APP_TITLE))
        return 0;

    // ── Modern shell identity ───────────────────────────────────────
    // AppUserModelID must be set BEFORE any HWND or tray icon, or the
    // taskbar / toast / jump-list grouping uses fallback heuristics
    // (typically "satellite.exe" rather than the published identity).
    // COM apartment is required by jump-list registration below.
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    shell_integration::registerAppUserModelID();

    // INITCOMMONCONTROLSEX is required for TaskDialogIndirect on
    // pre-Vista comctl32; harmless on Win10+. Done early so a failed
    // libsodium init can use the modern dialog rather than MessageBox.
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    // Elevate process priority -- critical for low-latency input forwarding.
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    // Force 1ms timer resolution (default is 15.6ms which affects scheduling).
    timeBeginPeriod(1);

    // Initialize Winsock globally (needed by httplib).
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    // Initialize libsodium.
    if (!sodiumInit()) {
        showFatalError(L"Satellite",
                       L"Cryptography library failed to initialize",
                       L"libsodium could not start. Satellite cannot run without it.\n\n"
                       L"Try reinstalling Satellite; if the problem persists, file an issue at "
                       L"https://github.com/TinkerNorth/satellite/issues.");
        return 1;
    }

    // Load config. The JSON file is the source of truth for the user's
    // autostart preference once they've ever toggled it; on first run
    // (no JSON yet) we seed from the registry instead, so the
    // installer's autostart task selection survives the first launch.
    bool firstRun;
    {
        std::ifstream probe(configPath());
        firstRun = !probe.is_open();
    }
    g_config = loadConfig();
    if (firstRun) g_config.autoStart = getAutoStart();

    // Reconcile HKCU\Run with the loaded preference. Strictly
    // constructive: writes when autostart is enabled, never deletes.
    // (Deletion only happens via explicit setAutoStart(false) from the
    // UI toggle, so a side-loaded portable build can't accidentally
    // wipe out an installed copy's Run entry.)
    lifecycle::reconcileAutoStart();

    // ── Persistent file logger ──────────────────────────────────────
    // Mirror every logMsg() to a rolling file under %LOCALAPPDATA%.
    // Without this, the in-memory ring is the only diagnostic trail,
    // which evaporates the moment the user reports "it crashed
    // yesterday at 3pm". Started after config load so the configured
    // log dir override (future) can take effect.
    lifecycle::startFileLogger();

    // ── Composition Root: wire adapters -> service ──────────────────
    ViGEmAdapter vigemAdapter;
    ClientAdapter clientAdapter;
    LogAdapter logAdapter;
    SessionService svc(vigemAdapter, clientAdapter, logAdapter);

    // OTA updater. Owner/repo are baked in here -- switching forks
    // means changing this line. The persist callback runs under
    // g_configMtx so saveConfig() sees a consistent struct.
    WindowsUpdaterAdapter updaterAdapter("TinkerNorth", "satellite");
    UpdateService updateService(updaterAdapter, logAdapter, g_config, g_configMtx);
    updateService.setPersistCallback([] {
        std::lock_guard<std::mutex> lk(g_configMtx);
        saveConfig(g_config);
    });
    // Fire a Win10/11 toast (auto-routed to Action Center via our AUMID)
    // when an update first becomes available. Edge-triggered: we only
    // show the toast on the Idle/Checking -> UpdateAvailable transition,
    // not on every SSE broadcast, otherwise the user would get spammed
    // while the settings page is open.
    {
        std::atomic<bool> toastedAvailable{false};
        // Note: the callback is invoked from the worker thread without
        // the service's mutex held, so calling Shell_NotifyIcon is safe.
        updateService.setStatusCallback(
            [&toastedAvailable](const UpdateStatusSnapshot& snap) {
                if (snap.state == UpdateState::UpdateAvailable && snap.info.available) {
                    if (!toastedAvailable.exchange(true)) {
                        shell_integration::showToast(
                            std::string("Satellite update ready"),
                            std::string("Version ") + snap.info.version +
                                " is available. Click to install.");
                    }
                } else if (snap.state == UpdateState::Idle ||
                           snap.state == UpdateState::UpToDate) {
                    // Transitioned back to a non-actionable state --
                    // arm the latch so a later UpdateAvailable triggers
                    // another toast.
                    toastedAvailable.store(false);
                }
                // Update the hover tooltip if state transition changed
                // what we'd display.
                updateTrayTooltip();
            });
    }
    g_updateService = &updateService;

    // Register hidden window class. Wide form to match the new tray
    // surface; APP_TITLE remains the literal Explorer uses to find
    // an existing instance.
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"ControllerForwardTray";
    RegisterClassW(&wc);

    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Satellite", 0, 0, 0, 0, 0,
                             HWND_MESSAGE, nullptr, hInst, nullptr);

    addTrayIcon(g_hwnd);

    // Register the taskbar jump list. Needs to happen AFTER the AUMID
    // is set + COM is initialised. CommitList is idempotent and cheap.
    shell_integration::refreshJumpList();

    // Resolve web/ directory relative to the exe.
    g_webDir = getExeDir() + "\\web";

    // Start the updater (spawns worker + timer threads internally).
    updateService.start();

    // Launch worker threads (pass service & adapters by reference).
    std::thread recvTh(receiverThread, std::ref(svc), std::ref(clientAdapter));
    std::thread adminTh(adminHttpThread, std::ref(svc));
    std::thread clientTh(clientApiThread, std::ref(svc));
    std::thread discTh(discoveryThread);
    std::thread mdnsTh(mdnsResponderThread);
    std::thread tooltipTh(tooltipTickerThread);

    // Win32 message loop.
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // Shutdown.
    g_appRunning = false;
    g_httpServer.stop();
    if (g_clientServer) g_clientServer->stop();

    // Stop the updater worker BEFORE joining the http thread so its
    // SSE-broadcast callback doesn't fire into a torn-down server.
    updateService.stop();
    g_updateService = nullptr;

    recvTh.join();
    adminTh.join();
    clientTh.join();
    discTh.join();
    mdnsTh.join();
    tooltipTh.join();

    // Clean up all remaining sessions before exit.
    svc.closeAllSessions();

    removeTrayIcon();
    saveConfig(g_config);
    timeEndPeriod(1);
    WSACleanup();
    CoUninitialize();
    return 0;
}
