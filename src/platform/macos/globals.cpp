// SPDX-License-Identifier: LGPL-3.0-or-later
#include "globals.h"

std::unique_ptr<ConfigStore> g_configStore;
std::atomic<bool> g_appRunning{true};
std::atomic<bool> g_listening{false};
std::atomic<bool> g_mdnsResponderActive{false};
std::atomic<bool> g_uiAccess{false};
std::atomic<uint64_t> g_packetCount{0};
std::atomic<uint64_t> g_submitOk{0};
std::atomic<uint64_t> g_submitFail{0};
std::atomic<uint64_t> g_lastLoopUs{0};
std::atomic<uint64_t> g_maxLoopUs{0};
std::atomic<uint32_t> g_senderIP{0};
satellite::SeenDevices g_seenDevices;
httplib::Server* g_clientServer = nullptr;
std::string g_webDir;
UpdateService* g_updateService = nullptr;
satellite::ControllerDbService* g_controllerDbService = nullptr;

std::atomic<uint64_t> g_decryptFail{0};
std::atomic<uint64_t> g_replayDrop{0};

satellite::LogWriter& logWriter() {
    static auto* const writer = new satellite::LogWriter();
    return *writer;
}

void logMsg(LogLevel level, const std::string& source, const std::string& message) {
    logWriter().log(level, source, message);
}
