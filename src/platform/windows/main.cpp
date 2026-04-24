/*
 * main.cpp — WinMain entry point / Composition Root.
 *
 * Instantiates all adapters and the SessionService, then passes references
 * to the threads that need them.  No business logic lives here.
 */
#include "globals.h"
#include "config.h"
#include "crypto.h"
#include "net/receiver.h"
#include "net/webserver.h"
#include "net/pairing.h"
#include "net/discovery.h"
#include "tray.h"

// Adapters (outbound ports)
#include "vigem_adapter.h"
#include "adapters/client_adapter.h"
#include "adapters/log_adapter.h"

// Domain service
#include "core/session_service.h"

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    // Elevate process priority — critical for low-latency input forwarding
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    // Force 1ms timer resolution (default is 15.6ms which affects scheduling)
    timeBeginPeriod(1);

    // Initialize Winsock globally (needed by httplib)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    // Initialize libsodium
    if (!sodiumInit()) {
        MessageBoxA(nullptr, "Failed to initialize libsodium", APP_TITLE, MB_ICONERROR);
        return 1;
    }

    // Load config
    g_config = loadConfig();
    g_config.autoStart = getAutoStart();

    // Auto-start listener if configured
    if (g_config.autoStart) g_wantListen = true;

    // ── Composition Root: wire adapters → service ────────────────────
    ViGEmAdapter vigemAdapter;
    ClientAdapter clientAdapter;
    LogAdapter logAdapter;
    SessionService svc(vigemAdapter, clientAdapter, logAdapter);

    // Register hidden window class
    WNDCLASSA wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = "ControllerForwardTray";
    RegisterClassA(&wc);

    g_hwnd = CreateWindowExA(0, wc.lpszClassName, APP_TITLE, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                             hInst, nullptr);

    addTrayIcon(g_hwnd);

    // Resolve web/ directory relative to the exe
    g_webDir = getExeDir() + "\\web";

    // Launch worker threads (pass service & adapters by reference)
    std::thread recvTh(receiverThread, std::ref(svc), std::ref(clientAdapter));
    std::thread httpTh(httpThread, std::ref(svc));
    std::thread pairTh(pairingThread);
    std::thread discTh(discoveryThread);

    // Win32 message loop
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Shutdown
    g_appRunning = false;
    g_wantListen = false;
    g_httpServer.stop();
    if (g_pairSock != INVALID_SOCKET) closesocket(g_pairSock);

    recvTh.join();
    httpTh.join();
    pairTh.join();
    discTh.join();

    // Clean up all remaining sessions before exit
    svc.closeAllSessions();

    removeTrayIcon();
    saveConfig(g_config);
    timeEndPeriod(1);
    WSACleanup();
    return 0;
}
