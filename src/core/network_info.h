// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

struct LocalInterface {
    std::string name;
    std::string ipv4;
    std::string category;
    bool physical = false;
    bool privateIp = false;
};

struct NetworkInfo {
    std::string lanIp;
    std::string device;
    std::string category;
    std::string selected;
    bool allowPublic = false;
    int udpPort = 0;
    int webPort = 0;
    int discPort = 0;
    int clientPort = 0;
    int mdnsPort = 0;
    bool firewallSupported = false;
    std::string firewallState;
    std::vector<LocalInterface> interfaces;
};

bool isPrivateIPv4(const std::string& ip);
int pickAutoInterface(const std::vector<LocalInterface>& ifaces);
int chooseInterface(const std::vector<LocalInterface>& ifaces, const std::string& selectedName);
std::string buildNetworkInfoJson(const NetworkInfo& info);
