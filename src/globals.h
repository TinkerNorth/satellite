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
#include <unordered_map>
#include <random>
#include <chrono>
#include <sstream>
#include <algorithm>
#include <array>
#include <functional>

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
inline const char* APP_NAME = "satellite";
inline const char* APP_TITLE = "Satellite";
inline const int DEFAULT_UDP_PORT = 9876;
inline const int DEFAULT_WEB_PORT = 9877;
inline const int DEFAULT_PAIR_PORT = 9878;
inline const int DEFAULT_DISC_PORT = 9879;
inline const UINT WM_TRAYICON = WM_APP + 1;
inline const UINT IDM_OPEN_UI = 1001;
inline const UINT IDM_TOGGLE = 1002;
inline const UINT IDM_EXIT = 1003;

// ── Protocol constants ──────────────────────────────────────────────────────
inline const uint16_t MSG_GAMEPAD_DATA      = 0x0001;
inline const uint16_t MSG_HEARTBEAT_PING    = 0x0002;
inline const uint16_t MSG_HEARTBEAT_ACK     = 0x0003;
inline const uint16_t MSG_CONTROLLER_ADD    = 0x0004;
inline const uint16_t MSG_CONTROLLER_REMOVE = 0x0005;

inline const int HEADER_SIZE       = 8;   // token(4) + counter(4)
inline const int INNER_HEADER_SIZE = 4;   // type(2) + length(2)
inline const int AUTH_TAG_SIZE     = 16;  // Poly1305
inline const int CRYPTO_KEY_SIZE   = 32;  // ChaCha20-Poly1305 key
inline const int CRYPTO_NONCE_SIZE = 12;  // IETF nonce

inline const int HEARTBEAT_INTERVAL_SEC = 2;
inline const int HEARTBEAT_MISS_MAX     = 3;
inline const int MAX_CONTROLLERS_PER_CONN = 16;
inline const int MAX_VIGEM_CONTROLLERS    = 16;

// ── Paired device info ──────────────────────────────────────────────────────
struct PairedDevice {
    std::string id;
    std::string name;
    std::string lastIP;
    std::string pairedAt;
    std::string sharedKeyHex; // 64-char hex string (32 bytes)
};

// ── Config ──────────────────────────────────────────────────────────────────
struct Config {
    int udpPort = DEFAULT_UDP_PORT;
    int webPort = DEFAULT_WEB_PORT;
    int pairPort = DEFAULT_PAIR_PORT;
    int discPort = DEFAULT_DISC_PORT;
    bool autoStart = false;
    std::string credentials; // DPAPI-encrypted "username:salt:sha256hash"
    std::vector<PairedDevice> pairedDevices;
};

// ── Controller state (per virtual gamepad) ──────────────────────────────────
struct Controller {
    uint8_t  index    = 0;      // 0-based index within the connection
    ULONG    serialNo = 0;      // ViGEm serial number
    bool     active   = false;
    XUSB_REPORT lastReport{};
    HANDLE   submitEvent = nullptr; // pre-allocated overlapped event
};

// ── Connection state (per paired client session) ────────────────────────────
struct Connection {
    uint32_t token       = 0;
    std::string deviceId;
    std::string deviceName;
    std::string clientIP;
    uint8_t  sharedKey[CRYPTO_KEY_SIZE] = {};
    uint32_t lastCounter = 0;         // replay protection
    std::chrono::steady_clock::time_point lastPacketTime;
    std::chrono::steady_clock::time_point connectedAt;
    std::array<Controller, MAX_CONTROLLERS_PER_CONN> controllers;
    int activeControllerCount = 0;
    sockaddr_in clientAddr{};          // for sending replies (heartbeat ACK)
};

// ── Shared global state ─────────────────────────────────────────────────────
extern Config g_config;
extern std::mutex g_configMtx;
extern std::atomic<bool> g_appRunning;
extern std::atomic<bool> g_listening;
extern std::atomic<bool> g_wantListen;
extern std::atomic<uint64_t> g_packetCount;
extern std::atomic<uint64_t> g_submitOk;
extern std::atomic<uint64_t> g_submitFail;
extern std::atomic<uint64_t> g_lastLoopUs;
extern std::atomic<uint64_t> g_maxLoopUs;
extern std::atomic<uint32_t> g_senderIP;
extern HWND g_hwnd;
extern httplib::Server g_httpServer;
extern SOCKET g_pairSock;
extern std::string g_webDir;

// ── Connection management globals ───────────────────────────────────────────
extern std::mutex g_connMtx;
extern std::unordered_map<uint32_t, Connection> g_connections;  // token → Connection
extern HANDLE g_busDevice;                                       // shared ViGEm bus handle
extern std::atomic<uint64_t> g_decryptFail;                      // failed decryptions
extern std::atomic<uint64_t> g_replayDrop;                       // replay drops
extern SOCKET g_udpSock;                                         // shared UDP socket for sending replies

// ── Serial allocator (tracks which ViGEm serial numbers are in use) ─────────
extern std::mutex g_serialMtx;
extern bool g_serialInUse[MAX_VIGEM_CONTROLLERS]; // index 0 = serial 1

// ── Logging ─────────────────────────────────────────────────────────────────
enum class LogLevel { INFO, WARN, ERR };

struct LogEntry {
    std::chrono::system_clock::time_point timestamp;
    LogLevel level;
    std::string source;   // e.g. "receiver", "pairing", "webserver"
    std::string message;
};

inline const int LOG_RING_SIZE = 500;  // keep last 500 entries

extern std::mutex g_logMtx;
extern std::vector<LogEntry> g_logRing;
extern int g_logHead;           // next write position
extern uint64_t g_logSeq;      // monotonic sequence number (total logs ever written)

void logMsg(LogLevel level, const std::string& source, const std::string& message);
