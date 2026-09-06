// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include "core/json.h"
#include "app/wire_stats.h"

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
    bool controllerAudio = true;
    bool controllerAudioMic = true;
    bool controllerAudioSpeaker = true;
    bool controllerAudioKeepDefaultDevice = true;
    // The operator's opt-in, and whether it actually amounts to anything. They
    // differ on every build without a DSN compiled in, and the UI has to say
    // so rather than imply reports are going somewhere they are not.
    bool crashReporting = false;
    bool crashReportingActive = false;
    bool mdnsResponderActive = false;
    bool backendAvailable = false;
    uint64_t submitOk = 0;
    uint64_t submitFail = 0;
    uint64_t lastLoopUs = 0;
    uint64_t maxLoopUs = 0;
    uint64_t decryptFail = 0;
    uint64_t replayDrop = 0;
    uint64_t logSeq = 0;
    uint64_t peakLoopUs = 0;
    bool clientApiListening = false;
    int connections = 0;
    int controllers = 0;
    int maxControllers = 0;
    RxCounts rx;
    TxCounts tx;
    AudioStreamCounts audio;
    uint64_t authNotPaired = 0;
    uint64_t authBadProof = 0;
    uint64_t sessionsReaped = 0;
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
    j["controllerAudio"] = f.controllerAudio;
    j["controllerAudioMic"] = f.controllerAudioMic;
    j["controllerAudioSpeaker"] = f.controllerAudioSpeaker;
    j["controllerAudioKeepDefaultDevice"] = f.controllerAudioKeepDefaultDevice;
    j["crashReporting"] = f.crashReporting;
    j["crashReportingActive"] = f.crashReportingActive;
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
    j["peakLoopUs"] = f.peakLoopUs;
    j["senderIP"] = f.senderIP;
    j["udpPort"] = f.udpPort;
    j["webPort"] = f.webPort;
    j["decryptFail"] = f.decryptFail;
    j["replayDrop"] = f.replayDrop;
    j["backendAvailable"] = f.backendAvailable;
    j["backend"] = f.backend;
    j["mdnsResponderActive"] = f.mdnsResponderActive;
    j["clientApiListening"] = f.clientApiListening;
    j["connections"] = f.connections;
    j["controllers"] = f.controllers;
    j["maxControllers"] = f.maxControllers;

    JsonOut rx;
    rx["input"] = f.rx.input;
    rx["heartbeat"] = f.rx.heartbeat;
    rx["motion"] = f.rx.motion;
    rx["battery"] = f.rx.battery;
    rx["pointer"] = f.rx.pointer;
    rx["micAudio"] = f.rx.micAudio;
    rx["malformed"] = f.rx.malformed;
    rx["unknownType"] = f.rx.unknownType;
    rx["runt"] = f.rx.runt;
    rx["unknownToken"] = f.rx.unknownToken;
    j["rx"] = std::move(rx);

    JsonOut tx;
    tx["packets"] = f.tx.packets;
    tx["bytes"] = f.tx.bytes;
    tx["heartbeatAck"] = f.tx.heartbeatAck;
    tx["rumble"] = f.tx.rumble;
    tx["lightbar"] = f.tx.lightbar;
    tx["triggerEffects"] = f.tx.triggerEffects;
    tx["playerLeds"] = f.tx.playerLeds;
    tx["speakerAudio"] = f.tx.speakerAudio;
    tx["micLed"] = f.tx.micLed;
    tx["sessionClose"] = f.tx.sessionClose;
    tx["unroutable"] = f.tx.unroutable;
    tx["encryptFailed"] = f.tx.encryptFailed;
    tx["oversize"] = f.tx.oversize;
    tx["sendFailed"] = f.tx.sendFailed;
    j["tx"] = std::move(tx);

    JsonOut audio;
    audio["micAccepted"] = f.audio.micAccepted;
    audio["micDropped"] = f.audio.micDropped;
    audio["micLate"] = f.audio.micLate;
    audio["micDecoded"] = f.audio.micDecoded;
    audio["micFecRecovered"] = f.audio.micFecRecovered;
    audio["micConcealed"] = f.audio.micConcealed;
    audio["speakerSent"] = f.audio.speakerSent;
    audio["speakerSilenceSuppressed"] = f.audio.speakerSilenceSuppressed;
    audio["speakerEncodeFailed"] = f.audio.speakerEncodeFailed;
    audio["speakerLockContended"] = f.audio.speakerLockContended;
    j["audio"] = std::move(audio);

    JsonOut auth;
    auth["notPaired"] = f.authNotPaired;
    auth["badProof"] = f.authBadProof;
    j["auth"] = std::move(auth);

    j["sessionsReaped"] = f.sessionsReaped;
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
