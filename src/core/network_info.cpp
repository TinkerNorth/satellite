// SPDX-License-Identifier: LGPL-3.0-or-later
#include "core/network_info.h"

#include "core/json.h"

#include <string>
#include <vector>

bool isPrivateIPv4(const std::string& ip) {
    int parts[4];
    int idx = 0;
    int val = -1;
    for (size_t i = 0; i <= ip.size(); ++i) {
        char ch = (i < ip.size()) ? ip[i] : '.';
        if (ch == '.') {
            if (val < 0 || val > 255 || idx >= 4) { return false; }
            parts[idx++] = val;
            val = -1;
        } else if (ch >= '0' && ch <= '9') {
            val = (val < 0 ? 0 : val) * 10 + (ch - '0');
            if (val > 255) { return false; }
        } else {
            return false;
        }
    }
    if (idx != 4) { return false; }
    if (parts[0] == 10) { return true; }
    if (parts[0] == 172 && parts[1] >= 16 && parts[1] <= 31) { return true; }
    if (parts[0] == 192 && parts[1] == 168) { return true; }
    return false;
}

int pickAutoInterface(const std::vector<LocalInterface>& ifaces) {
    for (size_t i = 0; i < ifaces.size(); ++i) {
        if (!ifaces[i].ipv4.empty() && ifaces[i].physical && ifaces[i].privateIp) {
            return static_cast<int>(i);
        }
    }
    for (size_t i = 0; i < ifaces.size(); ++i) {
        if (!ifaces[i].ipv4.empty() && ifaces[i].privateIp) { return static_cast<int>(i); }
    }
    for (size_t i = 0; i < ifaces.size(); ++i) {
        if (!ifaces[i].ipv4.empty() && ifaces[i].physical) { return static_cast<int>(i); }
    }
    for (size_t i = 0; i < ifaces.size(); ++i) {
        if (!ifaces[i].ipv4.empty()) { return static_cast<int>(i); }
    }
    return -1;
}

int chooseInterface(const std::vector<LocalInterface>& ifaces, const std::string& selectedName) {
    if (!selectedName.empty()) {
        for (size_t i = 0; i < ifaces.size(); ++i) {
            if (ifaces[i].name == selectedName && !ifaces[i].ipv4.empty()) {
                return static_cast<int>(i);
            }
        }
    }
    return pickAutoInterface(ifaces);
}

static satellite::JsonOut interfaceJson(const LocalInterface& f) {
    satellite::JsonOut j;
    j["name"] = f.name;
    j["ip"] = f.ipv4;
    j["category"] = f.category;
    j["physical"] = f.physical;
    j["private"] = f.privateIp;
    return j;
}

std::string buildNetworkInfoJson(const NetworkInfo& info) {
    satellite::JsonOut j;
    j["lanIp"] = info.lanIp;
    j["device"] = info.device;
    j["category"] = info.category;
    j["selected"] = info.selected;
    j["allowPublic"] = info.allowPublic;
    j["ports"] = {{"udp", info.udpPort},
                  {"web", info.webPort},
                  {"discovery", info.discPort},
                  {"client", info.clientPort},
                  {"mdns", info.mdnsPort}};
    j["firewall"] = {{"supported", info.firewallSupported}, {"state", info.firewallState}};
    satellite::JsonOut ifaces = satellite::JsonOut::array();
    for (const auto& f : info.interfaces) ifaces.push_back(interfaceJson(f));
    j["interfaces"] = std::move(ifaces);
    return satellite::jsonDump(j);
}
