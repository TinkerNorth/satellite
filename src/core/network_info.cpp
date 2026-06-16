// SPDX-License-Identifier: LGPL-3.0-or-later
#include "core/network_info.h"

#include <string>
#include <vector>

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

static void appendInterface(std::string& out, const LocalInterface& f) {
    out += "{\"name\":\"";
    out += escapeJson(f.name);
    out += "\",\"ip\":\"";
    out += escapeJson(f.ipv4);
    out += "\",\"category\":\"";
    out += escapeJson(f.category);
    out += "\",\"physical\":";
    out += f.physical ? "true" : "false";
    out += ",\"private\":";
    out += f.privateIp ? "true" : "false";
    out += "}";
}

std::string buildNetworkInfoJson(const NetworkInfo& info) {
    std::string out = "{\"lanIp\":\"";
    out += escapeJson(info.lanIp);
    out += "\",\"device\":\"";
    out += escapeJson(info.device);
    out += "\",\"category\":\"";
    out += escapeJson(info.category);
    out += "\",\"selected\":\"";
    out += escapeJson(info.selected);
    out += "\",\"allowPublic\":";
    out += info.allowPublic ? "true" : "false";
    out += ",\"ports\":{\"udp\":";
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
    out += "},\"firewall\":{\"supported\":";
    out += info.firewallSupported ? "true" : "false";
    out += ",\"state\":\"";
    out += escapeJson(info.firewallState);
    out += "\"},\"interfaces\":[";
    for (size_t i = 0; i < info.interfaces.size(); ++i) {
        if (i != 0) { out += ","; }
        appendInterface(out, info.interfaces[i]);
    }
    out += "]}";
    return out;
}
