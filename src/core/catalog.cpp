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
        // Primary-subtag match: "de-AT" to "de"; also lets bare "pt" reach "pt-BR".
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
    static const std::vector<std::string> slugs = {"xbox360", "ds4", "dualsense", "switchpro"};
    return slugs;
}

const std::vector<std::string>& catalogStringKeys() {
    static const std::vector<std::string> keys = {
        "catalog.type.xbox360.name",          "catalog.type.xbox360.shortName",
        "catalog.type.xbox360.description",   "catalog.type.ds4.name",
        "catalog.type.ds4.shortName",         "catalog.type.ds4.description",
        "catalog.type.dualsense.name",        "catalog.type.dualsense.shortName",
        "catalog.type.dualsense.description",  "catalog.type.switchpro.name",
        "catalog.type.switchpro.shortName",   "catalog.type.switchpro.description",
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

// Physical-pad identity this virtual type is the natural default for. Carried
// for a FUTURE client-side matcher: the physical->virtual mapping policy lives
// here on the host, not in each client's switch. Current clients ignore it and
// default to the first offered type. Protocol constants, never localized;
// sdlType mirrors the clients' SDL_GameControllerType vocabulary, usb is
// lowercase "vid:pid" (array admits more hardware revisions without a bump).
JsonOut emulatesJson(const std::string& slug) {
    JsonOut j;
    if (slug == "xbox360") {
        j["sdlType"] = "xbox360";
        j["usb"] = JsonOut::array({"045e:028e"});
    } else if (slug == "ds4") {
        j["sdlType"] = "ps4";
        j["usb"] = JsonOut::array({"054c:05c4"});
    } else if (slug == "dualsense") {
        j["sdlType"] = "ps5";
        j["usb"] = JsonOut::array({"054c:0ce6"});
    } else if (slug == "switchpro") {
        j["sdlType"] = "switchpro";
        j["usb"] = JsonOut::array({"057e:2009"});
    }
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
    // Only offered types are built here, so emulates rides only offered types.
    JsonOut emulates = emulatesJson(slug);
    if (!emulates.empty()) j["emulates"] = std::move(emulates);
    return j;
}

} // namespace

std::string buildCatalogJson(const std::string& locale, const std::string& langJson,
                             const std::string& enJson, const std::string& serverVersion,
                             const CatalogBackendTraits& traits) {
    Json lang, en;
    if (!jsonParse(langJson, lang)) lang = Json::object();
    if (!jsonParse(enJson, en)) en = Json::object();

    // Feature slugs, modes, and requires codes below are PROTOCOL CONSTANTS,
    // never localized (docs/contract.md localization boundary rule). DualSense
    // shares the DS4 feature surface; its touchpad renders the "ds4" pad mode.
    auto ds4LikeFeatures = [&]() {
        JsonOut f;
        f["rumble"] = featureJson(true);
        f["analogTriggers"] = featureJson(true);
        f["motion"] = featureJson(traits.ds4MotionSupported, traits.ds4MotionRequires);
        f["lightbar"] = featureJson(traits.ds4LightbarSupported);
        f["touchpad"] = featureJsonModes(traits.ds4TouchpadSupported, JsonOut::array({"ds4"}));
        return f;
    };

    JsonOut j;
    j["locale"] = locale;
    j["protocolVersion"] = 1;
    j["serverVersion"] = serverVersion;
    // Only the types this backend can materialize: a backend that can't host a
    // native identity omits it rather than advertise an unbuildable pad.
    JsonOut types = JsonOut::array();
    if (traits.offersXbox) {
        JsonOut xbox;
        xbox["rumble"] = featureJson(true);
        xbox["analogTriggers"] = featureJson(true);
        xbox["motion"] = featureJson(false);
        xbox["lightbar"] = featureJson(false);
        xbox["touchpad"] = featureJson(false);
        types.push_back(typeJson(0, "xbox360", lang, en, serverVersion, std::move(xbox)));
    }
    if (traits.offersDS4) {
        types.push_back(typeJson(1, "ds4", lang, en, serverVersion, ds4LikeFeatures()));
    }
    if (traits.offersDualSense) {
        types.push_back(typeJson(2, "dualsense", lang, en, serverVersion, ds4LikeFeatures()));
    }
    if (traits.offersSwitchPro) {
        // Switch Pro: motion, no analog triggers, no touchpad, no light bar.
        JsonOut sw;
        sw["rumble"] = featureJson(true);
        sw["analogTriggers"] = featureJson(false);
        sw["motion"] = featureJson(traits.ds4MotionSupported);
        sw["lightbar"] = featureJson(false);
        sw["touchpad"] = featureJson(false);
        types.push_back(typeJson(3, "switchpro", lang, en, serverVersion, std::move(sw)));
    }
    j["controllerTypes"] = std::move(types);

    // What the HOST can be driven to do, independent of any slot. Slugs and mode
    // values are protocol constants. mouseControl.modes enumerates the valid
    // descriptor touchpadMode values; rumble (RECEIVE) and keyboardControl (SEND)
    // let the client gate those host paths.
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
    // `available` is the bus-up proxy, NOT a per-feature delivery probe; enough
    // to surface "present but driver down" pre-bind. catalog is always served, so
    // its presence is unconditionally true.
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
