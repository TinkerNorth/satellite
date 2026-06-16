// SPDX-License-Identifier: LGPL-3.0-or-later
#include "local_iface.h"

#include "net/net_compat.h"

#include <cstdint>
#include <string>

#ifdef _WIN32
#include <iphlpapi.h>
#include <vector>
#endif

static bool localIPv4(uint32_t& ipv4NetworkOrder) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) { return false; }
    sockaddr_in remote{};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(53);
    inet_pton(AF_INET, "203.0.113.1", &remote.sin_addr);

    bool ok = false;
    if (connect(s, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) == 0) {
        sockaddr_in local{};
        socklen_t len = sizeof(local);
        if (getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0) {
            ipv4NetworkOrder = local.sin_addr.s_addr;
            ok = ipv4NetworkOrder != 0;
        }
    }
    closesocket(s);
    return ok;
}

#ifdef _WIN32
static std::string wideToUtf8(const wchar_t* w) {
    if (w == nullptr) { return std::string(); }
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) { return std::string(); }
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], n, nullptr, nullptr);
    out.resize(static_cast<size_t>(n - 1));
    return out;
}

static std::string deviceNameForIPv4(uint32_t ipv4NetworkOrder) {
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG size = 15000;
    std::vector<unsigned char> buffer(size);
    ULONG ret = GetAdaptersAddresses(AF_INET, flags, nullptr,
                                     reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        ret = GetAdaptersAddresses(AF_INET, flags, nullptr,
                                   reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (ret != NO_ERROR) { return std::string(); }
    for (IP_ADAPTER_ADDRESSES* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
         a != nullptr; a = a->Next) {
        for (IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress; u != nullptr; u = u->Next) {
            if (u->Address.lpSockaddr == nullptr || u->Address.lpSockaddr->sa_family != AF_INET) {
                continue;
            }
            sockaddr_in* sa = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
            if (sa->sin_addr.s_addr == ipv4NetworkOrder) { return wideToUtf8(a->FriendlyName); }
        }
    }
    return std::string();
}
#else
static std::string deviceNameForIPv4(uint32_t) { return std::string(); }
#endif

bool resolveLocalInterface(std::string& ipOut, std::string& deviceOut) {
    ipOut.clear();
    deviceOut.clear();
    if (!netInit()) { return false; }
    uint32_t ipv4 = 0;
    bool haveIp = localIPv4(ipv4);
    if (haveIp) {
        char buf[INET_ADDRSTRLEN] = {};
        in_addr ia;
        ia.s_addr = ipv4;
        if (inet_ntop(AF_INET, &ia, buf, sizeof(buf)) != nullptr) { ipOut = buf; }
        deviceOut = deviceNameForIPv4(ipv4);
    }
    netShutdown();
    return haveIp;
}
