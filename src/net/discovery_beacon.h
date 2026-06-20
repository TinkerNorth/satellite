// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include "core/json.h"

#include <string>

// Pure builder for the legacy UDP discovery beacon JSON, split from the socket
// loop in discovery.cpp so the wire format is unit-testable without a socket
// (same split as net/mdns_protocol.h vs net/mdns_responder.cpp).
inline std::string buildDiscoveryBeacon(const std::string& name, int udpPort, int pairPort,
                                        int httpPort, const std::string& machineId) {
    satellite::JsonOut j;
    j["service"] = "satellite";
    j["name"] = name;
    j["udpPort"] = udpPort;
    j["pairPort"] = pairPort;
    j["httpPort"] = httpPort;
    j["machineId"] = machineId;
    return satellite::jsonDump(j);
}
