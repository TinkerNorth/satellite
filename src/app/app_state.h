// SPDX-License-Identifier: LGPL-3.0-or-later

// Definitions live in the per-platform globals.cpp.
#pragma once

#include "core/types.h"
#include "net/net_compat.h"

#include "httplib.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

extern Config g_config;
extern std::mutex g_configMtx;

extern std::atomic<bool> g_appRunning;
extern std::atomic<bool> g_listening;

// True while the mDNS responder thread holds its multicast socket; false when
// bind/join failed or the thread exited. Surfaced read-only in the web UI.
extern std::atomic<bool> g_mdnsResponderActive;

// Telemetry counters (read by webserver SSE).
extern std::atomic<uint64_t> g_packetCount;
extern std::atomic<uint64_t> g_submitOk;
extern std::atomic<uint64_t> g_submitFail;
extern std::atomic<uint64_t> g_lastLoopUs;
extern std::atomic<uint64_t> g_maxLoopUs;
extern std::atomic<uint32_t> g_senderIP;
extern std::atomic<uint64_t> g_decryptFail;
extern std::atomic<uint64_t> g_replayDrop;

// g_httpServer is the admin UI + admin API (plain HTTP, 127.0.0.1).
// g_clientServer points at the sender-facing HTTPS server; owned by
// clientApiThread (an httplib::SSLServer has no default ctor so can't be a
// global) and null until that thread constructs it.
extern httplib::Server g_httpServer;
extern httplib::Server* g_clientServer;
extern std::string g_webDir;

// Owned by main.cpp per platform; null where the updater isn't wired.
class UpdateService;
extern UpdateService* g_updateService;

extern std::mutex g_logMtx;
extern std::vector<LogEntry> g_logRing;
extern int g_logHead;     // next write position
extern uint64_t g_logSeq; // monotonic sequence number (total logs ever written)

void logMsg(LogLevel level, const std::string& source, const std::string& message);
