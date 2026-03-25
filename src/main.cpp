/*
 * main.cpp — WinMain entry point, thread launching
 */
#include "globals.h"
#include "config.h"
#include "receiver.h"
#include "webserver.h"
#include "pairing.h"
#include "discovery.h"
#include "tray.h"

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    // Elevate process priority — critical for low-latency input forwarding
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    // Force 1ms timer resolution (default is 15.6ms which affects scheduling)
    timeBeginPeriod(1);

    // Initialize Winsock globally (needed by httplib)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    // Load config
    g_config = loadConfig();
    g_config.autoStart = getAutoStart();

    // Auto-start listener if configured
    if (g_config.autoStart) g_wantListen = true;

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

    // Launch worker threads
    std::thread recvTh(receiverThread);
    std::thread httpTh(httpThread);
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

    removeTrayIcon();
    saveConfig(g_config);
    timeEndPeriod(1);
    WSACleanup();
    return 0;
}
