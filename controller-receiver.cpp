/*
 * Controller Receiver — service/tray edition
 *
 * Runs as a system-tray application with:
 *   - UDP receiver thread: captures XUSB_REPORT packets, injects via ViGEmBus
 *   - HTTP server thread:  serves a local web UI for configuration
 *   - TCP pairing thread:  handles PIN-based device pairing handshake
 *   - Main thread:         Win32 message loop + tray icon
 *
 * Build:  g++ -O2 -std=c++17 -DCPPHTTPLIB_NO_EXCEPTIONS
 *              -o controller-receiver.exe controller-receiver.cpp
 *              -Ivigem/include -Ilib
 *              -lsetupapi -lws2_32 -lshell32 -lole32 -ladvapi32 -lbcrypt -lcrypt32 -mwindows
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <fstream>
#include <vector>
#include <map>
#include <random>
#include <chrono>
#include <sstream>
#include <algorithm>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <setupapi.h>
#include <shlobj.h>
#include <wincrypt.h>
#include <bcrypt.h>

#include "ViGEm/BusShared.h"
#include "httplib.h"

// ── Forward declarations ────────────────────────────────────────────────────
static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

// ── Constants ───────────────────────────────────────────────────────────────
static const char* APP_NAME = "satellite";
static const char* APP_TITLE = "Satellite";
static const int DEFAULT_UDP_PORT = 9876;
static const int DEFAULT_WEB_PORT = 9877;
static const int DEFAULT_PAIR_PORT = 9878;
static const UINT WM_TRAYICON = WM_APP + 1;
static const UINT IDM_OPEN_UI = 1001;
static const UINT IDM_TOGGLE = 1002;
static const UINT IDM_EXIT = 1003;

// ── Paired device info ──────────────────────────────────────────────────────
struct PairedDevice {
    std::string id;
    std::string name;
    std::string lastIP;
    std::string pairedAt;
};

// ── Config ──────────────────────────────────────────────────────────────────
struct Config {
    int udpPort = DEFAULT_UDP_PORT;
    int webPort = DEFAULT_WEB_PORT;
    int pairPort = DEFAULT_PAIR_PORT;
    bool autoStart = false;
    // Auth (DPAPI-encrypted blob, base64-encoded)
    std::string credentials; // "username:salt:sha256hash" encrypted
    // Paired devices
    std::vector<PairedDevice> pairedDevices;
};

// ── Base64 helpers ──────────────────────────────────────────────────────────
static const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64Encode(const std::vector<BYTE>& data) {
    std::string out;
    int val = 0, bits = -6;
    for (BYTE c : data) {
        val = (val << 8) + c;
        bits += 8;
        while (bits >= 0) {
            out.push_back(b64chars[(val >> bits) & 0x3F]);
            bits -= 6;
        }
    }
    if (bits > -6) out.push_back(b64chars[((val << 8) >> (bits + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

static std::vector<BYTE> base64Decode(const std::string& s) {
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; i++) T[(int)b64chars[i]] = i;
    std::vector<BYTE> out;
    int val = 0, bits = -8;
    for (char c : s) {
        if (T[(unsigned char)c] == -1) break;
        val = (val << 6) + T[(unsigned char)c];
        bits += 6;
        if (bits >= 0) {
            out.push_back((BYTE)(val >> bits));
            bits -= 8;
        }
    }
    return out;
}

// ── SHA-256 via Windows BCrypt ──────────────────────────────────────────────
static std::string sha256hex(const std::string& input) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    BYTE hash[32];
    BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0);
    BCryptHashData(hHash, (PUCHAR)input.data(), (ULONG)input.size(), 0);
    BCryptFinishHash(hHash, hash, 32, 0);
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    char hex[65];
    for (int i = 0; i < 32; i++) sprintf(hex + static_cast<ptrdiff_t>(i) * 2, "%02x", hash[i]);
    hex[64] = 0;
    return std::string(hex);
}

// ── DPAPI encrypt/decrypt ───────────────────────────────────────────────────
static std::string dpapiEncrypt(const std::string& plaintext) {
    DATA_BLOB in, out;
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.data()));
    in.cbData = (DWORD)plaintext.size();
    if (!CryptProtectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out)) return "";
    std::vector<BYTE> blob(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    return base64Encode(blob);
}

static std::string dpapiDecrypt(const std::string& encoded) {
    auto blob = base64Decode(encoded);
    DATA_BLOB in, out;
    in.pbData = blob.data();
    in.cbData = (DWORD)blob.size();
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out)) return "";
    std::string result(reinterpret_cast<char*>(out.pbData), out.cbData);
    LocalFree(out.pbData);
    return result;
}

// ── Random hex/digit generation ─────────────────────────────────────────────
static std::string randomHex(int bytes) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);
    std::string out;
    char buf[3];
    for (int i = 0; i < bytes; i++) {
        sprintf(buf, "%02x", dis(gen));
        out += buf;
    }
    return out;
}

static std::string randomDigits(int n) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 9);
    std::string out;
    for (int i = 0; i < n; i++) out += ('0' + dis(gen));
    return out;
}

// ── Session management ──────────────────────────────────────────────────────
static std::mutex g_sessionMtx;
static std::map<std::string, std::chrono::steady_clock::time_point> g_sessions;
static const int SESSION_LIFETIME_HOURS = 24;

static std::string createSession() {
    std::string token = randomHex(32);
    std::lock_guard<std::mutex> lk(g_sessionMtx);
    g_sessions[token] =
        std::chrono::steady_clock::now() + std::chrono::hours(SESSION_LIFETIME_HOURS);
    return token;
}

static bool validateSession(const std::string& token) {
    std::lock_guard<std::mutex> lk(g_sessionMtx);
    auto it = g_sessions.find(token);
    if (it == g_sessions.end()) return false;
    if (std::chrono::steady_clock::now() > it->second) {
        g_sessions.erase(it);
        return false;
    }
    return true;
}

static void removeSession(const std::string& token) {
    std::lock_guard<std::mutex> lk(g_sessionMtx);
    g_sessions.erase(token);
}

static std::string getSessionFromCookie(const httplib::Request& req) {
    auto it = req.headers.find("Cookie");
    if (it == req.headers.end()) return "";
    auto& cookie = it->second;
    auto pos = cookie.find("session=");
    if (pos == std::string::npos) return "";
    auto start = pos + 8;
    auto end = cookie.find(';', start);
    return cookie.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

// ── Credential helpers ──────────────────────────────────────────────────────
static bool isConfigured(const Config& cfg) { return !cfg.credentials.empty(); }

static bool setupCredentials(Config& cfg, const std::string& username,
                             const std::string& password) {
    std::string salt = randomHex(16);
    std::string hash = sha256hex(salt + password);
    std::string plain = username + ":" + salt + ":" + hash;
    cfg.credentials = dpapiEncrypt(plain);
    return !cfg.credentials.empty();
}

static bool verifyCredentials(const Config& cfg, const std::string& username,
                              const std::string& password) {
    std::string plain = dpapiDecrypt(cfg.credentials);
    if (plain.empty()) return false;
    // Parse "username:salt:hash"
    auto p1 = plain.find(':');
    if (p1 == std::string::npos) return false;
    auto p2 = plain.find(':', p1 + 1);
    if (p2 == std::string::npos) return false;
    std::string storedUser = plain.substr(0, p1);
    std::string salt = plain.substr(p1 + 1, p2 - p1 - 1);
    std::string storedHash = plain.substr(p2 + 1);
    return username == storedUser && sha256hex(salt + password) == storedHash;
}

// ── PIN state ───────────────────────────────────────────────────────────────
static std::mutex g_pinMtx;
static std::string g_currentPin;
static std::chrono::steady_clock::time_point g_pinExpiry;

static std::string generatePin() {
    std::lock_guard<std::mutex> lk(g_pinMtx);
    g_currentPin = randomDigits(4);
    g_pinExpiry = std::chrono::steady_clock::now() + std::chrono::minutes(5);
    return g_currentPin;
}

static bool verifyPin(const std::string& pin) {
    std::lock_guard<std::mutex> lk(g_pinMtx);
    if (g_currentPin.empty() || std::chrono::steady_clock::now() > g_pinExpiry) return false;
    bool ok = (pin == g_currentPin);
    if (ok) g_currentPin.clear(); // one-time use
    return ok;
}

// ── JSON string escaping ────────────────────────────────────────────────────
static std::string jsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"')
            out += "\\\"";
        else if (c == '\\')
            out += "\\\\";
        else if (c == '\n')
            out += "\\n";
        else
            out += c;
    }
    return out;
}

// ── Simple JSON string extraction ───────────────────────────────────────────
static std::string jsonGetString(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return "";
    auto colon = json.find(':', pos + needle.size());
    if (colon == std::string::npos) return "";
    auto q1 = json.find('"', colon + 1);
    if (q1 == std::string::npos) return "";
    auto q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos) return "";
    return json.substr(q1 + 1, q2 - q1 - 1);
}

static std::string configPath() {
    char buf[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, buf))) {
        std::string dir = std::string(buf) + "\\satellite";
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
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    // Parse simple fields
    auto getInt = [&](const char* key) -> int {
        auto pos = content.find(std::string("\"") + key + "\"");
        if (pos == std::string::npos) return -1;
        auto colon = content.find(':', pos);
        if (colon == std::string::npos) return -1;
        return atoi(content.c_str() + colon + 1);
    };
    auto getBool = [&](const char* key) -> bool {
        auto pos = content.find(std::string("\"") + key + "\"");
        if (pos == std::string::npos) return false;
        auto colon = content.find(':', pos);
        if (colon == std::string::npos) return false;
        auto rest = content.substr(colon + 1, 10);
        return rest.find("true") != std::string::npos;
    };
    auto getString = [&](const char* key) -> std::string { return jsonGetString(content, key); };

    int v = 0;
    v = getInt("udpPort");
    if (v > 0) cfg.udpPort = v;
    v = getInt("webPort");
    if (v > 0) cfg.webPort = v;
    v = getInt("pairPort");
    if (v > 0) cfg.pairPort = v;
    cfg.autoStart = getBool("autoStart");
    cfg.credentials = getString("credentials");

    // Parse paired devices array
    auto arrStart = content.find("\"pairedDevices\"");
    if (arrStart != std::string::npos) {
        auto bracket = content.find('[', arrStart);
        auto bracketEnd = content.find(']', bracket);
        if (bracket != std::string::npos && bracketEnd != std::string::npos) {
            std::string arr = content.substr(bracket, bracketEnd - bracket + 1);
            // Find each device object
            size_t pos = 0;
            while (true) {
                auto objStart = arr.find('{', pos);
                if (objStart == std::string::npos) break;
                auto objEnd = arr.find('}', objStart);
                if (objEnd == std::string::npos) break;
                std::string obj = arr.substr(objStart, objEnd - objStart + 1);
                PairedDevice dev;
                dev.id = jsonGetString(obj, "id");
                dev.name = jsonGetString(obj, "name");
                dev.lastIP = jsonGetString(obj, "lastIP");
                dev.pairedAt = jsonGetString(obj, "pairedAt");
                if (!dev.id.empty()) cfg.pairedDevices.push_back(dev);
                pos = objEnd + 1;
            }
        }
    }
    return cfg;
}

static void saveConfig(const Config& cfg) {
    std::ofstream f(configPath());
    f << "{\n"
      << "  \"udpPort\": " << cfg.udpPort << ",\n"
      << "  \"webPort\": " << cfg.webPort << ",\n"
      << "  \"pairPort\": " << cfg.pairPort << ",\n"
      << "  \"autoStart\": " << (cfg.autoStart ? "true" : "false") << ",\n"
      << "  \"credentials\": \"" << jsonEscape(cfg.credentials) << "\",\n"
      << "  \"pairedDevices\": [\n";
    for (size_t i = 0; i < cfg.pairedDevices.size(); i++) {
        const auto& d = cfg.pairedDevices[i];
        f << "    {\"id\":\"" << jsonEscape(d.id) << "\",\"name\":\"" << jsonEscape(d.name)
          << "\",\"lastIP\":\"" << jsonEscape(d.lastIP) << "\",\"pairedAt\":\""
          << jsonEscape(d.pairedAt) << "\"}";
        if (i + 1 < cfg.pairedDevices.size()) f << ",";
        f << "\n";
    }
    f << "  ]\n}\n";
}

// ── Shared state ────────────────────────────────────────────────────────────
static Config g_config;
static std::mutex g_configMtx;                // protects g_config writes
static std::atomic<bool> g_appRunning{true};  // overall app lifecycle
static std::atomic<bool> g_listening{false};  // UDP receiver active?
static std::atomic<bool> g_wantListen{false}; // request to start/stop
static std::atomic<uint64_t> g_packetCount{0};
static std::mutex g_senderMtx;
static std::string g_senderIP = "none";
static HWND g_hwnd = nullptr;

// ── ViGEmBus helpers (unchanged) ────────────────────────────────────────────

static HANDLE openVigemBus() {
    SP_DEVICE_INTERFACE_DATA did{};
    did.cbSize = sizeof(did);
    DWORD idx = 0, reqSize = 0;
    HDEVINFO devInfo = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_BUSENUM_VIGEM, nullptr, nullptr,
                                            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

    while (SetupDiEnumDeviceInterfaces(devInfo, nullptr, &GUID_DEVINTERFACE_BUSENUM_VIGEM, idx++,
                                       &did)) {
        SetupDiGetDeviceInterfaceDetailW(devInfo, &did, nullptr, 0, &reqSize, nullptr);
        auto* detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)malloc(reqSize);
        if (!detail) continue;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (SetupDiGetDeviceInterfaceDetailW(devInfo, &did, detail, reqSize, nullptr, nullptr)) {
            HANDLE h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
            free(detail);
            if (h != INVALID_HANDLE_VALUE) {
                OVERLAPPED ov{};
                ov.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                VIGEM_CHECK_VERSION ver;
                VIGEM_CHECK_VERSION_INIT(&ver, VIGEM_COMMON_VERSION);
                DWORD xfr = 0;
                DeviceIoControl(h, IOCTL_VIGEM_CHECK_VERSION, &ver, ver.Size, nullptr, 0, &xfr,
                                &ov);
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

static bool isVigemInstalled() {
    HANDLE h = openVigemBus();
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

static bool pluginTarget(HANDLE bus, ULONG serial) {
    OVERLAPPED ov{};
    ov.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    DWORD xfr = 0;
    VIGEM_PLUGIN_TARGET plug;
    VIGEM_PLUGIN_TARGET_INIT(&plug, serial, Xbox360Wired);
    plug.VendorId = 0x045E;
    plug.ProductId = 0x028E;
    DeviceIoControl(bus, IOCTL_VIGEM_PLUGIN_TARGET, &plug, plug.Size, nullptr, 0, &xfr, &ov);
    bool ok = GetOverlappedResult(bus, &ov, &xfr, TRUE) != 0;
    if (ok) {
        VIGEM_WAIT_DEVICE_READY wr;
        VIGEM_WAIT_DEVICE_READY_INIT(&wr, serial);
        OVERLAPPED ov2{};
        ov2.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        DeviceIoControl(bus, IOCTL_VIGEM_WAIT_DEVICE_READY, &wr, wr.Size, nullptr, 0, &xfr, &ov2);
        GetOverlappedResult(bus, &ov2, &xfr, TRUE);
        CloseHandle(ov2.hEvent);
    }
    CloseHandle(ov.hEvent);
    return ok;
}

static bool submitReport(HANDLE bus, ULONG serial, const XUSB_REPORT& rpt) {
    OVERLAPPED ov{};
    ov.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    DWORD xfr = 0;
    XUSB_SUBMIT_REPORT sr;
    XUSB_SUBMIT_REPORT_INIT(&sr, serial);
    sr.Report = rpt;
    DeviceIoControl(bus, IOCTL_XUSB_SUBMIT_REPORT, &sr, sr.Size, nullptr, 0, &xfr, &ov);
    bool ok = GetOverlappedResult(bus, &ov, &xfr, TRUE) != 0;
    CloseHandle(ov.hEvent);
    return ok;
}

static void unplugTarget(HANDLE bus, ULONG serial) {
    OVERLAPPED ov{};
    ov.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    DWORD xfr = 0;
    VIGEM_UNPLUG_TARGET up;
    VIGEM_UNPLUG_TARGET_INIT(&up, serial);
    DeviceIoControl(bus, IOCTL_VIGEM_UNPLUG_TARGET, &up, up.Size, nullptr, 0, &xfr, &ov);
    GetOverlappedResult(bus, &ov, &xfr, TRUE);
    CloseHandle(ov.hEvent);
}

// ── Auto-start (registry) ───────────────────────────────────────────────────

static void setAutoStart(bool enable) {
    HKEY key;
    const char* run = "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run";
    if (RegOpenKeyExA(HKEY_CURRENT_USER, run, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    if (enable) {
        char exePath[MAX_PATH];
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        RegSetValueExA(key, APP_NAME, 0, REG_SZ, reinterpret_cast<const BYTE*>(exePath),
                       (DWORD)strlen(exePath) + 1);
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
            if (pluginTarget(busDevice, s)) {
                serialNo = s;
                break;
            }
        }
        if (serialNo == 0) {
            CloseHandle(busDevice);
            g_wantListen = false;
            continue;
        }

        // ── Winsock ──
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
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
        addr.sin_port = htons((u_short)port);
        addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
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
        {
            std::lock_guard<std::mutex> lk(g_senderMtx);
            g_senderIP = "none";
        }

        XUSB_REPORT report;
        XUSB_REPORT_INIT(&report);

        // ── Hot loop ──
        while (g_appRunning && g_wantListen) {
            sockaddr_in sender{};
            int slen = sizeof(sender);
            char buf[64];
            int n =
                recvfrom(sock, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&sender), &slen);

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

// ── Auth middleware helper ───────────────────────────────────────────────────
static bool requireAuth(const httplib::Request& req, httplib::Response& res) {
    if (!isConfigured(g_config)) return true; // no credentials set yet
    auto token = getSessionFromCookie(req);
    if (validateSession(token)) return true;
    res.status = 401;
    res.set_content(R"({"error":"unauthorized"})", "application/json");
    return false;
}

// ── HTTP Server Thread ──────────────────────────────────────────────────────

static httplib::Server g_httpServer;

static void httpThread() {
    // Serve static files from the web/ folder
    g_httpServer.set_mount_point("/", g_webDir);

    // ── Auth routes (no auth required) ──────────────────────────────────
    g_httpServer.Get("/api/auth/status", [](const httplib::Request& req, httplib::Response& res) {
        bool configured = isConfigured(g_config);
        bool authenticated = false;
        if (configured) {
            auto token = getSessionFromCookie(req);
            authenticated = validateSession(token);
        }
        char json[128];
        snprintf(json, sizeof(json), R"({"configured":%s,"authenticated":%s})",
                 configured ? "true" : "false", authenticated ? "true" : "false");
        res.set_content(json, "application/json");
    });

    g_httpServer.Post("/api/auth/setup", [](const httplib::Request& req, httplib::Response& res) {
        if (isConfigured(g_config)) {
            res.status = 400;
            res.set_content(R"({"error":"already configured"})", "application/json");
            return;
        }
        auto username = jsonGetString(req.body, "username");
        auto password = jsonGetString(req.body, "password");
        if (username.empty() || password.size() < 4) {
            res.status = 400;
            res.set_content(R"({"error":"username required, password min 4 chars"})",
                            "application/json");
            return;
        }
        std::lock_guard<std::mutex> lk(g_configMtx);
        if (!setupCredentials(g_config, username, password)) {
            res.status = 500;
            res.set_content(R"({"error":"encryption failed"})", "application/json");
            return;
        }
        saveConfig(g_config);
        auto token = createSession();
        res.set_header("Set-Cookie", "session=" + token + "; HttpOnly; Path=/; SameSite=Strict");
        res.set_content(R"({"ok":true})", "application/json");
    });

    g_httpServer.Post("/api/auth/login", [](const httplib::Request& req, httplib::Response& res) {
        auto username = jsonGetString(req.body, "username");
        auto password = jsonGetString(req.body, "password");
        if (!verifyCredentials(g_config, username, password)) {
            res.status = 401;
            res.set_content(R"({"error":"invalid credentials"})", "application/json");
            return;
        }
        auto token = createSession();
        res.set_header("Set-Cookie", "session=" + token + "; HttpOnly; Path=/; SameSite=Strict");
        res.set_content(R"({"ok":true})", "application/json");
    });

    g_httpServer.Post("/api/auth/logout", [](const httplib::Request& req, httplib::Response& res) {
        auto token = getSessionFromCookie(req);
        if (!token.empty()) removeSession(token);
        res.set_header("Set-Cookie", "session=; HttpOnly; Path=/; Max-Age=0");
        res.set_content(R"({"ok":true})", "application/json");
    });

    // ── Protected routes ────────────────────────────────────────────────
    g_httpServer.Get("/api/vigem/status", [](const httplib::Request& req, httplib::Response& res) {
        if (!requireAuth(req, res)) return;
        bool installed = isVigemInstalled();
        char json[64];
        snprintf(json, sizeof(json), R"({"installed":%s})", installed ? "true" : "false");
        res.set_content(json, "application/json");
    });

    g_httpServer.Get("/api/status", [](const httplib::Request& req, httplib::Response& res) {
        if (!requireAuth(req, res)) return;
        std::string senderIP;
        {
            std::lock_guard<std::mutex> lk(g_senderMtx);
            senderIP = g_senderIP;
        }
        char json[512];
        snprintf(
            json, sizeof(json),
            R"({"listening":%s,"packets":%llu,"senderIP":"%s","udpPort":%d,"webPort":%d,"autoStart":%s})",
            g_listening.load() ? "true" : "false", (unsigned long long)g_packetCount.load(),
            senderIP.c_str(), g_config.udpPort, g_config.webPort,
            g_config.autoStart ? "true" : "false");
        res.set_content(json, "application/json");
    });

    g_httpServer.Post("/api/start", [](const httplib::Request& req, httplib::Response& res) {
        if (!requireAuth(req, res)) return;
        g_wantListen = true;
        res.set_content(R"({"ok":true})", "application/json");
    });

    g_httpServer.Post("/api/stop", [](const httplib::Request& req, httplib::Response& res) {
        if (!requireAuth(req, res)) return;
        g_wantListen = false;
        res.set_content(R"({"ok":true})", "application/json");
    });

    g_httpServer.Post("/api/config", [](const httplib::Request& req, httplib::Response& res) {
        if (!requireAuth(req, res)) return;
        auto body = req.body;
        std::lock_guard<std::mutex> lk(g_configMtx);
        auto pPort = body.find("\"udpPort\"");
        if (pPort != std::string::npos) {
            auto colon = body.find(':', pPort);
            if (colon != std::string::npos) {
                int port = atoi(body.c_str() + colon + 1);
                if (port >= 1024 && port <= 65535) g_config.udpPort = port;
            }
        }
        g_config.autoStart = body.find("\"autoStart\":true") != std::string::npos ||
                             body.find("\"autoStart\": true") != std::string::npos;
        setAutoStart(g_config.autoStart);
        saveConfig(g_config);
        res.set_content(R"({"ok":true})", "application/json");
    });

    // ── PIN & device pairing routes ─────────────────────────────────────
    g_httpServer.Post("/api/pin/generate", [](const httplib::Request& req, httplib::Response& res) {
        if (!requireAuth(req, res)) return;
        auto pin = generatePin();
        res.set_content("{\"pin\":\"" + pin + "\"}", "application/json");
    });

    g_httpServer.Get("/api/devices", [](const httplib::Request& req, httplib::Response& res) {
        if (!requireAuth(req, res)) return;
        std::string json = "[";
        std::lock_guard<std::mutex> lk(g_configMtx);
        for (size_t i = 0; i < g_config.pairedDevices.size(); i++) {
            const auto& d = g_config.pairedDevices[i];
            json += "{\"id\":\"" + jsonEscape(d.id) + "\",\"name\":\"" + jsonEscape(d.name) +
                    "\",\"lastIP\":\"" + jsonEscape(d.lastIP) + "\",\"pairedAt\":\"" +
                    jsonEscape(d.pairedAt) + "\"}";
            if (i + 1 < g_config.pairedDevices.size()) json += ",";
        }
        json += "]";
        res.set_content(json, "application/json");
    });

    g_httpServer.Post(
        "/api/devices/remove", [](const httplib::Request& req, httplib::Response& res) {
            if (!requireAuth(req, res)) return;
            auto deviceId = jsonGetString(req.body, "id");
            std::lock_guard<std::mutex> lk(g_configMtx);
            auto& devs = g_config.pairedDevices;
            devs.erase(std::remove_if(devs.begin(), devs.end(),
                                      [&](const PairedDevice& d) { return d.id == deviceId; }),
                       devs.end());
            saveConfig(g_config);
            res.set_content(R"({"ok":true})", "application/json");
        });

    g_httpServer.listen("127.0.0.1", g_config.webPort);
}

// ── TCP Pairing Server Thread ────────────────────────────────────────────────

static SOCKET g_pairSock = INVALID_SOCKET;

static std::string getCurrentDate() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    char buf[32];
    struct tm tm{};
    localtime_s(&tm, &t);
    strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

static void pairingThread() {
    g_pairSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_pairSock == INVALID_SOCKET) return;

    int opt = 1;
    setsockopt(g_pairSock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt),
               sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)g_config.pairPort);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(g_pairSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(g_pairSock);
        g_pairSock = INVALID_SOCKET;
        return;
    }
    listen(g_pairSock, 5);

    // Non-blocking so we can check g_appRunning
    u_long nonBlock = 1;
    ioctlsocket(g_pairSock, FIONBIO, &nonBlock);

    while (g_appRunning) {
        sockaddr_in client{};
        int clen = sizeof(client);
        SOCKET cs = accept(g_pairSock, reinterpret_cast<sockaddr*>(&client), &clen);
        if (cs == INVALID_SOCKET) {
            Sleep(100);
            continue;
        }

        // Set client socket to blocking with timeout
        u_long blocking = 0;
        ioctlsocket(cs, FIONBIO, &blocking);
        DWORD timeout = 5000;
        setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
                   sizeof(timeout));

        char buf[1024] = {};
        int n = recv(cs, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = 0;
            std::string msg(buf);
            // Expected: {"deviceId":"xxx","deviceName":"yyy","pin":"1234"}
            auto deviceId = jsonGetString(msg, "deviceId");
            auto deviceName = jsonGetString(msg, "deviceName");
            auto pin = jsonGetString(msg, "pin");

            char clientIP[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client.sin_addr, clientIP, sizeof(clientIP));

            // Check if already paired
            bool alreadyPaired = false;
            {
                std::lock_guard<std::mutex> lk(g_configMtx);
                auto it = std::find_if(g_config.pairedDevices.begin(), g_config.pairedDevices.end(),
                                       [&](const PairedDevice& d) { return d.id == deviceId; });
                if (it != g_config.pairedDevices.end()) {
                    alreadyPaired = true;
                    it->lastIP = clientIP;
                }
            }

            if (alreadyPaired) {
                send(cs, R"({"ok":true,"message":"already paired"})", 38, 0);
                std::lock_guard<std::mutex> lk(g_configMtx);
                saveConfig(g_config);
            } else if (verifyPin(pin)) {
                PairedDevice dev;
                dev.id = deviceId;
                dev.name = deviceName.empty() ? ("Device-" + deviceId.substr(0, 8)) : deviceName;
                dev.lastIP = clientIP;
                dev.pairedAt = getCurrentDate();
                {
                    std::lock_guard<std::mutex> lk(g_configMtx);
                    g_config.pairedDevices.push_back(dev);
                    saveConfig(g_config);
                }
                send(cs, R"({"ok":true,"message":"paired successfully"})", 43, 0);
            } else {
                send(cs, R"({"ok":false,"error":"invalid or expired PIN"})", 46, 0);
            }
        }
        closesocket(cs);
    }

    closesocket(g_pairSock);
    g_pairSock = INVALID_SOCKET;
}

// ── System Tray ─────────────────────────────────────────────────────────────

static NOTIFYICONDATAA g_nid{};

static void addTrayIcon(HWND hwnd) {
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    // Load custom icon from icon.ico next to the exe, fall back to default
    std::string icoPath = getExeDir() + "\\icon.ico";
    HICON hCustom = (HICON)LoadImageA(nullptr, icoPath.c_str(), IMAGE_ICON, 0, 0,
                                      LR_LOADFROMFILE | LR_DEFAULTSIZE);
    g_nid.hIcon = hCustom ? hCustom : LoadIcon(nullptr, IDI_APPLICATION);
    strncpy(g_nid.szTip, APP_TITLE, sizeof(g_nid.szTip) - 1);
    Shell_NotifyIconA(NIM_ADD, &g_nid);
}

static void removeTrayIcon() { Shell_NotifyIconA(NIM_DELETE, &g_nid); }

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
        if (lp == WM_RBUTTONUP || lp == WM_LBUTTONUP) { showTrayMenu(hwnd); }
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
            if (g_listening.load())
                g_wantListen = false;
            else
                g_wantListen = true;
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

    removeTrayIcon();
    saveConfig(g_config);
    return 0;
}