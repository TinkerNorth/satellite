// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include "core/ipv4_util.h"
#include "core/json.h"
#include "core/update_types.h"
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
    bool controllerAudioHaptics = true;
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
    j["controllerAudioHaptics"] = f.controllerAudioHaptics;
    j["controllerAudioKeepDefaultDevice"] = f.controllerAudioKeepDefaultDevice;
    j["crashReporting"] = f.crashReporting;
    j["crashReportingActive"] = f.crashReportingActive;
    j["mdnsResponderActive"] = f.mdnsResponderActive;
    j["backendAvailable"] = f.backendAvailable;
    j["backend"] = f.backend;
    return jsonDump(j);
}

inline JsonOut rxCountsJson(const RxCounts& rx) {
    JsonOut j;
    j["input"] = rx.input;
    j["heartbeat"] = rx.heartbeat;
    j["motion"] = rx.motion;
    j["battery"] = rx.battery;
    j["pointer"] = rx.pointer;
    j["micAudio"] = rx.micAudio;
    j["malformed"] = rx.malformed;
    j["unknownType"] = rx.unknownType;
    j["runt"] = rx.runt;
    j["unknownToken"] = rx.unknownToken;
    return j;
}

inline JsonOut txCountsJson(const TxCounts& tx) {
    JsonOut j;
    j["packets"] = tx.packets;
    j["bytes"] = tx.bytes;
    j["heartbeatAck"] = tx.heartbeatAck;
    j["rumble"] = tx.rumble;
    j["lightbar"] = tx.lightbar;
    j["triggerEffects"] = tx.triggerEffects;
    j["playerLeds"] = tx.playerLeds;
    j["speakerAudio"] = tx.speakerAudio;
    j["hapticAudio"] = tx.hapticAudio;
    j["micLed"] = tx.micLed;
    j["sessionClose"] = tx.sessionClose;
    j["unroutable"] = tx.unroutable;
    j["encryptFailed"] = tx.encryptFailed;
    j["oversize"] = tx.oversize;
    j["sendFailed"] = tx.sendFailed;
    return j;
}

inline JsonOut audioCountsJson(const AudioStreamCounts& audio) {
    JsonOut j;
    j["micAccepted"] = audio.micAccepted;
    j["micDropped"] = audio.micDropped;
    j["micLate"] = audio.micLate;
    j["micDecoded"] = audio.micDecoded;
    j["micFecRecovered"] = audio.micFecRecovered;
    j["micConcealed"] = audio.micConcealed;
    j["speakerSent"] = audio.speakerSent;
    j["speakerSilenceSuppressed"] = audio.speakerSilenceSuppressed;
    j["speakerEncodeFailed"] = audio.speakerEncodeFailed;
    j["speakerLockContended"] = audio.speakerLockContended;
    j["hapticSent"] = audio.hapticSent;
    j["hapticSilenceSuppressed"] = audio.hapticSilenceSuppressed;
    j["hapticEncodeFailed"] = audio.hapticEncodeFailed;
    j["hapticLockContended"] = audio.hapticLockContended;
    j["hapticReducedToRumble"] = audio.hapticReducedToRumble;
    return j;
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

    j["rx"] = rxCountsJson(f.rx);
    j["tx"] = txCountsJson(f.tx);
    j["audio"] = audioCountsJson(f.audio);

    JsonOut auth;
    auth["notPaired"] = f.authNotPaired;
    auth["badProof"] = f.authBadProof;
    j["auth"] = std::move(auth);

    j["sessionsReaped"] = f.sessionsReaped;
    return jsonDump(j);
}

// The dashboard's sender label: "none" until a packet has arrived, then the
// dotted quad of the last one.
inline std::string senderIpLabel(uint32_t nbo) { return nbo == 0 ? "none" : formatIPv4Nbo(nbo); }

// One server-sent event, framed as EventSource reads it.
inline std::string sseEvent(const std::string& event, const std::string& data) {
    return "event: " + event + "\ndata: " + data + "\n\n";
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

inline std::string buildUpdateJson(const UpdateStatusSnapshot& s) {
    JsonOut j;
    j["state"] = updateStateName(s.state);
    j["currentVersion"] = s.currentVersion;
    j["platformId"] = s.platformId;
    j["channel"] = s.channel;
    j["autoCheck"] = s.autoCheck;
    j["autoDownload"] = s.autoDownload;
    j["autoInstall"] = s.autoInstall;
    j["lastCheckEpoch"] = s.lastCheckEpoch;
    j["bytesDownloaded"] = s.bytesDownloaded;
    j["totalBytes"] = s.totalBytes;
    j["message"] = s.message;
    j["failedPhase"] = updateStateName(s.failedPhase);
    j["dismissed"] = s.dismissed;
    JsonOut info;
    info["available"] = s.info.available;
    info["version"] = s.info.version;
    info["channel"] = s.info.channel;
    info["assetName"] = s.info.assetName;
    info["assetSize"] = s.info.assetSize;
    info["assetSha256"] = s.info.assetSha256;
    info["htmlUrl"] = s.info.htmlUrl;
    info["publishedAtEpoch"] = s.info.publishedAtEpoch;
    info["installMethod"] = s.info.installMethod == InstallMethod::SelfInstall ? "self" : "manual";
    info["manualInstruction"] = s.info.manualInstruction;
    info["releaseNotes"] = s.info.releaseNotes;
    j["info"] = std::move(info);
    return jsonDump(j);
}

} // namespace satellite
