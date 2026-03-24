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

// ── Embedded Web UI (HTML) ───────────────────────────────────────────────────

static const char* WEB_UI_HTML = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Controller Forward</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;
  background:#0f0f0f;color:#e0e0e0;display:flex;justify-content:center;
  align-items:center;min-height:100vh}
.card{background:#1a1a2e;border-radius:16px;padding:32px;width:420px;
  box-shadow:0 8px 32px rgba(0,0,0,.5)}
h1{font-size:20px;margin-bottom:24px;color:#fff;display:flex;align-items:center;gap:10px}
h1 .dot{width:10px;height:10px;border-radius:50%;flex-shrink:0}
.dot.on{background:#00e676;box-shadow:0 0 8px #00e676}
.dot.off{background:#ff5252;box-shadow:0 0 8px #ff5252}
.stat{display:flex;justify-content:space-between;padding:10px 0;
  border-bottom:1px solid #2a2a3e;font-size:14px}
.stat .label{color:#888}.stat .value{color:#fff;font-family:'Courier New',monospace}
.controls{margin-top:24px;display:flex;flex-direction:column;gap:12px}
.row{display:flex;gap:12px;align-items:center}
.row label{font-size:13px;color:#888;min-width:80px}
.row input[type=number]{flex:1;background:#0f0f1a;border:1px solid #333;
  border-radius:8px;padding:8px 12px;color:#fff;font-size:14px}
.row input[type=number]:focus{outline:none;border-color:#7c4dff}
.toggle-row{display:flex;align-items:center;justify-content:space-between;
  padding:8px 0}
.toggle-row span{font-size:13px;color:#888}
.switch{position:relative;width:44px;height:24px}
.switch input{opacity:0;width:0;height:0}
.slider{position:absolute;inset:0;background:#333;border-radius:24px;
  cursor:pointer;transition:.2s}
.slider:before{content:'';position:absolute;width:18px;height:18px;left:3px;
  bottom:3px;background:#888;border-radius:50%;transition:.2s}
.switch input:checked+.slider{background:#7c4dff}
.switch input:checked+.slider:before{transform:translateX(20px);background:#fff}
btn,.btn{display:inline-flex;align-items:center;justify-content:center;
  padding:10px 20px;border:none;border-radius:8px;font-size:14px;
  font-weight:600;cursor:pointer;transition:.15s}
.btn-start{background:#00e676;color:#000}.btn-start:hover{background:#00c853}
.btn-stop{background:#ff5252;color:#fff}.btn-stop:hover{background:#ff1744}
.btn-save{background:#7c4dff;color:#fff}.btn-save:hover{background:#651fff}
.btn-row{display:flex;gap:12px;margin-top:8px}
</style>
</head>
<body>
<div class="card">
  <h1><span class="dot off" id="dot"></span> Controller Forward</h1>
  <div id="stats">
    <div class="stat"><span class="label">Status</span><span class="value" id="s-status">Stopped</span></div>
    <div class="stat"><span class="label">Packets</span><span class="value" id="s-packets">0</span></div>
    <div class="stat"><span class="label">Sender</span><span class="value" id="s-sender">—</span></div>
    <div class="stat"><span class="label">UDP Port</span><span class="value" id="s-port">—</span></div>
  </div>
  <div class="controls">
    <div class="row">
      <label>UDP Port</label>
      <input type="number" id="udpPort" min="1024" max="65535" value="9876">
    </div>
    <div class="toggle-row">
      <span>Start with Windows</span>
      <label class="switch"><input type="checkbox" id="autoStart"><span class="slider"></span></label>
    </div>
    <div class="btn-row">
      <button class="btn btn-start" id="btnToggle" onclick="toggle()">Start</button>
      <button class="btn btn-save" onclick="saveConfig()">Save Config</button>
    </div>
  </div>
</div>
<script>
async function poll(){
  try{
    const r=await fetch('/api/status');
    const d=await r.json();
    document.getElementById('s-status').textContent=d.listening?'Listening':'Stopped';
    document.getElementById('s-packets').textContent=d.packets.toLocaleString();
    document.getElementById('s-sender').textContent=d.senderIP;
    document.getElementById('s-port').textContent=d.udpPort;
    const dot=document.getElementById('dot');
    dot.className='dot '+(d.listening?'on':'off');
    const btn=document.getElementById('btnToggle');
    btn.textContent=d.listening?'Stop':'Start';
    btn.className='btn '+(d.listening?'btn-stop':'btn-start');
    document.getElementById('udpPort').value=d.udpPort;
    document.getElementById('autoStart').checked=d.autoStart;
  }catch(e){}
}
async function toggle(){
  const r=await fetch('/api/status');
  const d=await r.json();
  await fetch(d.listening?'/api/stop':'/api/start',{method:'POST'});
  setTimeout(poll,300);
}
async function saveConfig(){
  const body=JSON.stringify({
    udpPort:parseInt(document.getElementById('udpPort').value),
    autoStart:document.getElementById('autoStart').checked
  });
  await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body});
  poll();
}
poll();
setInterval(poll,1000);
</script>
</body>
</html>
)HTML";

// ── HTTP Server Thread ──────────────────────────────────────────────────────

static httplib::Server g_httpServer;

static void httpThread() {
    g_httpServer.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(WEB_UI_HTML, "text/html");
    });

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