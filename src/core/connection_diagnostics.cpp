// SPDX-License-Identifier: LGPL-3.0-or-later
#include "connection_diagnostics.h"

#include "descriptor_json.h"

#include <algorithm>
#include <cctype>

namespace satellite {

namespace {

constexpr const char* SERIAL_WORD = "serial ";

char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

bool isDigit(char c) { return c >= '0' && c <= '9'; }

bool isWordChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }

bool equalsIgnoringCaseAt(const std::string& text, size_t pos, const std::string& word) {
    if (pos + word.size() > text.size()) return false;
    for (size_t i = 0; i < word.size(); i++) {
        if (lower(text[pos + i]) != lower(word[i])) return false;
    }
    return true;
}

// "Serial 3" or "serial 3" as a whole word: not "Serial 31", not "xserial 3".
bool namesSerial(const std::string& text, uint32_t serial) {
    const std::string word = std::string(SERIAL_WORD) + std::to_string(serial);
    for (size_t pos = 0; pos + word.size() <= text.size(); pos++) {
        if (!equalsIgnoringCaseAt(text, pos, word)) continue;
        const size_t end = pos + word.size();
        const bool startsWord = pos == 0 || !isWordChar(text[pos - 1]);
        const bool endsNumber = end == text.size() || !isDigit(text[end]);
        if (startsWord && endsNumber) return true;
    }
    return false;
}

// An id runs on through letters, digits, '-' and '_' ("conn_ab12cd34",
// "dish-self-41"), so a token is matched only where neither neighbour is one.
bool isIdChar(char c) { return isWordChar(c) || c == '-' || c == '_'; }

bool namesToken(const std::string& text, const std::string& token) {
    for (size_t pos = text.find(token); pos != std::string::npos; pos = text.find(token, pos + 1)) {
        const size_t end = pos + token.size();
        const bool starts = pos == 0 || !isIdChar(text[pos - 1]);
        const bool ends = end == text.size() || !isIdChar(text[end]);
        if (starts && ends) return true;
    }
    return false;
}

bool namesAnyToken(const std::string& text, const std::vector<std::string>& tokens) {
    return std::any_of(tokens.begin(), tokens.end(),
                       [&text](const std::string& token) { return namesToken(text, token); });
}

bool namesClaimedSerial(const DiagnosticsLogLine& line, const std::vector<SerialClaim>& claims) {
    return std::any_of(claims.begin(), claims.end(), [&line](const SerialClaim& claim) {
        return line.atMs >= claim.sinceMs && namesSerial(line.message, claim.serial);
    });
}

void addText(std::vector<std::string>& out, const std::string& text) {
    if (!text.empty()) out.push_back(text);
}

void addAliasText(std::vector<HostAlias>& out, const std::string& text, const char* alias) {
    if (!text.empty()) out.push_back({text, alias});
}

void addIdentityAs(std::vector<HostAlias>& out, const HostIdentity& id, const char* alias) {
    addAliasText(out, id.deviceId, alias);
    addAliasText(out, id.deviceName, alias);
    addAliasText(out, id.address, alias);
}

JsonOut nullableText(const std::string& text) {
    if (text.empty()) return nullptr;
    return text;
}

JsonOut satelliteJson(const ConnectionDiagnosticsInput& in) {
    JsonOut j;
    j["version"] = in.version;
    j["platform"] = in.platform;
    j["protocol"] = JsonOut::array({in.protocolMin, in.protocolMax});
    j["uptimeS"] = in.uptimeS;
    return j;
}

JsonOut backendJson(const ConnectionBackend& b) {
    JsonOut j;
    j["id"] = b.id;
    j["available"] = b.available;
    j["driver"] = nullableText(b.driver);
    j["bundled"] = nullableText(b.bundled);
    j["preferred"] = b.preferred;
    j["lifecycle"] = nullableText(b.lifecycle);
    return j;
}

JsonOut controllerJson(const ConnectionController& c) {
    JsonOut j;
    j["ctrlIdx"] = c.ctrlIdx;
    j["requestedType"] = controllerTypeName(c.requestedType);
    j["appliedType"] = controllerTypeName(c.appliedType);
    j["backend"] = nullableText(c.backend);
    j["composite"] = c.composite;
    j["pluggedIn"] = c.pluggedIn;
    j["caps"] = capsJsonObj(c.caps);
    j["touchpadMode"] = touchpadModeName(c.touchpadMode);
    j["motion"] = {{"sinkSupportedForType", c.motionSinkSupportedForType},
                   {"backendOk", c.motionBackendOk}};
    j["input"] = controllerInputJson(c.input);
    j["output"] = controllerOutputJson(c.output);
    j["lastGameOutput"] = gameOutputJson(c.lastGameOutput, c.lastGameOutputAtMs);
    return j;
}

JsonOut redactedJson(const Json& doc) { return JsonOut::parse(redactForDiagnostics(doc).dump()); }

JsonOut countersJson(const Json& counters) {
    JsonOut j = counters.is_object() ? redactedJson(counters) : JsonOut::object();
    j["scope"] = CONNECTION_COUNTERS_SCOPE;
    return j;
}

JsonOut grantsJson(const Json& controllerAudio) {
    JsonOut j;
    j["controllerAudio"] = controllerAudio.is_object() ? redactedJson(controllerAudio) : nullptr;
    return j;
}

JsonOut logJson(const ConnectionDiagnosticsInput& in) {
    const ConnectionLogScope scope = connectionLogScope(in);
    std::vector<const DiagnosticsLogLine*> about;
    for (const DiagnosticsLogLine& line : in.log) {
        if (logLineConcernsConnection(line, scope)) about.push_back(&line);
    }
    const size_t skip = about.size() > CONNECTION_DIAGNOSTICS_LOG_LINES
                            ? about.size() - CONNECTION_DIAGNOSTICS_LOG_LINES
                            : 0;
    const std::vector<HostAlias> aliases = connectionAliases(in);
    JsonOut out = JsonOut::array();
    for (size_t i = skip; i < about.size(); i++) {
        JsonOut line;
        line["atMs"] = about[i]->atMs;
        line["level"] = about[i]->level;
        line["tag"] = about[i]->tag;
        line["message"] = scrubDiagnosticsText(about[i]->message, aliases);
        out.push_back(std::move(line));
    }
    return out;
}

} // namespace

bool logLineConcernsConnection(const DiagnosticsLogLine& line, const ConnectionLogScope& scope) {
    const bool namesConnection = namesAnyToken(line.message, scope.texts);
    return namesConnection || namesClaimedSerial(line, scope.serials);
}

ConnectionLogScope connectionLogScope(const ConnectionDiagnosticsInput& in) {
    ConnectionLogScope scope;
    addText(scope.texts, in.connectionId);
    addText(scope.texts, in.self.deviceId);
    for (const ConnectionController& c : in.controllers) {
        if (c.serial != 0) scope.serials.push_back({c.serial, c.pluggedAtMs});
    }
    return scope;
}

std::vector<HostAlias> connectionAliases(const ConnectionDiagnosticsInput& in) {
    std::vector<HostAlias> out;
    addIdentityAs(out, in.self, DIAGNOSTICS_THIS_CONNECTION);
    for (const HostIdentity& other : in.others) {
        addIdentityAs(out, other, DIAGNOSTICS_ANOTHER_CLIENT);
    }
    aliasMachine(out, in.machineNames);
    for (const std::string& secret : in.secrets) addAliasText(out, secret, DIAGNOSTICS_REDACTED);
    return out;
}

JsonOut buildConnectionDiagnostics(const ConnectionDiagnosticsInput& in) {
    JsonOut j;
    j["format"] = CONNECTION_DIAGNOSTICS_FORMAT;
    j["satellite"] = satelliteJson(in);
    JsonOut backends = JsonOut::array();
    for (const ConnectionBackend& b : in.backends) backends.push_back(backendJson(b));
    j["backends"] = std::move(backends);
    JsonOut controllers = JsonOut::array();
    for (const ConnectionController& c : in.controllers) controllers.push_back(controllerJson(c));
    j["controllers"] = std::move(controllers);
    j["grants"] = grantsJson(in.grants);
    j["counters"] = countersJson(in.counters);
    j["controllerDb"] = controllerDbStatusJson(in.controllerDb);
    j["log"] = logJson(in);
    return j;
}

} // namespace satellite
