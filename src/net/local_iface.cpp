// SPDX-License-Identifier: LGPL-3.0-or-later
#include "local_iface.h"

#include "net/net_compat.h"

#include <cstdint>
#include <string>
#include <vector>

#ifdef _WIN32
#include <iphlpapi.h>
#include <ipifcons.h>
#include <netfw.h>
#include <netlistmgr.h>
#include <oleauto.h>
#include <shellapi.h>

#include <map>
#endif

#ifndef _WIN32
#include <ifaddrs.h>
#include <net/if.h>
#endif

static std::string toLowerAscii(const std::string& s) {
    std::string r = s;
    for (char& c : r) {
        if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); }
    }
    return r;
}

static bool nameLooksVirtual(const std::string& name) {
    const std::string n = toLowerAscii(name);
    static const char* kMarkers[] = {
        "virtual",  "hyper-v",   "vethernet", "veth",      "vmware", "virtualbox",
        "vbox",     "tap",       "tun",       "wireguard", "wg",     "tailscale",
        "nordlynx", "openvpn",   "vpn",       "loopback",  "pseudo", "wsl",
        "docker",   "bluetooth", "virbr",     "vmnet",     "ppp",    "wan miniport"};
    for (const char* m : kMarkers) {
        if (n.find(m) != std::string::npos) { return true; }
    }
    return false;
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

static std::string toUpperAscii(const std::string& s) {
    std::string r = s;
    for (char& c : r) {
        if (c >= 'a' && c <= 'z') { c = static_cast<char>(c - 'a' + 'A'); }
    }
    return r;
}

static std::string guidToUpper(const GUID& g) {
    wchar_t buf[64] = {};
    if (StringFromGUID2(g, buf, 64) <= 0) { return std::string(); }
    return toUpperAscii(wideToUtf8(buf));
}

static const char* categoryName(NLM_NETWORK_CATEGORY cat) {
    switch (cat) {
    case NLM_NETWORK_CATEGORY_PUBLIC:
        return "public";
    case NLM_NETWORK_CATEGORY_PRIVATE:
        return "private";
    case NLM_NETWORK_CATEGORY_DOMAIN_AUTHENTICATED:
        return "domain";
    }
    return "unknown";
}

struct ComScope {
    bool inited = false;
    ComScope() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        inited = (hr == S_OK || hr == S_FALSE);
    }
    ~ComScope() {
        if (inited) { CoUninitialize(); }
    }
};

static std::map<std::string, std::string> nlmCategoriesByAdapter() {
    std::map<std::string, std::string> out;
    ComScope com;
    INetworkListManager* mgr = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(NetworkListManager), nullptr, CLSCTX_ALL,
                                __uuidof(INetworkListManager), reinterpret_cast<void**>(&mgr))) ||
        mgr == nullptr) {
        return out;
    }
    IEnumNetworkConnections* conns = nullptr;
    if (SUCCEEDED(mgr->GetNetworkConnections(&conns)) && conns != nullptr) {
        INetworkConnection* conn = nullptr;
        ULONG got = 0;
        while (conns->Next(1, &conn, &got) == S_OK && got == 1 && conn != nullptr) {
            GUID adapterId = {};
            INetwork* net = nullptr;
            if (SUCCEEDED(conn->GetAdapterId(&adapterId)) && SUCCEEDED(conn->GetNetwork(&net)) &&
                net != nullptr) {
                NLM_NETWORK_CATEGORY cat = NLM_NETWORK_CATEGORY_PUBLIC;
                if (SUCCEEDED(net->GetCategory(&cat))) {
                    std::string key = guidToUpper(adapterId);
                    if (!key.empty()) { out[key] = categoryName(cat); }
                }
            }
            if (net != nullptr) { net->Release(); }
            conn->Release();
            conn = nullptr;
        }
        conns->Release();
    }
    mgr->Release();
    return out;
}

// GetAdaptersAddresses' table, re-sized once when the first guess is short;
// empty when the call fails.
static std::vector<unsigned char> adapterAddresses() {
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG size = 15000;
    std::vector<unsigned char> buffer(size);
    ULONG ret = GetAdaptersAddresses(AF_INET, flags, nullptr,
                                     reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        ret = GetAdaptersAddresses(AF_INET, flags, nullptr,
                                   reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (ret != NO_ERROR) buffer.clear();
    return buffer;
}

// The adapter's first IPv4 unicast address, or empty.
static std::string firstIpv4Of(const IP_ADAPTER_ADDRESSES* a) {
    for (const IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress; u != nullptr; u = u->Next) {
        if (u->Address.lpSockaddr == nullptr || u->Address.lpSockaddr->sa_family != AF_INET) {
            continue;
        }
        const sockaddr_in* sa = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
        char buf[INET_ADDRSTRLEN] = {};
        if (inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf)) != nullptr) return buf;
    }
    return "";
}

// What the dashboard shows for one adapter. Physical means a wired or Wi-Fi
// interface type whose names do not read as a virtual one.
static LocalInterface interfaceFor(const IP_ADAPTER_ADDRESSES* a, const std::string& ip) {
    LocalInterface f;
    f.name = wideToUtf8(a->FriendlyName);
    if (f.name.empty()) f.name = (a->AdapterName != nullptr) ? a->AdapterName : "";
    f.ipv4 = ip;
    f.privateIp = isPrivateIPv4(ip);
    const bool typePhysical =
        (a->IfType == IF_TYPE_ETHERNET_CSMACD || a->IfType == IF_TYPE_IEEE80211);
    const bool looksVirt = nameLooksVirtual(f.name) || nameLooksVirtual(wideToUtf8(a->Description));
    f.physical = typePhysical && !looksVirt;
    return f;
}

std::vector<LocalInterface> enumerateInterfaces(bool withCategory) {
    std::vector<LocalInterface> result;
    std::vector<std::string> guids;
    const std::vector<unsigned char> buffer = adapterAddresses();
    if (buffer.empty()) return result;

    for (const IP_ADAPTER_ADDRESSES* a =
             reinterpret_cast<const IP_ADAPTER_ADDRESSES*>(buffer.data());
         a != nullptr; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        const std::string ip = firstIpv4Of(a);
        if (ip.empty()) continue;
        result.push_back(interfaceFor(a, ip));
        guids.push_back((a->AdapterName != nullptr) ? toUpperAscii(a->AdapterName) : std::string());
    }

    if (withCategory) {
        const std::map<std::string, std::string> cats = nlmCategoriesByAdapter();
        for (size_t i = 0; i < result.size(); ++i) {
            const auto it = cats.find(guids[i]);
            if (it != cats.end()) result[i].category = it->second;
        }
    }
    return result;
}

bool allowPublicFirewall() {
    ComScope com;
    const wchar_t* params =
        L"/c netsh advfirewall firewall set rule name=\"Satellite mDNS\" new "
        L"profile=domain,private,public"
        L" & netsh advfirewall firewall set rule name=\"Satellite UDP\" new "
        L"profile=domain,private,public"
        L" & netsh advfirewall firewall set rule name=\"Satellite Pairing\" new "
        L"profile=domain,private,public"
        L" & netsh advfirewall firewall set rule name=\"Satellite Discovery\" new "
        L"profile=domain,private,public"
        L" & netsh advfirewall firewall set rule name=\"Satellite Client TLS\" new "
        L"profile=domain,private,public";
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = L"cmd.exe";
    sei.lpParameters = params;
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) { return false; }
    if (sei.hProcess != nullptr) {
        WaitForSingleObject(sei.hProcess, 15000);
        CloseHandle(sei.hProcess);
    }
    return true;
}

static std::wstring lowerWide(const wchar_t* w) {
    std::wstring s = (w != nullptr) ? w : L"";
    for (wchar_t& c : s) {
        if (c >= L'A' && c <= L'Z') { c = static_cast<wchar_t>(c - L'A' + L'a'); }
    }
    return s;
}

// An enabled inbound allow rule for `self` folds its profiles into the mask.
static void noteSelfInboundRule(INetFwRule* rule, const std::wstring& self, int& ruleMask,
                                bool& haveRule) {
    NET_FW_RULE_DIRECTION dir = NET_FW_RULE_DIR_IN;
    NET_FW_ACTION action = NET_FW_ACTION_BLOCK;
    VARIANT_BOOL enabled = VARIANT_FALSE;
    BSTR app = nullptr;
    const bool inbound = SUCCEEDED(rule->get_Direction(&dir)) && dir == NET_FW_RULE_DIR_IN;
    const bool allows = SUCCEEDED(rule->get_Action(&action)) && action == NET_FW_ACTION_ALLOW;
    const bool on = SUCCEEDED(rule->get_Enabled(&enabled)) && enabled == VARIANT_TRUE;
    const bool named = SUCCEEDED(rule->get_ApplicationName(&app)) && app != nullptr;
    if (inbound && allows && on && named && lowerWide(app) == self) {
        haveRule = true;
        long profiles = 0;
        if (SUCCEEDED(rule->get_Profiles(&profiles))) ruleMask |= static_cast<int>(profiles);
    }
    if (app != nullptr) SysFreeString(app);
}

// Every rule the policy holds, shown to noteSelfInboundRule. False when the
// enumeration could not be reached at all, which the caller reports as
// unsupported rather than as "no rule".
static bool scanRulesForSelf(INetFwPolicy2* policy, const std::wstring& self, int& ruleMask,
                             bool& haveRule) {
    INetFwRules* rules = nullptr;
    if (FAILED(policy->get_Rules(&rules)) || rules == nullptr) return false;
    IUnknown* unk = nullptr;
    if (FAILED(rules->get__NewEnum(&unk)) || unk == nullptr) {
        rules->Release();
        return false;
    }
    IEnumVARIANT* en = nullptr;
    const bool haveEnum =
        SUCCEEDED(unk->QueryInterface(__uuidof(IEnumVARIANT), reinterpret_cast<void**>(&en))) &&
        en != nullptr;
    if (haveEnum) {
        VARIANT v;
        VariantInit(&v);
        ULONG got = 0;
        while (en->Next(1, &v, &got) == S_OK && got == 1) {
            if (v.vt == VT_DISPATCH && v.pdispVal != nullptr) {
                INetFwRule* rule = nullptr;
                if (SUCCEEDED(v.pdispVal->QueryInterface(__uuidof(INetFwRule),
                                                         reinterpret_cast<void**>(&rule))) &&
                    rule != nullptr) {
                    noteSelfInboundRule(rule, self, ruleMask, haveRule);
                    rule->Release();
                }
            }
            VariantClear(&v);
            VariantInit(&v);
            got = 0;
        }
        en->Release();
    }
    unk->Release();
    rules->Release();
    return haveEnum;
}

bool selfInboundFirewallRules(int& ruleMask, bool& haveRule) {
    ruleMask = 0;
    haveRule = false;

    wchar_t exe[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return false;
    const std::wstring self = lowerWide(exe);

    ComScope com;
    INetFwPolicy2* policy = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER,
                                __uuidof(INetFwPolicy2), reinterpret_cast<void**>(&policy))) ||
        policy == nullptr) {
        return false;
    }
    const bool queried = scanRulesForSelf(policy, self, ruleMask, haveRule);
    policy->Release();
    return queried;
}
#endif

#ifndef _WIN32
std::vector<LocalInterface> enumerateInterfaces(bool) {
    std::vector<LocalInterface> result;
    struct ifaddrs* addrs = nullptr;
    if (getifaddrs(&addrs) != 0) { return result; }
    for (struct ifaddrs* a = addrs; a != nullptr; a = a->ifa_next) {
        if (a->ifa_addr == nullptr || a->ifa_addr->sa_family != AF_INET) { continue; }
        if ((a->ifa_flags & IFF_UP) == 0 || (a->ifa_flags & IFF_LOOPBACK) != 0) { continue; }
        sockaddr_in* sa = reinterpret_cast<sockaddr_in*>(a->ifa_addr);
        char buf[INET_ADDRSTRLEN] = {};
        if (inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf)) == nullptr) { continue; }
        LocalInterface f;
        f.name = (a->ifa_name != nullptr) ? a->ifa_name : "";
        f.ipv4 = buf;
        f.privateIp = isPrivateIPv4(f.ipv4);
        f.physical = !nameLooksVirtual(f.name);
        result.push_back(f);
    }
    freeifaddrs(addrs);
    return result;
}

bool allowPublicFirewall() { return false; }

bool selfInboundFirewallRules(int&, bool&) { return false; }
#endif

bool resolveBoundIPv4(const std::string& selectedName, uint32_t& ipv4NetworkOrder) {
    std::vector<LocalInterface> ifaces = enumerateInterfaces(false);
    int idx = chooseInterface(ifaces, selectedName);
    if (idx < 0) { return false; }
    in_addr addr = {};
    if (inet_pton(AF_INET, ifaces[static_cast<size_t>(idx)].ipv4.c_str(), &addr) != 1) {
        return false;
    }
    ipv4NetworkOrder = addr.s_addr;
    return ipv4NetworkOrder != 0;
}
