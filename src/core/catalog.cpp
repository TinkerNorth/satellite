// SPDX-License-Identifier: LGPL-3.0-or-later
#include "catalog.h"

#include "json.h"

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

std::string catalogStringFrom(const Json& lang, const Json& en, const std::string& key) {
    std::string v = jsonStr(lang, key.c_str());
    if (!v.empty()) return v;
    v = jsonStr(en, key.c_str());
    if (!v.empty()) return v;
    return key;
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
    Json lang, en;
    if (!jsonParse(langJson, lang)) lang = Json::object();
    if (!jsonParse(enJson, en)) en = Json::object();
    return catalogStringFrom(lang, en, key);
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

JsonOut featureJson(bool supported, const std::string& requires_ = "") {
    JsonOut j;
    j["supported"] = supported;
    if (supported && !requires_.empty()) j["requires"] = requires_;
    return j;
}

JsonOut featureJsonModes(bool supported, JsonOut modes) {
    JsonOut j;
    j["supported"] = supported;
    if (supported && !modes.empty()) j["modes"] = std::move(modes);
    return j;
}

JsonOut typeJson(int id, const std::string& slug, const Json& lang, const Json& en,
                 const std::string& serverVersion, JsonOut features) {
    const std::string base = "catalog.type." + slug + ".";
    JsonOut j;
    j["id"] = id;
    j["slug"] = slug;
    j["name"] = catalogStringFrom(lang, en, base + "name");
    j["shortName"] = catalogStringFrom(lang, en, base + "shortName");
    j["description"] = catalogStringFrom(lang, en, base + "description");
    JsonOut image;
    image["href"] = "/api/catalog/images/" + slug;
    image["etag"] = "\"" + serverVersion + "\"";
    j["image"] = std::move(image);
    j["features"] = std::move(features);
    return j;
}

} // namespace

std::string buildCatalogJson(const std::string& locale, const std::string& langJson,
                             const std::string& enJson, const std::string& serverVersion,
                             const CatalogBackendTraits& traits) {
    Json lang, en;
    if (!jsonParse(langJson, lang)) lang = Json::object();
    if (!jsonParse(enJson, en)) en = Json::object();

    // Feature slugs, modes, and requires codes below are PROTOCOL CONSTANTS —
    // never localized (docs/contract.md localization boundary rule).
    JsonOut xboxFeatures;
    xboxFeatures["rumble"] = featureJson(true);
    xboxFeatures["analogTriggers"] = featureJson(true);
    xboxFeatures["motion"] = featureJson(false);
    xboxFeatures["lightbar"] = featureJson(false);
    xboxFeatures["touchpad"] = featureJson(false);

    // The DS4 touchpad renders the "ds4" pad mode (the descriptor touchpadMode value);
    // "mouse" is host injection and lives under hostFeatures.mouseControl, not here.
    JsonOut ds4Features;
    ds4Features["rumble"] = featureJson(true);
    ds4Features["analogTriggers"] = featureJson(true);
    ds4Features["motion"] = featureJson(traits.ds4MotionSupported, traits.ds4MotionRequires);
    ds4Features["lightbar"] = featureJson(traits.ds4LightbarSupported);
    ds4Features["touchpad"] =
        featureJsonModes(traits.ds4TouchpadSupported, JsonOut::array({"ds4"}));

    JsonOut j;
    j["locale"] = locale;
    j["protocolVersion"] = 1;
    j["serverVersion"] = serverVersion;
    JsonOut types = JsonOut::array();
    types.push_back(typeJson(0, "xbox360", lang, en, serverVersion, std::move(xboxFeatures)));
    types.push_back(typeJson(1, "ds4", lang, en, serverVersion, std::move(ds4Features)));
    j["controllerTypes"] = std::move(types);

    // hostFeatures: what the HOST can be driven to do, independent of any slot. Slugs
    // and mode values are protocol constants (never localized). mouseControl.modes
    // enumerates the valid descriptor touchpadMode values; rumble (RECEIVE) and
    // keyboardControl (SEND) let the client gate those host paths instead of assuming.
    JsonOut mouseControl;
    mouseControl["supported"] = traits.mouseControlSupported;
    mouseControl["modes"] = JsonOut::array({"off", "ds4", "mouse"});
    JsonOut keyboardControl;
    keyboardControl["supported"] = traits.keyboardControlSupported;
    JsonOut rumble;
    rumble["supported"] = traits.rumbleSupported;
    JsonOut host;
    host["mouseControl"] = std::move(mouseControl);
    host["keyboardControl"] = std::move(keyboardControl);
    host["rumble"] = std::move(rumble);
    j["hostFeatures"] = std::move(host);
    return jsonDump(j);
}

std::string buildHostBlockJson(const CatalogBackendTraits& traits, bool backendAvailable) {
    // `available` is the bus-up proxy (backend can accept controllers), NOT a per-feature
    // delivery probe — enough to surface "present but driver down" pre-bind. catalog is
    // always served by this server, so its presence is unconditionally true.
    const bool mouseLive = backendAvailable && traits.mouseControlSupported;
    const bool rumbleLive = backendAvailable && traits.rumbleSupported;

    JsonOut catalog;
    catalog["supported"] = true;
    JsonOut mouseControl;
    mouseControl["supported"] = traits.mouseControlSupported;
    mouseControl["available"] = mouseLive;
    JsonOut keyboardControl;
    keyboardControl["supported"] = traits.keyboardControlSupported;
    JsonOut rumble;
    rumble["supported"] = traits.rumbleSupported;
    rumble["available"] = rumbleLive;

    JsonOut j;
    j["catalog"] = std::move(catalog);
    j["mouseControl"] = std::move(mouseControl);
    j["keyboardControl"] = std::move(keyboardControl);
    j["rumble"] = std::move(rumble);
    return jsonDump(j);
}

} // namespace satellite
