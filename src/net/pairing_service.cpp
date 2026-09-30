// SPDX-License-Identifier: LGPL-3.0-or-later

#include "pairing_service.h"
#include "app/app_state.h"
#include "config.h" // g_config, g_configMtx, saveConfig, getCurrentDate
#include "core/hex.h"
#include "core/types.h"
#include "pairing.h"

#include <sodium.h>

#include <algorithm>
#include <mutex>

void upsertPairedDevice(const std::string& deviceId, const std::string& deviceName,
                        const std::string& clientIP, const std::string& sharedKeyHex) {
    PairedDevice dev;
    dev.id = deviceId;
    dev.name = deviceName.empty() ? ("Device-" + deviceId.substr(0, 8)) : deviceName;
    dev.lastIP = clientIP;
    dev.pairedAt = getCurrentDate();
    dev.sharedKeyHex = sharedKeyHex;
    std::lock_guard<std::mutex> lk(g_configMtx);
    auto& devs = g_config.pairedDevices;
    devs.erase(std::remove_if(devs.begin(), devs.end(),
                              [&](const PairedDevice& d) { return d.id == deviceId; }),
               devs.end());
    devs.push_back(dev);
    saveConfig(g_config);
}

namespace {
// Mint a fresh 32-byte pairing key as hex; the caller persists it.
std::string mintPairingKeyHex() {
    uint8_t key[32];
    randombytes_buf(key, sizeof(key));
    std::string hex = hexEncode(key, sizeof(key));
    sodium_memzero(key, sizeof(key));
    return hex;
}
} // namespace

bool rotatePairedDeviceKey(const std::string& deviceId, const std::string& clientIP,
                           std::string& outKeyHex) {
    std::lock_guard<std::mutex> lk(g_configMtx);
    for (auto& d : g_config.pairedDevices) {
        if (d.id != deviceId) continue;
        outKeyHex = mintPairingKeyHex();
        d.sharedKeyHex = outKeyHex;
        d.lastIP = clientIP;
        d.pairedAt = getCurrentDate();
        saveConfig(g_config);
        return true;
    }
    return false;
}

bool confirmPairing(const std::string& deviceId) {
    const std::string keyHex = mintPairingKeyHex();
    std::string name, ip;
    if (!acceptPairRequestConfirmed(deviceId, keyHex, name, ip)) {
        logMsg(LogLevel::WARN, "pairing",
               "Accept for " + deviceId +
                   " did nothing; the request already expired or was withdrawn. Ask the device "
                   "to tap Pair again");
        return false;
    }
    upsertPairedDevice(deviceId, name, ip, keyHex);
    logMsg(LogLevel::INFO, "pairing", "Paired device via prompt: " + deviceId + " (" + ip + ")");
    return true;
}

bool declinePairing(const std::string& deviceId) { return denyPairRequest(deviceId); }

bool findPairedDevice(const std::string& deviceId, PairedDevice& out) {
    std::lock_guard<std::mutex> lk(g_configMtx);
    for (const auto& d : g_config.pairedDevices) {
        if (d.id != deviceId) continue;
        out = d;
        return true;
    }
    return false;
}

void refreshPairedDeviceIdentity(const std::string& deviceId, const std::string& clientIP,
                                 const std::string& deviceName) {
    std::lock_guard<std::mutex> lk(g_configMtx);
    for (auto& d : g_config.pairedDevices) {
        if (d.id != deviceId) continue;
        d.lastIP = clientIP;
        d.name = deviceName;
        saveConfig(g_config);
        return;
    }
}

bool forgetPairedDevice(const std::string& deviceId) {
    std::lock_guard<std::mutex> lk(g_configMtx);
    auto& devs = g_config.pairedDevices;
    const size_t before = devs.size();
    devs.erase(std::remove_if(devs.begin(), devs.end(),
                              [&](const PairedDevice& d) { return d.id == deviceId; }),
               devs.end());
    const bool removed = devs.size() != before;
    if (removed) saveConfig(g_config);
    return removed;
}
