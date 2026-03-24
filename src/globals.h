/*
 * globals.h — Shared state, constants, and forward declarations
 */
#pragma once

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

// ── Constants ───────────────────────────────────────────────────────────────
inline const char* APP_NAME          = "controller-forward";
inline const char* APP_TITLE         = "Controller Forward";
inline const int   DEFAULT_UDP_PORT  = 9876;
inline const int   DEFAULT_WEB_PORT  = 9877;
inline const int   DEFAULT_PAIR_PORT = 9878;
inline const int   DEFAULT_DISC_PORT = 9879;
inline const UINT  WM_TRAYICON      = WM_APP + 1;
inline const UINT  IDM_OPEN_UI      = 1001;
inline const UINT  IDM_TOGGLE       = 1002;
inline const UINT  IDM_EXIT         = 1003;

// ── Paired device info ──────────────────────────────────────────────────────
struct PairedDevice {
    std::string id;
    std::string name;
    std::string lastIP;
    std::string pairedAt;
};

// ── Config ──────────────────────────────────────────────────────────────────
struct Config {
    int  udpPort   = DEFAULT_UDP_PORT;
    int  webPort   = DEFAULT_WEB_PORT;
    int  pairPort  = DEFAULT_PAIR_PORT;
    int  discPort  = DEFAULT_DISC_PORT;
    bool autoStart = false;
    std::string credentials;  // DPAPI-encrypted "username:salt:sha256hash"
    std::vector<PairedDevice> pairedDevices;
};

// ── Shared global state ─────────────────────────────────────────────────────
extern Config              g_config;
extern std::mutex          g_configMtx;
extern std::atomic<bool>   g_appRunning;
extern std::atomic<bool>   g_listening;
extern std::atomic<bool>   g_wantListen;
extern std::atomic<uint64_t> g_packetCount;
extern std::mutex          g_senderMtx;
extern std::string         g_senderIP;
extern HWND                g_hwnd;
extern httplib::Server     g_httpServer;
extern SOCKET              g_pairSock;
extern std::string         g_webDir;

