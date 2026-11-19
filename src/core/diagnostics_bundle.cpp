// SPDX-License-Identifier: LGPL-3.0-or-later
#include "diagnostics_bundle.h"

#include "descriptor_json.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace satellite {

namespace {

constexpr const char* HOST_KIND = "dish";

bool isForbiddenKey(const std::string& key) {
    for (const char* forbidden : DIAGNOSTICS_FORBIDDEN_KEYS) {
        if (key == forbidden) return true;
    }
    return false;
}

Json withoutForbiddenKeys(const Json& doc) {
    if (doc.is_array()) {
        Json out = Json::array();
        for (const Json& v : doc) out.push_back(withoutForbiddenKeys(v));
        return out;
    }
    if (!doc.is_object()) return doc;
    Json out = Json::object();
    for (auto it = doc.begin(); it != doc.end(); ++it) {
        if (!isForbiddenKey(it.key())) out[it.key()] = withoutForbiddenKeys(it.value());
    }
    return out;
}

void addAlias(std::vector<HostAlias>& out, const std::string& text, const std::string& alias) {
    if (!text.empty()) out.push_back({text, alias});
}

void addIdentity(std::vector<HostAlias>& out, const HostIdentity& id, const std::string& alias) {
    addAlias(out, id.deviceId, alias);
    addAlias(out, id.deviceName, alias);
    addAlias(out, id.address, alias);
}

std::string replaceAll(std::string text, const std::string& from, const std::string& to) {
    size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
    return text;
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }

// Reads one 0..255 octet of up to three digits at `pos`; the end past it.
bool readOctet(const std::string& s, size_t pos, size_t& end) {
    size_t i = pos;
    int value = 0;
    while (i < s.size() && i - pos < 3 && isDigit(s[i])) value = value * 10 + (s[i++] - '0');
    const bool digits = i > pos && !(i < s.size() && isDigit(s[i]));
    end = i;
    return digits && value <= 255;
}

// The length of a dotted-quad address starting at `pos`, or 0.
size_t ipv4At(const std::string& s, size_t pos) {
    const bool startsWord = pos == 0 || !(isDigit(s[pos - 1]) || s[pos - 1] == '.');
    if (!startsWord) return 0;
    size_t cursor = pos;
    for (int octet = 0; octet < 4; octet++) {
        size_t end = 0;
        if (!readOctet(s, cursor, end)) return 0;
        cursor = end;
        const bool last = octet == 3;
        if (last) break;
        if (cursor >= s.size() || s[cursor] != '.') return 0;
        cursor++;
    }
    const bool trailingOctet = cursor + 1 < s.size() && s[cursor] == '.' && isDigit(s[cursor + 1]);
    return trailingOctet ? 0 : cursor - pos;
}

std::string maskIpv4(const std::string& text) {
    std::string out;
    size_t i = 0;
    while (i < text.size()) {
        const size_t len = isDigit(text[i]) ? ipv4At(text, i) : 0;
        if (len > 0) {
            out += DIAGNOSTICS_MASKED_IP;
            i += len;
        } else {
            out += text[i++];
        }
    }
    return out;
}

// Howard Hinnant's days-to-civil, for a UTC date without the C library's
// thread-unsafe gmtime.
void civilFromDays(int64_t days, int64_t& y, unsigned& m, unsigned& d) {
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = static_cast<int64_t>(yoe) + era * 400 + (m <= 2 ? 1 : 0);
}

JsonOut controllerJson(const DiagnosticsController& c) {
    JsonOut j;
    j["host"] = c.host;
    j["ctrlIdx"] = c.ctrlIdx;
    j["record"] = nullptr;
    j["type"] = c.type;
    j["backend"] = c.backend;
    j["pluggedIn"] = c.pluggedIn;
    j["caps"] = capsJsonObj(c.caps);
    j["lastGameOutput"] = gameOutputJson(c.lastGameOutput, c.lastGameOutputAtMs);
    return j;
}

JsonOut hostJson(const DiagnosticsHost& h) {
    JsonOut j;
    j["alias"] = h.alias;
    j["kind"] = HOST_KIND;
    j["protocol"] = h.protocol;
    j["link"] = h.link;
    return j;
}

JsonOut logJson(const std::vector<DiagnosticsLogLine>& log, const std::vector<HostAlias>& aliases) {
    JsonOut out = JsonOut::array();
    const size_t skip = log.size() > DIAGNOSTICS_LOG_LINES ? log.size() - DIAGNOSTICS_LOG_LINES : 0;
    for (size_t i = skip; i < log.size(); i++) {
        JsonOut line;
        line["atMs"] = log[i].atMs;
        line["level"] = log[i].level;
        line["tag"] = log[i].tag;
        line["message"] = scrubDiagnosticsText(log[i].message, aliases);
        out.push_back(std::move(line));
    }
    return out;
}

} // namespace

Json redactConfigForDiagnostics(const Json& config) {
    Json out = withoutForbiddenKeys(config);
    const auto paired = config.find("pairedDevices");
    const bool listed = paired != config.end() && paired->is_array();
    out["pairedDeviceCount"] = listed ? paired->size() : 0;
    out["networkInterfaceChosen"] = !jsonStr(config, "networkInterface").empty();
    return out;
}

Json redactForDiagnostics(const Json& doc) { return withoutForbiddenKeys(doc); }

std::string hostAlias(size_t index) { return "Host " + std::to_string(index + 1); }

std::vector<HostAlias> aliasHosts(const std::vector<HostIdentity>& connected,
                                  const std::vector<HostIdentity>& pairedOnly) {
    std::vector<HostAlias> out;
    size_t next = 0;
    for (const HostIdentity& id : connected) addIdentity(out, id, hostAlias(next++));
    for (const HostIdentity& id : pairedOnly) addIdentity(out, id, hostAlias(next++));
    return out;
}

std::string scrubDiagnosticsText(const std::string& text, const std::vector<HostAlias>& aliases) {
    std::vector<HostAlias> longestFirst = aliases;
    std::stable_sort(
        longestFirst.begin(), longestFirst.end(),
        [](const HostAlias& a, const HostAlias& b) { return a.text.size() > b.text.size(); });
    std::string out = text;
    for (const HostAlias& a : longestFirst) out = replaceAll(out, a.text, a.alias);
    return maskIpv4(out);
}

std::string isoUtc(int64_t epochSeconds) {
    const int64_t days = (epochSeconds >= 0 ? epochSeconds : epochSeconds - 86399) / 86400;
    const int64_t secs = epochSeconds - days * 86400;
    int64_t y = 0;
    unsigned m = 0;
    unsigned d = 0;
    civilFromDays(days, y, m, d);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04lld-%02u-%02uT%02lld:%02lld:%02lldZ",
                  static_cast<long long>(y), m, d, static_cast<long long>(secs / 3600),
                  static_cast<long long>((secs % 3600) / 60), static_cast<long long>(secs % 60));
    return buf;
}

JsonOut controllerDbStatusJson(const ControllerDbStatus& s) {
    JsonOut j;
    j["bundledVersion"] = s.bundledVersion;
    j["activeVersion"] = s.activeVersion;
    j["source"] = dbSourceName(s.source);
    j["enabled"] = s.enabled;
    j["checking"] = s.checking;
    if (s.checked) {
        j["lastCheckAt"] = isoUtc(s.lastCheckEpoch);
        j["lastCheckResult"] = dbCheckResultName(s.lastResult);
    } else {
        j["lastCheckAt"] = nullptr;
        j["lastCheckResult"] = nullptr;
    }
    return j;
}

JsonOut gameOutputJson(GameOutputKind kind, int64_t atMs) {
    if (kind == GameOutputKind::None) return nullptr;
    JsonOut j;
    j["kind"] = gameOutputName(kind);
    j["atMs"] = atMs;
    return j;
}

JsonOut buildDiagnosticsBundle(const DiagnosticsInput& in) {
    JsonOut j;
    j["bundle"] = DIAGNOSTICS_BUNDLE_VERSION;
    j["createdAt"] = isoUtc(in.nowEpoch);
    j["app"] = {{"name", DIAGNOSTICS_APP_NAME},
                {"version", in.version},
                {"build", in.build},
                {"flavor", in.flavor}};
    j["device"] = {{"model", in.deviceModel}, {"os", in.os}, {"arch", in.arch}};
    j["controllerDb"] = controllerDbStatusJson(in.controllerDb);
    JsonOut controllers = JsonOut::array();
    for (const DiagnosticsController& c : in.controllers) controllers.push_back(controllerJson(c));
    j["controllers"] = std::move(controllers);
    JsonOut hosts = JsonOut::array();
    for (const DiagnosticsHost& h : in.hosts) hosts.push_back(hostJson(h));
    j["hosts"] = std::move(hosts);
    j["debug"] = JsonOut::parse(redactForDiagnostics(in.debug).dump());
    j["backends"] = JsonOut::parse(redactForDiagnostics(in.backends).dump());
    j["config"] = JsonOut::parse(redactConfigForDiagnostics(in.config).dump());
    j["log"] = logJson(in.log, in.aliases);
    return j;
}

} // namespace satellite
