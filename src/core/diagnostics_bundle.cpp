// SPDX-License-Identifier: LGPL-3.0-or-later
#include "diagnostics_bundle.h"

#include "descriptor_json.h"
#include "hex.h"
#include "core/checked_io.h"

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

bool isDigit(char c) { return c >= '0' && c <= '9'; }

bool isNameChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }

// `name` at `pos` as a whole: where its first or last character is a letter or
// digit, the text's neighbour on that side is not one, so "e" is not found in
// "session" nor "10.0.0.1" in "10.0.0.17".
bool wholeNameAt(const std::string& text, size_t pos, const std::string& name) {
    if (text.compare(pos, name.size(), name) != 0) return false;
    const size_t end = pos + name.size();
    const bool joinsBefore = pos > 0 && isNameChar(name.front()) && isNameChar(text[pos - 1]);
    const bool joinsAfter = end < text.size() && isNameChar(name.back()) && isNameChar(text[end]);
    return !joinsBefore && !joinsAfter;
}

// The longest alias whose text stands whole at `pos` (the first listed on a
// tie), or null. `longestFirst` is sorted by text length, longest first.
const HostAlias* aliasAt(const std::string& text, size_t pos,
                         const std::vector<HostAlias>& longestFirst) {
    for (const HostAlias& a : longestFirst) {
        if (!a.text.empty() && wholeNameAt(text, pos, a.text)) return &a;
    }
    return nullptr;
}

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

bool isHex(char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }

bool isIpv6Char(char c) { return isHex(c) || c == ':' || c == '.'; }

// Groups of one to four hex digits, split on ':'; false on an empty group.
bool readHexGroups(const std::string& part, size_t& groups) {
    groups = 0;
    if (part.empty()) return true;
    size_t start = 0;
    while (true) {
        const size_t colon = part.find(':', start);
        const size_t end = colon == std::string::npos ? part.size() : colon;
        const size_t len = end - start;
        if (len == 0 || len > 4) return false;
        for (size_t i = start; i < end; i++) {
            if (!isHex(part[i])) return false;
        }
        groups++;
        if (colon == std::string::npos) return true;
        start = colon + 1;
    }
}

// RFC 4291's text forms: eight groups, or fewer around one "::", the last two
// optionally a dotted quad. A time (20:47:13) or a MAC address has no "::" and
// too few groups.
bool isIpv6(std::string candidate) {
    size_t extra = 0;
    const size_t lastColon = candidate.rfind(':');
    if (lastColon == std::string::npos) return false;
    const std::string tail = candidate.substr(lastColon + 1);
    if (tail.find('.') != std::string::npos) {
        if (ipv4At(tail, 0) != tail.size()) return false;
        candidate = candidate.substr(0, lastColon + 1) + "0";
        extra = 1;
    }
    if (candidate.find('.') != std::string::npos) return false;
    const size_t gap = candidate.find("::");
    const bool compressed = gap != std::string::npos;
    if (compressed && candidate.find("::", gap + 1) != std::string::npos) return false;
    size_t left = 0;
    size_t right = 0;
    if (compressed) {
        const bool sides = readHexGroups(candidate.substr(0, gap), left) &&
                           readHexGroups(candidate.substr(gap + 2), right);
        return sides && left + right + extra <= 7;
    }
    return readHexGroups(candidate, left) && left + extra == 8;
}

// The length of an IPv6 address (with any "%zone") starting at `pos`, or 0.
size_t ipv6At(const std::string& s, size_t pos) {
    const bool startsWord = pos == 0 || !(isNameChar(s[pos - 1]) || isIpv6Char(s[pos - 1]));
    if (!startsWord) return 0;
    size_t end = pos;
    while (end < s.size() && isIpv6Char(s[end])) end++;
    while (end > pos && s[end - 1] == '.') end--;
    if (!isIpv6(s.substr(pos, end - pos))) return 0;
    const bool zoned = end + 1 < s.size() && s[end] == '%' && isNameChar(s[end + 1]);
    if (zoned) {
        end++;
        while (end < s.size() && isNameChar(s[end])) end++;
    }
    const bool endsWord = end == s.size() || !isNameChar(s[end]);
    return endsWord ? end - pos : 0;
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

constexpr const char* MIC_LED_STATE_NAMES[MIC_LED_STATE_COUNT] = {"off", "on", "pulse"};

const char* micLedStateName(uint8_t state) {
    return state < MIC_LED_STATE_COUNT ? MIC_LED_STATE_NAMES[state] : "unknown";
}

JsonOut laneJson(const OutputLane& lane, JsonOut last) {
    JsonOut j;
    j["count"] = lane.count;
    if (lane.count == 0) {
        j["last"] = nullptr;
        return j;
    }
    last["atMs"] = lane.atMs;
    j["last"] = std::move(last);
    return j;
}

JsonOut rumbleLast(const GameOutputRecord& r) {
    JsonOut j;
    j["strong"] = r.rumbleStrong;
    j["weak"] = r.rumbleWeak;
    return j;
}

JsonOut lightbarLast(const GameOutputRecord& r) {
    JsonOut j;
    j["r"] = r.lightbarR;
    j["g"] = r.lightbarG;
    j["b"] = r.lightbarB;
    return j;
}

JsonOut triggerEffectsLast(const GameOutputRecord& r) {
    JsonOut j;
    j["left"] = hexEncode(r.triggers.left, TRIGGER_EFFECT_BLOCK_BYTES);
    j["right"] = hexEncode(r.triggers.right, TRIGGER_EFFECT_BLOCK_BYTES);
    return j;
}

JsonOut playerLedsLast(const GameOutputRecord& r) {
    JsonOut j;
    j["mask"] = hexEncode(&r.playerLedsMask, 1);
    return j;
}

JsonOut micLedLast(const GameOutputRecord& r) {
    JsonOut j;
    j["state"] = micLedStateName(r.micLedState);
    return j;
}

JsonOut controllerJson(const DiagnosticsController& c) {
    JsonOut j;
    j["host"] = c.host;
    j["ctrlIdx"] = c.ctrlIdx;
    j["record"] = nullptr;
    j["type"] = c.type;
    j["backend"] = c.backend.empty() ? JsonOut(nullptr) : JsonOut(c.backend);
    j["pluggedIn"] = c.pluggedIn;
    j["caps"] = capsJsonObj(c.caps);
    j["lastGameOutput"] = gameOutputJson(c.lastGameOutput, c.lastGameOutputAtMs);
    j["input"] = controllerInputJson(c.input);
    j["output"] = controllerOutputJson(c.output);
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

// The alias an identity already took by its device id, or a fresh one.
static std::string aliasFor(const std::vector<HostAlias>& taken, const HostIdentity& id,
                            size_t& next) {
    const auto same = std::find_if(taken.begin(), taken.end(), [&id](const HostAlias& a) {
        return !id.deviceId.empty() && a.text == id.deviceId;
    });
    if (same != taken.end()) return same->alias;
    return hostAlias(next++);
}

std::vector<HostAlias> aliasHosts(const std::vector<HostIdentity>& connected,
                                  const std::vector<HostIdentity>& pairedOnly) {
    std::vector<HostAlias> out;
    size_t next = 0;
    for (const HostIdentity& id : connected) addIdentity(out, id, aliasFor(out, id, next));
    for (const HostIdentity& id : pairedOnly) addIdentity(out, id, aliasFor(out, id, next));
    return out;
}

void aliasSecret(std::vector<HostAlias>& aliases, const std::string& secret) {
    addAlias(aliases, secret, DIAGNOSTICS_REDACTED);
}

void aliasMachine(std::vector<HostAlias>& aliases, const std::vector<std::string>& names) {
    for (const std::string& name : names) addAlias(aliases, name, DIAGNOSTICS_THIS_PC);
}

std::string scrubDiagnosticsText(const std::string& text, const std::vector<HostAlias>& aliases) {
    std::vector<HostAlias> longestFirst = aliases;
    std::stable_sort(
        longestFirst.begin(), longestFirst.end(),
        [](const HostAlias& a, const HostAlias& b) { return a.text.size() > b.text.size(); });
    // One pass over the original text: what was replaced is never read again.
    std::string out;
    size_t i = 0;
    while (i < text.size()) {
        const HostAlias* alias = aliasAt(text, i, longestFirst);
        if (alias != nullptr) {
            out += alias->alias;
            i += alias->text.size();
            continue;
        }
        const size_t ip6Len = isIpv6Char(text[i]) ? ipv6At(text, i) : 0;
        if (ip6Len > 0) {
            out += DIAGNOSTICS_MASKED_IPV6;
            i += ip6Len;
            continue;
        }
        const size_t ipLen = isDigit(text[i]) ? ipv4At(text, i) : 0;
        if (ipLen > 0) {
            out += DIAGNOSTICS_MASKED_IP;
            i += ipLen;
            continue;
        }
        out += text[i++];
    }
    return out;
}

std::string isoUtc(int64_t epochSeconds) {
    const int64_t days = (epochSeconds >= 0 ? epochSeconds : epochSeconds - 86399) / 86400;
    const int64_t secs = epochSeconds - days * 86400;
    int64_t y = 0;
    unsigned m = 0;
    unsigned d = 0;
    civilFromDays(days, y, m, d);
    char buf[128];
    const int written = std::snprintf(
        buf, sizeof(buf), "%04lld-%02u-%02uT%02lld:%02lld:%02lldZ", static_cast<long long>(y), m, d,
        static_cast<long long>(secs / 3600), static_cast<long long>((secs % 3600) / 60),
        static_cast<long long>(secs % 60));
    // A stamp that did not format is no stamp, not a cut one.
    return satellite::io::formatted(written, buf) ? std::string(buf) : std::string();
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

JsonOut controllerInputJson(const InputFacts& input) {
    JsonOut j;
    j["frames"] = input.frames;
    j["rateHz"] = input.rateHz;
    if (input.lastAtMs == 0) {
        j["lastAtMs"] = nullptr;
    } else {
        j["lastAtMs"] = input.lastAtMs;
    }
    return j;
}

JsonOut controllerOutputJson(const GameOutputRecord& output) {
    JsonOut j;
    j["rumble"] = laneJson(output.rumble, rumbleLast(output));
    j["lightbar"] = laneJson(output.lightbar, lightbarLast(output));
    j["triggerEffects"] = laneJson(output.triggerEffects, triggerEffectsLast(output));
    j["playerLeds"] = laneJson(output.playerLeds, playerLedsLast(output));
    j["micLed"] = laneJson(output.micLed, micLedLast(output));
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
