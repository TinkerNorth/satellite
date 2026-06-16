// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <string>

struct NetworkInfo {
    std::string lanIp;
    std::string device;
    int udpPort = 0;
    int webPort = 0;
    int pairPort = 0;
    int discPort = 0;
    int clientPort = 0;
    int mdnsPort = 0;
};

std::string buildNetworkInfoJson(const NetworkInfo& info);
