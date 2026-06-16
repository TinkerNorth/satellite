// SPDX-License-Identifier: LGPL-3.0-or-later
#include "core/network_info.h"

#include <string>

static std::string escapeJson(const std::string& s) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    for (char ch : s) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (c == '"') {
            out += "\\\"";
        } else if (c == '\\') {
            out += "\\\\";
        } else if (c == '\n') {
            out += "\\n";
        } else if (c < 0x20) {
            out += "\\u00";
            out += kHex[(c >> 4) & 0xF];
            out += kHex[c & 0xF];
        } else {
            out += ch;
        }
    }
    return out;
}

std::string buildNetworkInfoJson(const NetworkInfo& info) {
    std::string out = "{\"lanIp\":\"";
    out += escapeJson(info.lanIp);
    out += "\",\"device\":\"";
    out += escapeJson(info.device);
    out += "\",\"ports\":{\"udp\":";
    out += std::to_string(info.udpPort);
    out += ",\"web\":";
    out += std::to_string(info.webPort);
    out += ",\"pair\":";
    out += std::to_string(info.pairPort);
    out += ",\"discovery\":";
    out += std::to_string(info.discPort);
    out += ",\"client\":";
    out += std::to_string(info.clientPort);
    out += ",\"mdns\":";
    out += std::to_string(info.mdnsPort);
    out += "}}";
    return out;
}
