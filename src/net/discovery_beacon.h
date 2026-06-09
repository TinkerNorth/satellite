// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <cstdio>
#include <string>

// Pure builder for the legacy UDP discovery beacon JSON, split from the socket
// loop in discovery.cpp so the wire format is unit-testable without a socket
// (same split as net/mdns_protocol.h vs net/mdns_responder.cpp).
inline std::string buildDiscoveryBeacon(const std::string& name, int udpPort, int pairPort,
                                        int httpPort, const std::string& machineId) {
    char beacon[512];
    int n = snprintf(
        beacon, sizeof(beacon),
        R"({"service":"satellite","name":"%s","udpPort":%d,"pairPort":%d,"httpPort":%d,"machineId":"%s"})",
        name.c_str(), udpPort, pairPort, httpPort, machineId.c_str());
    if (n < 0) return std::string();
    return std::string(beacon, (size_t)(n < (int)sizeof(beacon) ? n : sizeof(beacon) - 1));
}
