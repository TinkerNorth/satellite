/*
 * Controller Receiver — service/tray edition
 *
 * Runs as a system-tray application with:
 *   - UDP receiver thread: captures XUSB_REPORT packets, injects via ViGEmBus
 *   - HTTP server thread:  serves a local web UI for configuration
 *   - Main thread:         Win32 message loop + tray icon
 *
 * Build:  g++ -O2 -std=c++17 -DCPPHTTPLIB_NO_EXCEPTIONS
 *              -o controller-receiver.exe controller-receiver.cpp
 *              -Ivigem/include -Ilib
 *              -lsetupapi -lws2_32 -lshell32 -lole32 -mwindows
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <fstream>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <setupapi.h>
#include <shlobj.h>

#include "ViGEm/BusShared.h"
#include "httplib.h"

// ── Forward declarations ────────────────────────────────────────────────────
static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

// ── Constants ───────────────────────────────────────────────────────────────
static const char* APP_NAME        = "controller-forward";
static const char* APP_TITLE       = "Controller Forward";
static const int   DEFAULT_UDP_PORT = 9876;
static const int   DEFAULT_WEB_PORT = 9877;
static const UINT  WM_TRAYICON     = WM_APP + 1;
static const UINT  IDM_OPEN_UI     = 1001;
static const UINT  IDM_TOGGLE      = 1002;
static const UINT  IDM_EXIT        = 1003;

// ── Config ──────────────────────────────────────────────────────────────────
struct Config {
    int  udpPort   = DEFAULT_UDP_PORT;
    int  webPort   = DEFAULT_WEB_PORT;
    bool autoStart = false;   // start listening on launch
};

static std::string configPath() {
    char buf[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, buf))) {
        std::string dir = std::string(buf) + "\\controller-forward";
        CreateDirectoryA(dir.c_str(), nullptr);
        return dir + "\\config.json";
    }
    return "config.json";
}

// Minimal JSON read/write — no external dependency
static Config loadConfig() {
    Config cfg;
    std::ifstream f(configPath());
    if (!f.is_open()) return cfg;
    std::string line;
    while (std::getline(f, line)) {
        if (line.find("\"udpPort\"") != std::string::npos) {
            auto p = line.find(':');
            if (p != std::string::npos) cfg.udpPort = atoi(line.c_str() + p + 1);
        } else if (line.find("\"webPort\"") != std::string::npos) {
            auto p = line.find(':');
            if (p != std::string::npos) cfg.webPort = atoi(line.c_str() + p + 1);
        } else if (line.find("\"autoStart\"") != std::string::npos) {
            cfg.autoStart = line.find("true") != std::string::npos;
        }
    }
    return cfg;
}

static void saveConfig(const Config& cfg) {
    std::ofstream f(configPath());
    f << "{\n"
      << "  \"udpPort\": "   << cfg.udpPort   << ",\n"
      << "  \"webPort\": "   << cfg.webPort   << ",\n"
      << "  \"autoStart\": " << (cfg.autoStart ? "true" : "false") << "\n"
      << "}\n";
}

// ── Shared state ────────────────────────────────────────────────────────────
static Config              g_config;
static std::atomic<bool>   g_appRunning{true};     // overall app lifecycle
static std::atomic<bool>   g_listening{false};      // UDP receiver active?
static std::atomic<bool>   g_wantListen{false};     // request to start/stop
static std::atomic<uint64_t> g_packetCount{0};
static std::mutex          g_senderMtx;
static std::string         g_senderIP = "none";
static HWND                g_hwnd = nullptr;

// ── ViGEmBus helpers (unchanged) ────────────────────────────────────────────

static HANDLE openVigemBus() {
    SP_DEVICE_INTERFACE_DATA did{};
    did.cbSize = sizeof(did);
    DWORD idx = 0, reqSize = 0;
    HDEVINFO devInfo = SetupDiGetClassDevsW(
        &GUID_DEVINTERFACE_BUSENUM_VIGEM, nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

    while (SetupDiEnumDeviceInterfaces(devInfo, nullptr,
            &GUID_DEVINTERFACE_BUSENUM_VIGEM, idx++, &did)) {
        SetupDiGetDeviceInterfaceDetailW(devInfo, &did, nullptr, 0, &reqSize, nullptr);
        auto* detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)malloc(reqSize);
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (SetupDiGetDeviceInterfaceDetailW(devInfo, &did, detail, reqSize, nullptr, nullptr)) {
            HANDLE h = CreateFileW(detail->DevicePath,
                GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
            free(detail);
            if (h != INVALID_HANDLE_VALUE) {
                OVERLAPPED ov{};
                ov.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                VIGEM_CHECK_VERSION ver;
                VIGEM_CHECK_VERSION_INIT(&ver, VIGEM_COMMON_VERSION);
                DWORD xfr = 0;
                DeviceIoControl(h, IOCTL_VIGEM_CHECK_VERSION, &ver, ver.Size,
                                nullptr, 0, &xfr, &ov);
                if (GetOverlappedResult(h, &ov, &xfr, TRUE)) {
                    CloseHandle(ov.hEvent);
                    SetupDiDestroyDeviceInfoList(devInfo);
                    return h;
                }
                CloseHandle(ov.hEvent);
                CloseHandle(h);
            }
        } else {
            free(detail);
        }
    }
    SetupDiDestroyDeviceInfoList(devInfo);
    return INVALID_HANDLE_VALUE;
}

static bool pluginTarget(HANDLE bus, ULONG serial) {
    OVERLAPPED ov{}; ov.hEvent = CreateEvent(nullptr,FALSE,FALSE,nullptr);
    DWORD xfr = 0;
    VIGEM_PLUGIN_TARGET plug;
    VIGEM_PLUGIN_TARGET_INIT(&plug, serial, Xbox360Wired);
    plug.VendorId  = 0x045E;
    plug.ProductId = 0x028E;
    DeviceIoControl(bus, IOCTL_VIGEM_PLUGIN_TARGET, &plug, plug.Size,
                    nullptr, 0, &xfr, &ov);
    bool ok = GetOverlappedResult(bus, &ov, &xfr, TRUE) != 0;
    if (ok) {
        VIGEM_WAIT_DEVICE_READY wr;
        VIGEM_WAIT_DEVICE_READY_INIT(&wr, serial);
        OVERLAPPED ov2{}; ov2.hEvent = CreateEvent(nullptr,FALSE,FALSE,nullptr);
        DeviceIoControl(bus, IOCTL_VIGEM_WAIT_DEVICE_READY, &wr, wr.Size,
                        nullptr, 0, &xfr, &ov2);
        GetOverlappedResult(bus, &ov2, &xfr, TRUE);
        CloseHandle(ov2.hEvent);
    }
    CloseHandle(ov.hEvent);
    return ok;
}

static bool submitReport(HANDLE bus, ULONG serial, const XUSB_REPORT& rpt) {
    OVERLAPPED ov{}; ov.hEvent = CreateEvent(nullptr,FALSE,FALSE,nullptr);
    DWORD xfr = 0;
    XUSB_SUBMIT_REPORT sr;
    XUSB_SUBMIT_REPORT_INIT(&sr, serial);
    sr.Report = rpt;
    DeviceIoControl(bus, IOCTL_XUSB_SUBMIT_REPORT, &sr, sr.Size,
                    nullptr, 0, &xfr, &ov);
    bool ok = GetOverlappedResult(bus, &ov, &xfr, TRUE) != 0;
    CloseHandle(ov.hEvent);
    return ok;
}

static void unplugTarget(HANDLE bus, ULONG serial) {
    OVERLAPPED ov{}; ov.hEvent = CreateEvent(nullptr,FALSE,FALSE,nullptr);
    DWORD xfr = 0;
    VIGEM_UNPLUG_TARGET up;
    VIGEM_UNPLUG_TARGET_INIT(&up, serial);
    DeviceIoControl(bus, IOCTL_VIGEM_UNPLUG_TARGET, &up, up.Size,
                    nullptr, 0, &xfr, &ov);
    GetOverlappedResult(bus, &ov, &xfr, TRUE);
    CloseHandle(ov.hEvent);
}

// ── Auto-start (registry) ───────────────────────────────────────────────────

static void setAutoStart(bool enable) {
    HKEY key;
    const char* run = "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run";
    if (RegOpenKeyExA(HKEY_CURRENT_USER, run, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return;
    if (enable) {
        char exePath[MAX_PATH];
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        RegSetValueExA(key, APP_NAME, 0, REG_SZ, (BYTE*)exePath, (DWORD)strlen(exePath) + 1);
    } else {
        RegDeleteValueA(key, APP_NAME);
    }
    RegCloseKey(key);
}

static bool getAutoStart() {
    HKEY key;
    const char* run = "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run";
    if (RegOpenKeyExA(HKEY_CURRENT_USER, run, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;
    DWORD type, size = 0;
    bool exists = RegQueryValueExA(key, APP_NAME, nullptr, &type, nullptr, &size) == ERROR_SUCCESS;
    RegCloseKey(key);
    return exists;
}

// ── UDP Receiver Thread ─────────────────────────────────────────────────────

static void receiverThread() {
    while (g_appRunning) {
        // Wait until we're told to start listening
        while (g_appRunning && !g_wantListen) { Sleep(50); }
        if (!g_appRunning) break;

        int port = g_config.udpPort;

        // ── Open ViGEmBus ──
        HANDLE busDevice = openVigemBus();
        if (busDevice == INVALID_HANDLE_VALUE) {
            g_wantListen = false;
            continue;
        }

        // ── Plug in virtual controller ──
        ULONG serialNo = 0;
        for (ULONG s = 1; s <= 16; ++s) {
            if (pluginTarget(busDevice, s)) { serialNo = s; break; }
        }
        if (serialNo == 0) {
            CloseHandle(busDevice);
            g_wantListen = false;
            continue;
        }

        // ── Winsock ──
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
            unplugTarget(busDevice, serialNo);
            CloseHandle(busDevice);
            g_wantListen = false;
            continue;
        }

        SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == INVALID_SOCKET) {
            unplugTarget(busDevice, serialNo);
            CloseHandle(busDevice);
            WSACleanup();
            g_wantListen = false;
            continue;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons((u_short)port);
        addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
            closesocket(sock);
            unplugTarget(busDevice, serialNo);
            CloseHandle(busDevice);
            WSACleanup();
            g_wantListen = false;
            continue;
        }

        u_long nonBlock = 1;
        ioctlsocket(sock, FIONBIO, &nonBlock);

        g_listening = true;
        g_packetCount = 0;
        { std::lock_guard<std::mutex> lk(g_senderMtx); g_senderIP = "none"; }

        XUSB_REPORT report;
        XUSB_REPORT_INIT(&report);

        // ── Hot loop ──
        while (g_appRunning && g_wantListen) {
            sockaddr_in sender{};
            int slen = sizeof(sender);
            char buf[64];
            int n = recvfrom(sock, buf, sizeof(buf), 0, (sockaddr*)&sender, &slen);

            if (n == sizeof(XUSB_REPORT)) {
                memcpy(&report, buf, sizeof(XUSB_REPORT));
                submitReport(busDevice, serialNo, report);
                g_packetCount++;

                if ((g_packetCount & 0xFF) == 0) {
                    char ip[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &sender.sin_addr, ip, sizeof(ip));
                    std::lock_guard<std::mutex> lk(g_senderMtx);
                    g_senderIP = ip;
                }
            } else if (n == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
                // real error — keep going
            } else if (n == SOCKET_ERROR) {
                Sleep(1);
            }
        }

        // ── Tear down ──
        g_listening = false;
        closesocket(sock);
        unplugTarget(busDevice, serialNo);
        CloseHandle(busDevice);
        WSACleanup();
    }
}

// ── Web UI directory (resolved relative to .exe location) ───────────────────

static std::string getExeDir() {
    char buf[MAX_PATH];
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string path(buf);
    auto pos = path.find_last_of("\\/");
    return (pos != std::string::npos) ? path.substr(0, pos) : ".";
}

static std::string g_webDir;

// ── HTTP Server Thread ──────────────────────────────────────────────────────

static httplib::Server g_httpServer;

static void httpThread() {
    // Serve static files (index.html, style.css, app.js) from the web/ folder
    g_httpServer.set_mount_point("/", g_webDir);

    g_httpServer.Get("/api/status", [](const httplib::Request&, httplib::Response& res) {
        std::string senderIP;
        { std::lock_guard<std::mutex> lk(g_senderMtx); senderIP = g_senderIP; }
        char json[512];
        snprintf(json, sizeof(json),
            R"({"listening":%s,"packets":%llu,"senderIP":"%s","udpPort":%d,"webPort":%d,"autoStart":%s})",
            g_listening.load() ? "true" : "false",
            (unsigned long long)g_packetCount.load(),
            senderIP.c_str(),
            g_config.udpPort,
            g_config.webPort,
            g_config.autoStart ? "true" : "false");
        res.set_content(json, "application/json");
    });

    g_httpServer.Post("/api/start", [](const httplib::Request&, httplib::Response& res) {
        g_wantListen = true;
        res.set_content(R"({"ok":true})", "application/json");
    });

    g_httpServer.Post("/api/stop", [](const httplib::Request&, httplib::Response& res) {
        g_wantListen = false;
        res.set_content(R"({"ok":true})", "application/json");
    });

    g_httpServer.Post("/api/config", [](const httplib::Request& req, httplib::Response& res) {
        // Minimal JSON parsing for {udpPort:N, autoStart:bool}
        auto body = req.body;
        auto pPort = body.find("\"udpPort\"");
        if (pPort != std::string::npos) {
            auto colon = body.find(':', pPort);
            if (colon != std::string::npos) {
                int port = atoi(body.c_str() + colon + 1);
                if (port >= 1024 && port <= 65535) g_config.udpPort = port;
            }
        }
        g_config.autoStart = body.find("\"autoStart\":true") != std::string::npos
                          || body.find("\"autoStart\": true") != std::string::npos;
        setAutoStart(g_config.autoStart);
        saveConfig(g_config);
        res.set_content(R"({"ok":true})", "application/json");
    });

    g_httpServer.listen("127.0.0.1", g_config.webPort);
}

// ── System Tray ─────────────────────────────────────────────────────────────

static NOTIFYICONDATAA g_nid{};

static void addTrayIcon(HWND hwnd) {
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd   = hwnd;
    g_nid.uID    = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon  = LoadIcon(nullptr, IDI_APPLICATION);
    strncpy(g_nid.szTip, APP_TITLE, sizeof(g_nid.szTip) - 1);
    Shell_NotifyIconA(NIM_ADD, &g_nid);
}

static void removeTrayIcon() {
    Shell_NotifyIconA(NIM_DELETE, &g_nid);
}

static void showTrayMenu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuA(menu, MF_STRING, IDM_OPEN_UI, "Open Web UI");
    AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(menu, MF_STRING, IDM_TOGGLE,
                g_listening.load() ? "Stop Listener" : "Start Listener");
    AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(menu, MF_STRING, IDM_EXIT, "Exit");
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

// ── WndProc ─────────────────────────────────────────────────────────────────

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TRAYICON:
        if (lp == WM_RBUTTONUP || lp == WM_LBUTTONUP) {
            showTrayMenu(hwnd);
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_OPEN_UI: {
            char url[64];
            snprintf(url, sizeof(url), "http://localhost:%d", g_config.webPort);
            ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL);
            break;
        }
        case IDM_TOGGLE:
            if (g_listening.load()) g_wantListen = false;
            else                    g_wantListen = true;
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

// ── WinMain ─────────────────────────────────────────────────────────────────

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    // Initialize Winsock globally (needed by httplib)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);

    // Load config
    g_config = loadConfig();
    g_config.autoStart = getAutoStart();

    // Auto-start listener if configured
    if (g_config.autoStart) g_wantListen = true;

    // Register hidden window class
    WNDCLASSA wc{};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance      = hInst;
    wc.lpszClassName  = "ControllerForwardTray";
    RegisterClassA(&wc);

    g_hwnd = CreateWindowExA(0, wc.lpszClassName, APP_TITLE,
        0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInst, nullptr);

    addTrayIcon(g_hwnd);

    // Resolve web/ directory relative to the exe
    g_webDir = getExeDir() + "\\web";

    // Launch worker threads
    std::thread recvTh(receiverThread);
    std::thread httpTh(httpThread);

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

    recvTh.join();
    httpTh.join();

    removeTrayIcon();
    saveConfig(g_config);
    return 0;
}