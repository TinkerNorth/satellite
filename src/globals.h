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

// Pull in domain types (constants, structs, enums) from the core layer.
// This avoids duplication — globals.h re-exports them for legacy includes.
#include "core/types.h"

// ── Win32-only constants (not part of domain) ───────────────────────────────
inline const UINT WM_TRAYICON = WM_APP + 1;
inline const UINT IDM_OPEN_UI = 1001;
inline const UINT IDM_TOGGLE = 1002;
inline const UINT IDM_EXIT = 1003;

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

// ── Telemetry counters (still global — read by webserver SSE) ────────────────
extern std::atomic<uint64_t> g_decryptFail; // failed decryptions
extern std::atomic<uint64_t> g_replayDrop;  // replay drops

// ── Logging (types defined in core/types.h) ─────────────────────────────────
extern std::mutex g_logMtx;
extern std::vector<LogEntry> g_logRing;
extern int g_logHead;     // next write position
extern uint64_t g_logSeq; // monotonic sequence number (total logs ever written)

void logMsg(LogLevel level, const std::string& source, const std::string& message);
