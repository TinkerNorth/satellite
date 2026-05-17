// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * app_state.h — Portable shared app state (extern declarations).
 *
 * These were previously declared in platform/windows/globals.h, which is
 * not reachable from a macOS/Linux build. Extracting them here lets the
 * net/ layer depend only on portable headers.
 *
 * Definitions live in the per-platform globals.cpp (currently just
 * platform/windows/globals.cpp; a platform/macos/globals.cpp will mirror
 * it in Stage 5).
 */
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

// ── Config ──────────────────────────────────────────────────────────────────
extern Config g_config;
extern std::mutex g_configMtx;

// ── App lifecycle / listen state ────────────────────────────────────────────
extern std::atomic<bool> g_appRunning;
extern std::atomic<bool> g_listening;
extern std::atomic<bool> g_wantListen;

// True while the mDNS / Bonjour responder thread holds its multicast socket
// (Task 1.6). False when the bind/join failed or the thread has exited.
// Surfaced read-only in the web UI so operators can see the modern discovery
// path is live without grepping the log ring.
extern std::atomic<bool> g_mdnsResponderActive;

// ── Telemetry counters (read by webserver SSE) ──────────────────────────────
extern std::atomic<uint64_t> g_packetCount;
extern std::atomic<uint64_t> g_submitOk;
extern std::atomic<uint64_t> g_submitFail;
extern std::atomic<uint64_t> g_lastLoopUs;
extern std::atomic<uint64_t> g_maxLoopUs;
extern std::atomic<uint32_t> g_senderIP;
extern std::atomic<uint64_t> g_decryptFail;
extern std::atomic<uint64_t> g_replayDrop;

// ── Shared networking state ─────────────────────────────────────────────────
extern httplib::Server g_httpServer;
extern SOCKET g_pairSock;
extern std::string g_webDir;

// ── OTA updater (composition-root assigned, nullable) ───────────────────────
// Owned by main.cpp on each platform. Webserver and tray code use this
// pointer when present; null on platforms where the updater isn't wired
// (e.g. portable Linux builds that still link the .cpp may opt out).
class UpdateService;
extern UpdateService* g_updateService;

// ── Log ring ────────────────────────────────────────────────────────────────
extern std::mutex g_logMtx;
extern std::vector<LogEntry> g_logRing;
extern int g_logHead;     // next write position
extern uint64_t g_logSeq; // monotonic sequence number (total logs ever written)

void logMsg(LogLevel level, const std::string& source, const std::string& message);
