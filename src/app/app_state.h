// SPDX-License-Identifier: LGPL-3.0-or-later

// Definitions live in the per-platform globals.cpp.
#pragma once

#include "core/config_store.h"
#include "core/log_writer.h"
#include "core/seen_devices.h"
#include "core/tracked_mutex.h"
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
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

extern std::unique_ptr<ConfigStore> g_configStore;

extern std::atomic<bool> g_appRunning;
extern std::atomic<bool> g_listening;

// True while the mDNS responder thread holds its multicast socket; false when
// bind/join failed or the thread exited. Surfaced read-only in the web UI.
extern std::atomic<bool> g_mdnsResponderActive;
extern std::atomic<bool> g_uiAccess;

// Telemetry counters (read by webserver SSE).
extern std::atomic<uint64_t> g_packetCount;
extern std::atomic<uint64_t> g_submitOk;
extern std::atomic<uint64_t> g_submitFail;
extern std::atomic<uint64_t> g_lastLoopUs;
extern std::atomic<uint64_t> g_maxLoopUs;
extern std::atomic<uint32_t> g_senderIP;
// The device ids and names the pairing and session routes have met this run,
// for the diagnostics scrubbers (core/seen_devices.h).
extern satellite::SeenDevices g_seenDevices;

extern std::atomic<uint64_t> g_decryptFail;
extern std::atomic<uint64_t> g_replayDrop;

// adminHttpServer() is the admin UI + admin API (plain HTTP, 127.0.0.1),
// built on first use rather than before main (its constructor may throw).
// g_clientServer points at the sender-facing HTTPS server; owned by
// clientApiThread (an httplib::SSLServer has no default ctor so can't be a
// global) and null until that thread constructs it.
httplib::Server& adminHttpServer();
extern httplib::Server* g_clientServer;
extern std::string g_webDir;

// Owned by main.cpp per platform; null where the updater isn't wired.
class UpdateService;
extern UpdateService* g_updateService;

// Owned by main.cpp per platform; null until it is loaded, which is before
// either HTTP server starts.
namespace satellite {
class ControllerDbService;
}
extern satellite::ControllerDbService* g_controllerDbService;

satellite::LogWriter& logWriter();

void logMsg(LogLevel level, const std::string& source, const std::string& message);
