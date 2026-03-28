/*
 * globals.cpp — Definition of shared global state
 */
#include "globals.h"

Config g_config;
std::mutex g_configMtx;
std::atomic<bool> g_appRunning{true};
std::atomic<bool> g_listening{false};
std::atomic<bool> g_wantListen{false};
std::atomic<uint64_t> g_packetCount{0};
std::atomic<uint64_t> g_submitOk{0};
std::atomic<uint64_t> g_submitFail{0};
std::atomic<uint64_t> g_lastLoopUs{0};
std::atomic<uint64_t> g_maxLoopUs{0};
std::atomic<uint32_t> g_senderIP{0};
HWND g_hwnd = nullptr;
httplib::Server g_httpServer;
SOCKET g_pairSock = INVALID_SOCKET;
std::string g_webDir;

// ── Connection management globals ───────────────────────────────────────────
std::mutex g_connMtx;
std::unordered_map<uint32_t, Connection> g_connections;
HANDLE g_busDevice = INVALID_HANDLE_VALUE;
std::atomic<uint64_t> g_decryptFail{0};
std::atomic<uint64_t> g_replayDrop{0};
SOCKET g_udpSock = INVALID_SOCKET;

// ── Serial allocator ────────────────────────────────────────────────────────
std::mutex g_serialMtx;
bool g_serialInUse[MAX_VIGEM_CONTROLLERS] = {};

// ── Logging ─────────────────────────────────────────────────────────────────
std::mutex g_logMtx;
std::vector<LogEntry> g_logRing(LOG_RING_SIZE);
int g_logHead = 0;
uint64_t g_logSeq = 0;

void logMsg(LogLevel level, const std::string& source, const std::string& message) {
    std::lock_guard<std::mutex> lk(g_logMtx);
    auto& entry = g_logRing[g_logHead];
    entry.timestamp = std::chrono::system_clock::now();
    entry.level = level;
    entry.source = source;
    entry.message = message;
    g_logHead = (g_logHead + 1) % LOG_RING_SIZE;
    ++g_logSeq;
}
