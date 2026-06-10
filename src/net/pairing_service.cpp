// SPDX-License-Identifier: LGPL-3.0-or-later

#include "pairing_service.h"
#include "config.h" // g_config, g_configMtx, saveConfig, getCurrentDate
#include "core/types.h"
#include "crypto.h" // hexEncode
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

bool acceptPairingWithPin(const std::string& deviceId, const std::string& operatorPin) {
    const std::string keyHex = mintPairingKeyHex();
    std::string name, ip;
    if (!acceptPairRequest(deviceId, operatorPin, keyHex, name, ip)) return false;
    upsertPairedDevice(deviceId, name, ip, keyHex);
    return true;
}

bool confirmPairing(const std::string& deviceId) {
    const std::string keyHex = mintPairingKeyHex();
    std::string name, ip;
    if (!acceptPairRequestConfirmed(deviceId, keyHex, name, ip)) return false;
    upsertPairedDevice(deviceId, name, ip, keyHex);
    return true;
}

bool declinePairing(const std::string& deviceId) { return denyPairRequest(deviceId); }
