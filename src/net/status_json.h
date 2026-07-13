// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include "core/json.h"

#include <cstdint>
#include <string>

namespace satellite {

struct StatusFields {
    bool listening = false;
    uint64_t packets = 0;
    std::string senderIP;
    int udpPort = 0;
    int webPort = 0;
    bool autoStart = false;
    bool discoveryBroadcastEnabled = false;
    bool mdnsResponderActive = false;
    bool backendAvailable = false;
    uint64_t submitOk = 0;
    uint64_t submitFail = 0;
    uint64_t lastLoopUs = 0;
    uint64_t maxLoopUs = 0;
    uint64_t decryptFail = 0;
    uint64_t replayDrop = 0;
    uint64_t logSeq = 0;
    JsonOut backend;
};

inline std::string buildStatusJson(const StatusFields& f) {
    JsonOut j;
    j["listening"] = f.listening;
    j["packets"] = f.packets;
    j["senderIP"] = f.senderIP;
    j["udpPort"] = f.udpPort;
    j["webPort"] = f.webPort;
    j["autoStart"] = f.autoStart;
    j["discoveryBroadcastEnabled"] = f.discoveryBroadcastEnabled;
    j["mdnsResponderActive"] = f.mdnsResponderActive;
    j["backendAvailable"] = f.backendAvailable;
    j["backend"] = f.backend;
    return jsonDump(j);
}

inline std::string buildDebugJson(const StatusFields& f) {
    JsonOut j;
    j["listening"] = f.listening;
    j["packets"] = f.packets;
    j["submitOk"] = f.submitOk;
    j["submitFail"] = f.submitFail;
    j["lastLoopUs"] = f.lastLoopUs;
    j["maxLoopUs"] = f.maxLoopUs;
    j["senderIP"] = f.senderIP;
    j["udpPort"] = f.udpPort;
    j["decryptFail"] = f.decryptFail;
    j["replayDrop"] = f.replayDrop;
    j["backendAvailable"] = f.backendAvailable;
    j["backend"] = f.backend;
    return jsonDump(j);
}

inline JsonOut buildSseStatusObject(const StatusFields& f) {
    JsonOut j;
    j["listening"] = f.listening;
    j["packets"] = f.packets;
    j["senderIP"] = f.senderIP;
    j["udpPort"] = f.udpPort;
    j["autoStart"] = f.autoStart;
    j["backendAvailable"] = f.backendAvailable;
    j["backend"] = f.backend;
    j["submitOk"] = f.submitOk;
    j["submitFail"] = f.submitFail;
    j["lastLoopUs"] = f.lastLoopUs;
    j["decryptFail"] = f.decryptFail;
    j["replayDrop"] = f.replayDrop;
    j["logSeq"] = f.logSeq;
    return j;
}

} // namespace satellite
