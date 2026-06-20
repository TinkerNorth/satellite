// SPDX-License-Identifier: LGPL-3.0-or-later

// Portable (de)serialization of the persisted Config, shared by every platform's
// config.cpp so the on-disk schema stays identical across Windows/macOS/Linux.
// The platform layer owns only path resolution and the atomic write.
#pragma once

#include "core/json.h"
#include "core/types.h"

#include <string>

namespace satellite {

// Pretty-printed config.json body (trailing newline for a tidy POSIX file).
// Field order mirrors the historical hand-written layout so diffs of an
// upgraded config stay minimal.
inline std::string serializeConfig(const Config& cfg) {
    JsonOut j;
    j["udpPort"] = cfg.udpPort;
    j["webPort"] = cfg.webPort;
    j["pairPort"] = cfg.pairPort;
    j["discPort"] = cfg.discPort;
    j["discoveryBroadcastEnabled"] = cfg.discoveryBroadcastEnabled;
    j["autoStart"] = cfg.autoStart;
    j["updateChannel"] = cfg.updateChannel;
    j["autoCheck"] = cfg.autoCheck;
    j["autoDownload"] = cfg.autoDownload;
    j["autoInstall"] = cfg.autoInstall;
    j["updateCheckIntervalHours"] = cfg.updateCheckIntervalHours;
    j["lastCheckEpoch"] = cfg.lastCheckEpoch;
    j["lastSeenVersion"] = cfg.lastSeenVersion;
    j["skipVersion"] = cfg.skipVersion;
    j["networkInterface"] = cfg.networkInterface;
    j["allowPublicNetwork"] = cfg.allowPublicNetwork;

    JsonOut devices = JsonOut::array();
    for (const auto& d : cfg.pairedDevices) {
        JsonOut o;
        o["id"] = d.id;
        o["name"] = d.name;
        o["lastIP"] = d.lastIP;
        o["pairedAt"] = d.pairedAt;
        o["sharedKey"] = d.sharedKeyHex; // on-disk key name predates the *Hex suffix
        devices.push_back(std::move(o));
    }
    j["pairedDevices"] = std::move(devices);
    return jsonDumpPretty(j) + "\n";
}

// Tolerant in-place load: only present, correctly-typed keys overwrite `cfg`, so
// an older/partial config keeps the struct defaults for keys it never wrote
// (matches the pre-nlohmann behavior the OTA/discovery defaults depend on).
inline void parseConfigInto(const std::string& text, Config& cfg) {
    Json j;
    if (!jsonParse(text, j) || !j.is_object()) return;

    long n = 0;
    if (jsonTryInt(j, "udpPort", n) && n > 0) cfg.udpPort = static_cast<int>(n);
    if (jsonTryInt(j, "webPort", n) && n > 0) cfg.webPort = static_cast<int>(n);
    if (jsonTryInt(j, "pairPort", n) && n > 0) cfg.pairPort = static_cast<int>(n);
    if (jsonTryInt(j, "discPort", n) && n > 0) cfg.discPort = static_cast<int>(n);

    bool b = false;
    if (jsonTryBool(j, "discoveryBroadcastEnabled", b)) cfg.discoveryBroadcastEnabled = b;
    cfg.autoStart = jsonBool(j, "autoStart", cfg.autoStart);

    const std::string ch = jsonStr(j, "updateChannel");
    if (!ch.empty()) cfg.updateChannel = ch;
    if (jsonTryBool(j, "autoCheck", b)) cfg.autoCheck = b;
    if (jsonTryBool(j, "autoDownload", b)) cfg.autoDownload = b;
    if (jsonTryBool(j, "autoInstall", b)) cfg.autoInstall = b;
    if (jsonTryInt(j, "updateCheckIntervalHours", n) && n > 0)
        cfg.updateCheckIntervalHours = static_cast<int>(n);

    int64_t epoch = 0;
    if (jsonTryI64(j, "lastCheckEpoch", epoch) && epoch >= 0) cfg.lastCheckEpoch = epoch;

    cfg.lastSeenVersion = jsonStr(j, "lastSeenVersion");
    cfg.skipVersion = jsonStr(j, "skipVersion");
    cfg.networkInterface = jsonStr(j, "networkInterface");
    cfg.allowPublicNetwork = jsonBool(j, "allowPublicNetwork", cfg.allowPublicNetwork);

    auto it = j.find("pairedDevices");
    if (it != j.end() && it->is_array()) {
        for (const auto& d : *it) {
            if (!d.is_object()) continue;
            PairedDevice dev;
            dev.id = jsonStr(d, "id");
            dev.name = jsonStr(d, "name");
            dev.lastIP = jsonStr(d, "lastIP");
            dev.pairedAt = jsonStr(d, "pairedAt");
            dev.sharedKeyHex = jsonStr(d, "sharedKey");
            if (!dev.id.empty()) cfg.pairedDevices.push_back(dev);
        }
    }
}

} // namespace satellite
