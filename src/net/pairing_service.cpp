// SPDX-License-Identifier: LGPL-3.0-or-later

#include "pairing_service.h"
#include "app/app_state.h"
#include "config.h"
#include "core/hex.h"
#include "core/types.h"
#include "pairing.h"

#include <sodium.h>

#include <algorithm>

namespace {
// Mint a fresh 32-byte pairing key as hex; the caller persists it.
std::string mintPairingKeyHex() {
    uint8_t key[32];
    randombytes_buf(key, sizeof(key));
    std::string hex = hexEncode(key, sizeof(key));
    sodium_memzero(key, sizeof(key));
    return hex;
}

bool replaceDevice(Config& cfg, const PairedDevice& dev) {
    auto& devs = cfg.pairedDevices;
    devs.erase(std::remove_if(devs.begin(), devs.end(),
                              [&](const PairedDevice& d) { return d.id == dev.id; }),
               devs.end());
    devs.push_back(dev);
    return true;
}

bool removeDevice(Config& cfg, const std::string& deviceId, bool& removed) {
    auto& devs = cfg.pairedDevices;
    const size_t before = devs.size();
    devs.erase(std::remove_if(devs.begin(), devs.end(),
                              [&](const PairedDevice& d) { return d.id == deviceId; }),
               devs.end());
    removed = devs.size() != before;
    return removed;
}

bool rekeyDevice(Config& cfg, const std::string& deviceId, const std::string& clientIP,
                 std::string& outKeyHex, bool& rotated) {
    for (auto& d : cfg.pairedDevices) {
        if (d.id != deviceId) continue;
        outKeyHex = mintPairingKeyHex();
        d.sharedKeyHex = outKeyHex;
        d.lastIP = clientIP;
        d.pairedAt = getCurrentDate();
        rotated = true;
        return true;
    }
    return false;
}

bool refreshDevice(Config& cfg, const std::string& deviceId, const std::string& clientIP,
                   const std::string& deviceName) {
    for (auto& d : cfg.pairedDevices) {
        if (d.id != deviceId) continue;
        if (d.lastIP == clientIP && d.name == deviceName) return false;
        d.lastIP = clientIP;
        d.name = deviceName;
        return true;
    }
    return false;
}
} // namespace

void upsertPairedDevice(const std::string& deviceId, const std::string& deviceName,
                        const std::string& clientIP, const std::string& sharedKeyHex) {
    PairedDevice dev;
    dev.id = deviceId;
    dev.name = deviceName.empty() ? ("Device-" + deviceId.substr(0, 8)) : deviceName;
    dev.lastIP = clientIP;
    dev.pairedAt = getCurrentDate();
    dev.sharedKeyHex = sharedKeyHex;
    g_configStore->update([&dev](Config& cfg) { return replaceDevice(cfg, dev); });
}

bool rotatePairedDeviceKey(const std::string& deviceId, const std::string& clientIP,
                           std::string& outKeyHex) {
    bool rotated = false;
    g_configStore->update(
        [&](Config& cfg) { return rekeyDevice(cfg, deviceId, clientIP, outKeyHex, rotated); });
    return rotated;
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
    const std::shared_ptr<const Config> cfg = g_configStore->current();
    for (const auto& d : cfg->pairedDevices) {
        if (d.id != deviceId) continue;
        out = d;
        return true;
    }
    return false;
}

void refreshPairedDeviceIdentity(const std::string& deviceId, const std::string& clientIP,
                                 const std::string& deviceName) {
    g_configStore->update(
        [&](Config& cfg) { return refreshDevice(cfg, deviceId, clientIP, deviceName); });
}

bool forgetPairedDevice(const std::string& deviceId) {
    bool removed = false;
    g_configStore->update([&](Config& cfg) { return removeDevice(cfg, deviceId, removed); });
    return removed;
}
