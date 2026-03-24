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
std::mutex g_senderMtx;
std::string g_senderIP = "none";
HWND g_hwnd = nullptr;
httplib::Server g_httpServer;
SOCKET g_pairSock = INVALID_SOCKET;
std::string g_webDir;
