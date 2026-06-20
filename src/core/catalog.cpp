// SPDX-License-Identifier: LGPL-3.0-or-later
#include "catalog.h"

#include "json_mini.h"

#include <algorithm>
#include <cctype>

namespace satellite {

namespace {

std::string toLower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// Minimal JSON string-value reader for the flat web/lang files (string values
// only, standard escapes).
std::string jsonGetStringFlat(const std::string& json, const std::string& key) {
    size_t vs;
    if (!jsonValueStart(json, key, vs)) return "";
    size_t q = json.find_first_not_of(" \t\r\n", vs);
    if (q == std::string::npos || json[q] != '"') return "";
    std::string out;
    for (size_t i = q + 1; i < json.size(); i++) {
        char c = json[i];
        if (c == '\\' && i + 1 < json.size()) {
            char e = json[++i];
            switch (e) {
            case 'n':
                out += '\n';
                break;
            case 't':
                out += '\t';
                break;
            case 'r':
                out += '\r';
                break;
            case 'u':
                // Pass the escape through untouched — catalog strings are
                // re-serialized verbatim and stay valid JSON either way.
                out += "\\u";
                break;
            default:
                out += e;
                break;
            }
            continue;
        }
        if (c == '"') return out;
        out += c;
    }
    return "";
}

std::string jsonEscapeMini(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += c;
            }
        }
    }
    return out;
}

struct LangTag {
    std::string tag;
    double q;
};

} // namespace

std::string resolveCatalogLocale(const std::string& acceptLanguage) {
    const auto& locales = catalogLocales();
    if (acceptLanguage.empty()) return locales[0];

    std::vector<LangTag> tags;
    size_t pos = 0;
    while (pos < acceptLanguage.size()) {
        size_t comma = acceptLanguage.find(',', pos);
        std::string part = acceptLanguage.substr(pos, comma == std::string::npos ? std::string::npos
                                                                                 : comma - pos);
        pos = comma == std::string::npos ? acceptLanguage.size() : comma + 1;

        double q = 1.0;
        size_t semi = part.find(';');
        if (semi != std::string::npos) {
            size_t qpos = part.find("q=", semi);
            if (qpos != std::string::npos) q = strtod(part.c_str() + qpos + 2, nullptr);
            part = part.substr(0, semi);
        }
        // Trim whitespace.
        size_t b = part.find_first_not_of(" \t");
        size_t e = part.find_last_not_of(" \t");
        if (b == std::string::npos) continue;
        tags.push_back({part.substr(b, e - b + 1), q});
    }
    std::stable_sort(tags.begin(), tags.end(),
                     [](const LangTag& a, const LangTag& b) { return a.q > b.q; });

    for (const auto& t : tags) {
        const std::string lower = toLower(t.tag);
        if (lower == "*") return locales[0];
        for (const auto& loc : locales) {
            if (toLower(loc) == lower) return loc;
        }
        // Primary-subtag match: "de-AT" → "de"; also lets bare "pt" reach "pt-BR".
        const std::string primary = lower.substr(0, lower.find('-'));
        for (const auto& loc : locales) {
            const std::string locPrimary = toLower(loc.substr(0, loc.find('-')));
            if (locPrimary == primary) return loc;
        }
    }
    return locales[0];
}

std::string catalogString(const std::string& langJson, const std::string& enJson,
                          const std::string& key) {
    std::string v = jsonGetStringFlat(langJson, key);
    if (!v.empty()) return v;
    v = jsonGetStringFlat(enJson, key);
    if (!v.empty()) return v;
    return key; // visible missing-string marker; CI gate keeps this out of releases
}

const std::vector<std::string>& catalogImageSlugs() {
    static const std::vector<std::string> slugs = {"xbox360", "ds4"};
    return slugs;
}

const std::vector<std::string>& catalogStringKeys() {
    static const std::vector<std::string> keys = {
        "catalog.type.xbox360.name",        "catalog.type.xbox360.shortName",
        "catalog.type.xbox360.description", "catalog.type.ds4.name",
        "catalog.type.ds4.shortName",       "catalog.type.ds4.description",
    };
    return keys;
}

std::string catalogETag(const std::string& serverVersion, const std::string& locale) {
    return "\"" + serverVersion + "+" + locale + "\"";
}

namespace {

std::string featureJson(bool supported, const std::string& requires_ = "") {
    std::string j = std::string("{\"supported\":") + (supported ? "true" : "false");
    if (supported && !requires_.empty()) j += ",\"requires\":\"" + jsonEscapeMini(requires_) + "\"";
    j += "}";
    return j;
}

// featureJson + an explicit `modes` array (pre-built JSON of protocol-constant slugs):
// the client reads offered modes rather than inferring them from the type id.
std::string featureJsonModes(bool supported, const std::string& modesJson) {
    std::string j = std::string("{\"supported\":") + (supported ? "true" : "false");
    if (supported && !modesJson.empty()) j += ",\"modes\":" + modesJson;
    j += "}";
    return j;
}

std::string typeJson(int id, const std::string& slug, const std::string& langJson,
                     const std::string& enJson, const std::string& serverVersion,
                     const std::string& featuresJson) {
    const std::string base = "catalog.type." + slug + ".";
    std::string j = "{\"id\":" + std::to_string(id) + ",\"slug\":\"" + slug + "\"";
    j += ",\"name\":\"" + jsonEscapeMini(catalogString(langJson, enJson, base + "name")) + "\"";
    j += ",\"shortName\":\"" + jsonEscapeMini(catalogString(langJson, enJson, base + "shortName")) +
         "\"";
    j += ",\"description\":\"" +
         jsonEscapeMini(catalogString(langJson, enJson, base + "description")) + "\"";
    j += ",\"image\":{\"href\":\"/api/catalog/images/" + slug + "\",\"etag\":\"\\\"" +
         jsonEscapeMini(serverVersion) + "\\\"\"}";
    j += ",\"features\":" + featuresJson + "}";
    return j;
}

} // namespace

std::string buildCatalogJson(const std::string& locale, const std::string& langJson,
                             const std::string& enJson, const std::string& serverVersion,
                             const CatalogBackendTraits& traits) {
    // Feature slugs, modes, and requires codes below are PROTOCOL CONSTANTS —
    // never localized (docs/contract.md localization boundary rule).
    std::string xboxFeatures =
        "{\"rumble\":" + featureJson(true) + ",\"analogTriggers\":" + featureJson(true) +
        ",\"motion\":" + featureJson(false) + ",\"lightbar\":" + featureJson(false) +
        ",\"touchpad\":" + featureJson(false) + "}";
    // The DS4 touchpad renders the "ds4" pad mode (the descriptor touchpadMode value);
    // "mouse" is host injection and lives under hostFeatures.mouseControl, not here.
    std::string ds4Features =
        "{\"rumble\":" + featureJson(true) + ",\"analogTriggers\":" + featureJson(true) +
        ",\"motion\":" + featureJson(traits.ds4MotionSupported, traits.ds4MotionRequires) +
        ",\"lightbar\":" + featureJson(traits.ds4LightbarSupported) +
        ",\"touchpad\":" + featureJsonModes(traits.ds4TouchpadSupported, "[\"ds4\"]") + "}";

    std::string json = "{\"locale\":\"" + jsonEscapeMini(locale) + "\"";
    json += ",\"protocolVersion\":1";
    json += ",\"serverVersion\":\"" + jsonEscapeMini(serverVersion) + "\"";
    json += ",\"controllerTypes\":[";
    json += typeJson(0, "xbox360", langJson, enJson, serverVersion, xboxFeatures);
    json += ",";
    json += typeJson(1, "ds4", langJson, enJson, serverVersion, ds4Features);
    json += "]";
    // hostFeatures: what the HOST can be driven to do, independent of any slot. Slugs
    // and mode values are protocol constants (never localized). mouseControl.modes
    // enumerates the valid descriptor touchpadMode values; rumble (RECEIVE) and
    // keyboardControl (SEND) let the client gate those host paths instead of assuming.
    json += ",\"hostFeatures\":{\"mouseControl\":{\"supported\":";
    json += traits.mouseControlSupported ? "true" : "false";
    json += ",\"modes\":[\"off\",\"ds4\",\"mouse\"]}";
    json += ",\"keyboardControl\":{\"supported\":";
    json += traits.keyboardControlSupported ? "true" : "false";
    json += "}";
    json += ",\"rumble\":{\"supported\":";
    json += traits.rumbleSupported ? "true" : "false";
    json += "}}";
    json += "}";
    return json;
}

std::string buildHostBlockJson(const CatalogBackendTraits& traits, bool backendAvailable) {
    // `available` is the bus-up proxy (backend can accept controllers), NOT a per-feature
    // delivery probe — enough to surface "present but driver down" pre-bind. catalog is
    // always served by this server, so its presence is unconditionally true.
    const bool mouseLive = backendAvailable && traits.mouseControlSupported;
    const bool rumbleLive = backendAvailable && traits.rumbleSupported;
    std::string j = "{\"catalog\":{\"supported\":true}";
    j += ",\"mouseControl\":{\"supported\":";
    j += traits.mouseControlSupported ? "true" : "false";
    j += ",\"available\":";
    j += mouseLive ? "true" : "false";
    j += "}";
    j += ",\"keyboardControl\":{\"supported\":";
    j += traits.keyboardControlSupported ? "true" : "false";
    j += "}";
    j += ",\"rumble\":{\"supported\":";
    j += traits.rumbleSupported ? "true" : "false";
    j += ",\"available\":";
    j += rumbleLive ? "true" : "false";
    j += "}}";
    return j;
}

} // namespace satellite
