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
                        const std::string& clientIP, const std::string& sharedKeyHex,
                        const std::string& initialTouchpadMode) {
    PairedDevice dev;
    dev.id = deviceId;
    dev.name = deviceName.empty() ? ("Device-" + deviceId.substr(0, 8)) : deviceName;
    dev.lastIP = clientIP;
    dev.pairedAt = getCurrentDate();
    dev.sharedKeyHex = sharedKeyHex;
    // The client is the authority on its own touchpad capability; honour a
    // valid initial mode, else leave the default (OFF).
    if (initialTouchpadMode == "ds4" || initialTouchpadMode == "mouse" ||
        initialTouchpadMode == "off") {
        dev.touchpadMode = touchpadModeFromName(initialTouchpadMode);
    }
    std::lock_guard<std::mutex> lk(g_configMtx);
    auto& devs = g_config.pairedDevices;
    devs.erase(std::remove_if(devs.begin(), devs.end(),
                              [&](const PairedDevice& d) { return d.id == deviceId; }),
               devs.end());
    devs.push_back(dev);
    saveConfig(g_config);
}

namespace {
// Mint a fresh 32-byte session key as hex; the caller persists it.
std::string mintSessionKeyHex() {
    uint8_t key[32];
    randombytes_buf(key, sizeof(key));
    std::string hex = hexEncode(key, sizeof(key));
    sodium_memzero(key, sizeof(key));
    return hex;
}
} // namespace

bool acceptPairingWithPin(const std::string& deviceId, const std::string& operatorPin) {
    const std::string keyHex = mintSessionKeyHex();
    std::string name, ip;
    if (!acceptPairRequest(deviceId, operatorPin, keyHex, name, ip)) return false;
    upsertPairedDevice(deviceId, name, ip, keyHex, "");
    return true;
}

bool confirmPairing(const std::string& deviceId) {
    const std::string keyHex = mintSessionKeyHex();
    std::string name, ip;
    if (!acceptPairRequestConfirmed(deviceId, keyHex, name, ip)) return false;
    upsertPairedDevice(deviceId, name, ip, keyHex, "");
    return true;
}

bool declinePairing(const std::string& deviceId) { return denyPairRequest(deviceId); }
