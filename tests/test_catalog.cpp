// SPDX-License-Identifier: LGPL-3.0-or-later

// core/catalog: Accept-Language resolution, catalog JSON shape, ETag, and the
// locale completeness gate: every controller-TYPE string must resolve in every
// supported locale (mirror of Android's MissingTranslation lint), while
// feature/mode slugs stay machine-readable protocol constants.
#include "../src/core/catalog.h"
#include "../src/core/json.h"

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "test_util.h"

using namespace satellite;

static std::string readFileAll(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return "";
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static std::string langPath(const std::string& locale) {
    return std::string(WEB_LANG_DIR) + "/" + locale + ".json";
}

static std::vector<std::string> controllerTypeElems(const std::string& json,
                                                    std::vector<long>& ids) {
    std::vector<std::string> out;
    ids.clear();
    nlohmann::ordered_json p = nlohmann::ordered_json::parse(json, nullptr, /*exceptions=*/false);
    if (!p.is_discarded() && p.contains("controllerTypes") && p["controllerTypes"].is_array()) {
        for (const auto& t : p["controllerTypes"]) {
            out.push_back(t.dump());
            ids.push_back(t.value("id", -1L));
        }
    }
    return out;
}

static void test_resolveLocale_exactMatch() {
    TEST("resolveCatalogLocale: exact tags, case-insensitive");
    EXPECT_EQ(resolveCatalogLocale("en"), std::string("en"));
    EXPECT_EQ(resolveCatalogLocale("de"), std::string("de"));
    EXPECT_EQ(resolveCatalogLocale("DE"), std::string("de"));
    EXPECT_EQ(resolveCatalogLocale("pt-BR"), std::string("pt-BR"));
    EXPECT_EQ(resolveCatalogLocale("pt-br"), std::string("pt-BR"));
}

static void test_resolveLocale_primarySubtag() {
    TEST("resolveCatalogLocale: primary-subtag fallback");
    EXPECT_EQ(resolveCatalogLocale("de-AT"), std::string("de"));
    EXPECT_EQ(resolveCatalogLocale("es-MX"), std::string("es"));
    EXPECT_EQ(resolveCatalogLocale("pt"), std::string("pt-BR"));
    EXPECT_EQ(resolveCatalogLocale("pt-PT"), std::string("pt-BR"));
}

static void test_resolveLocale_qValueOrdering() {
    TEST("resolveCatalogLocale: q-values order the candidates");
    EXPECT_EQ(resolveCatalogLocale("fr;q=0.5, de;q=0.9"), std::string("de"));
    EXPECT_EQ(resolveCatalogLocale("ja, fr;q=0.8, en;q=0.1"), std::string("fr"));
    EXPECT_EQ(resolveCatalogLocale("de-DE, de;q=0.9, en;q=0.8"), std::string("de"));
}

static void test_resolveLocale_fallbackEn() {
    TEST("resolveCatalogLocale: unknown/empty/wildcard fall back to en");
    EXPECT_EQ(resolveCatalogLocale(""), std::string("en"));
    EXPECT_EQ(resolveCatalogLocale("ja"), std::string("en"));
    EXPECT_EQ(resolveCatalogLocale("*"), std::string("en"));
    EXPECT_EQ(resolveCatalogLocale("zh-CN, ko;q=0.5"), std::string("en"));
}

static void test_catalogETag_shape() {
    TEST("catalogETag: version+locale, quoted");
    EXPECT_EQ(catalogETag("1.6.0", "de"), std::string("\"1.6.0+de\""));
    EXPECT(catalogETag("1.6.0", "de") != catalogETag("1.6.0", "en"));
    EXPECT(catalogETag("1.6.0", "de") != catalogETag("1.6.1", "de"));
}

static void test_catalogJson_structure() {
    TEST("buildCatalogJson: ids match the wire enum, slugs and layers correct");
    CatalogBackendTraits traits;
    traits.ds4MotionSupported = true;
    traits.ds4MotionRequires = "vigembus>=1.17";
    traits.ds4TouchpadSupported = true;
    traits.ds4LightbarSupported = true;
    traits.mouseControlSupported = true;
    traits.rumbleSupported = true;
    traits.offersXbox = true;
    traits.offersDS4 = true;
    traits.offersDualSense = true;
    traits.offersSwitchPro = true;

    const std::string enJson = readFileAll(langPath("en"));
    EXPECT(!enJson.empty());
    std::string json = buildCatalogJson("en", enJson, enJson, "1.6.0", traits);

    // Resolved locale echoed; version present.
    EXPECT(json.find("\"locale\":\"en\"") != std::string::npos);
    EXPECT(json.find("\"serverVersion\":\"1.6.0\"") != std::string::npos);
    EXPECT(json.find("\"protocolVersion\":1") != std::string::npos);
    EXPECT(json.find("\"catalogVersion\":2") != std::string::npos);

    // Catalog ids ARE the wire enum values (0 = xbox360, 1 = ds4).
    std::vector<long> ids;
    auto types = controllerTypeElems(json, ids);
    EXPECT_EQ(types.size(), size_t{4});
    if (types.size() == 4) {
        EXPECT_EQ(ids[0], 0L);
        EXPECT(types[0].find("\"slug\":\"xbox360\"") != std::string::npos);
        EXPECT_EQ(ids[1], 1L);
        EXPECT(types[1].find("\"slug\":\"ds4\"") != std::string::npos);
        EXPECT_EQ(ids[2], 2L);
        EXPECT(types[2].find("\"slug\":\"dualsense\"") != std::string::npos);
        EXPECT_EQ(ids[3], 3L);
        EXPECT(types[3].find("\"slug\":\"switchpro\"") != std::string::npos);
        // Image hrefs are served by the satellite itself (offline-capable).
        EXPECT(types[0].find("/api/catalog/images/xbox360") != std::string::npos);
        EXPECT(types[1].find("/api/catalog/images/ds4") != std::string::npos);
        EXPECT(types[2].find("/api/catalog/images/dualsense") != std::string::npos);
        EXPECT(types[3].find("/api/catalog/images/switchpro") != std::string::npos);
        // Structured requires code on DS4 motion, never prose.
        EXPECT(types[1].find("\"requires\":\"vigembus>=1.17\"") != std::string::npos);
        // Xbox 360 has no motion/lightbar/touchpad.
        EXPECT(types[0].find("\"motion\":{\"supported\":false}") != std::string::npos);
        // The DS4 touchpad advertises its pad mode explicitly (read, not inferred).
        EXPECT(types[1].find("\"touchpad\":{\"supported\":true,\"modes\":[\"ds4\"]}") !=
               std::string::npos);
        // DualSense shares the DS4 feature surface (touchpad + pad mode).
        EXPECT(types[2].find("\"touchpad\":{\"supported\":true,\"modes\":[\"ds4\"]}") !=
               std::string::npos);
        // Switch Pro: motion, but no analog triggers and no touchpad.
        EXPECT(types[3].find("\"analogTriggers\":{\"supported\":false}") != std::string::npos);
        EXPECT(types[3].find("\"touchpad\":{\"supported\":false}") != std::string::npos);
        EXPECT(types[3].find("\"motion\":{\"supported\":true}") != std::string::npos);
        // emulates: physical-pad hint per offered type, protocol constants (ordered
        // sdlType then usb). What a future client-side matcher keys a default off.
        EXPECT(types[0].find("\"emulates\":{\"sdlType\":\"xbox360\",\"usb\":[\"045e:028e\"]}") !=
               std::string::npos);
        EXPECT(types[1].find("\"emulates\":{\"sdlType\":\"ps4\",\"usb\":[\"054c:05c4\"]}") !=
               std::string::npos);
        EXPECT(types[2].find("\"emulates\":{\"sdlType\":\"ps5\",\"usb\":[\"054c:0ce6\"]}") !=
               std::string::npos);
        EXPECT(types[3].find("\"emulates\":{\"sdlType\":\"switchpro\",\"usb\":[\"057e:2009\"]}") !=
               std::string::npos);
    }

    // hostFeatures: pure capability data + machine-readable mode slugs.
    EXPECT(json.find("\"hostFeatures\":{\"mouseControl\":{\"supported\":true") !=
           std::string::npos);
    EXPECT(json.find("\"modes\":[\"off\",\"ds4\",\"mouse\"]") != std::string::npos);
    // rumble (RECEIVE) and keyboardControl (SEND) are explicit host features now.
    EXPECT(json.find("\"rumble\":{\"supported\":true}") != std::string::npos);
    EXPECT(json.find("\"keyboardControl\":{\"supported\":false}") != std::string::npos);
}

static void test_catalogJson_inertBackend() {
    TEST("buildCatalogJson: a backend that offers nothing emits an empty type list");
    CatalogBackendTraits traits; // all false, no offers
    const std::string enJson = readFileAll(langPath("en"));
    std::string json = buildCatalogJson("en", enJson, enJson, "1.6.0", traits);
    EXPECT(json.find("\"mouseControl\":{\"supported\":false") != std::string::npos);
    // An inert backend returns no rumble either (RECEIVE host feature gated off).
    EXPECT(json.find("\"rumble\":{\"supported\":false}") != std::string::npos);
    // No offered identities → no pads to emulate advertised (never a fiction).
    std::vector<long> ids;
    auto types = controllerTypeElems(json, ids);
    EXPECT_EQ(types.size(), size_t{0});
    EXPECT(json.find("\"controllerTypes\":[]") != std::string::npos);
    EXPECT(json.find("\"keyboardControl\":{\"supported\":false}") != std::string::npos);
    // No offered types → no emulates hints anywhere (rides only offered types).
    EXPECT(json.find("\"emulates\"") == std::string::npos);
}

static void test_hostBlock_liveBackend() {
    TEST("buildHostBlockJson: live backend: supported features are also available");
    CatalogBackendTraits traits;
    traits.mouseControlSupported = true;
    traits.rumbleSupported = true;
    const std::string json = buildHostBlockJson(traits, /*backendAvailable=*/true);
    EXPECT(json.find("\"catalog\":{\"supported\":true}") != std::string::npos);
    EXPECT(json.find("\"mouseControl\":{\"supported\":true,\"available\":true}") !=
           std::string::npos);
    EXPECT(json.find("\"rumble\":{\"supported\":true,\"available\":true}") != std::string::npos);
    // keyboardControl carries no runtime backend, so no available field, supported only.
    EXPECT(json.find("\"keyboardControl\":{\"supported\":false}") != std::string::npos);
}

static void test_hostBlock_supportedButDown() {
    TEST("buildHostBlockJson: supported but backend down: present yet unavailable");
    CatalogBackendTraits traits;
    traits.mouseControlSupported = true;
    traits.rumbleSupported = true;
    const std::string json = buildHostBlockJson(traits, /*backendAvailable=*/false);
    EXPECT(json.find("\"mouseControl\":{\"supported\":true,\"available\":false}") !=
           std::string::npos);
    EXPECT(json.find("\"rumble\":{\"supported\":true,\"available\":false}") != std::string::npos);
    EXPECT(json.find("\"catalog\":{\"supported\":true}") != std::string::npos);
}

static void test_hostBlock_inertBackend() {
    TEST("buildHostBlockJson: inert backend: nothing supported, catalog still present");
    CatalogBackendTraits traits; // all false
    const std::string json = buildHostBlockJson(traits, /*backendAvailable=*/false);
    EXPECT(json.find("\"mouseControl\":{\"supported\":false,\"available\":false}") !=
           std::string::npos);
    EXPECT(json.find("\"rumble\":{\"supported\":false,\"available\":false}") != std::string::npos);
    EXPECT(json.find("\"keyboardControl\":{\"supported\":false}") != std::string::npos);
    EXPECT(json.find("\"catalog\":{\"supported\":true}") != std::string::npos);
}

static void test_catalogString_fallbackChain() {
    TEST("catalogString: locale → en → key marker");
    const std::string lang = R"({"catalog.type.ds4.name":"DualShock 4 übersetzt"})";
    const std::string en = R"({"catalog.type.ds4.name":"DualShock 4","only.en":"English"})";
    EXPECT_EQ(catalogString(lang, en, "catalog.type.ds4.name"),
              std::string("DualShock 4 übersetzt"));
    EXPECT_EQ(catalogString(lang, en, "only.en"), std::string("English"));
    EXPECT_EQ(catalogString(lang, en, "missing.key"), std::string("missing.key"));
}

static void test_resolveLocale_malformedHeaders() {
    TEST("resolveCatalogLocale: malformed Accept-Language degrades, never throws");
    EXPECT_EQ(resolveCatalogLocale(",,,"), std::string("en"));
    EXPECT_EQ(resolveCatalogLocale(";q=0.5"), std::string("en"));
    EXPECT_EQ(resolveCatalogLocale("   "), std::string("en"));
    // Garbage q-value parses as 0 but the tag itself still matches.
    EXPECT_EQ(resolveCatalogLocale("de;q=notanumber"), std::string("de"));
    // Whitespace-padded tags are trimmed; q ordering still applies.
    EXPECT_EQ(resolveCatalogLocale("  de-DE  ;  q=0.9  ,fr"), std::string("fr"));
}

static void test_catalogJson_escapesLocalizedStrings() {
    TEST("buildCatalogJson: quotes/backslashes in lang strings stay valid JSON");
    // Lang value decodes to D"S\4; the builder must re-escape it on output.
    // Ordinary escaped literals, hoisted: raw strings containing \" break
    // MSVC's traditional preprocessor when stringized in macro args.
    const std::string lang = "{\"catalog.type.ds4.name\":\"D\\\"S\\\\4\"}";
    const std::string needle = "\"name\":\"D\\\"S\\\\4\"";
    CatalogBackendTraits traits;
    traits.offersDS4 = true; // ds4 must be emitted to check its escaped name
    std::string json = buildCatalogJson("en", lang, lang, "1.6.0", traits);
    EXPECT(json.find(needle) != std::string::npos);
}

static void test_imageSlugs_matchCatalogIds() {
    TEST("catalogImageSlugs: slug order matches catalog ids (image route contract)");
    const auto& slugs = catalogImageSlugs();
    EXPECT_EQ(slugs.size(), size_t{4});
    if (slugs.size() == 4) {
        EXPECT_EQ(slugs[0], std::string("xbox360"));   // id 0
        EXPECT_EQ(slugs[1], std::string("ds4"));       // id 1
        EXPECT_EQ(slugs[2], std::string("dualsense")); // id 2
        EXPECT_EQ(slugs[3], std::string("switchpro")); // id 3
    }
}

static void test_emulatesNotLocalized() {
    TEST("buildCatalogJson: emulates hints are protocol constants, identical across locales");
    CatalogBackendTraits traits;
    traits.offersXbox = true;
    traits.offersDS4 = true;
    traits.offersDualSense = true;
    traits.offersSwitchPro = true;
    const std::string enJson = readFileAll(langPath("en"));
    const std::string needles[] = {
        "\"emulates\":{\"sdlType\":\"xbox360\",\"usb\":[\"045e:028e\"]}",
        "\"emulates\":{\"sdlType\":\"ps4\",\"usb\":[\"054c:05c4\"]}",
        "\"emulates\":{\"sdlType\":\"ps5\",\"usb\":[\"054c:0ce6\"]}",
        "\"emulates\":{\"sdlType\":\"switchpro\",\"usb\":[\"057e:2009\"]}",
    };
    for (const auto& locale : catalogLocales()) {
        const std::string langJson = readFileAll(langPath(locale));
        const std::string json = buildCatalogJson(locale, langJson, enJson, "1.6.0", traits);
        for (const auto& needle : needles) EXPECT(json.find(needle) != std::string::npos);
    }
}

static void test_emulatesOnlyOnOfferedTypes() {
    TEST("buildCatalogJson: emulates rides only offered types (ds4-only backend)");
    CatalogBackendTraits traits; // machid/none-like: ds4 only
    traits.offersDS4 = true;
    traits.ds4TouchpadSupported = true;
    const std::string enJson = readFileAll(langPath("en"));
    const std::string json = buildCatalogJson("en", enJson, enJson, "1.6.0", traits);
    EXPECT(json.find("\"emulates\":{\"sdlType\":\"ps4\",\"usb\":[\"054c:05c4\"]}") !=
           std::string::npos);
    EXPECT(json.find("\"sdlType\":\"xbox360\"") == std::string::npos);
    EXPECT(json.find("\"sdlType\":\"ps5\"") == std::string::npos);
    EXPECT(json.find("\"sdlType\":\"switchpro\"") == std::string::npos);
}

static void test_catalogJson_goldenShape_uinput() {
    TEST("buildCatalogJson: uinput all-four golden shape (empty lang = deterministic body)");
    // The real uinput trait set (routes_common.cpp): all four identities, ds4-like
    // motion/touchpad/lightbar, host mouse + rumble, no motion requires code.
    CatalogBackendTraits traits;
    traits.ds4MotionSupported = true;
    traits.ds4TouchpadSupported = true;
    traits.ds4LightbarSupported = true;
    traits.mouseControlSupported = true;
    traits.rumbleSupported = true;
    traits.offersXbox = true;
    traits.offersDS4 = true;
    traits.offersDualSense = true;
    traits.offersSwitchPro = true;
    // Empty lang → name/shortName/description resolve to their key markers, so the
    // whole body is deterministic and the full structure (order, ids, slugs, features,
    // emulates, hostFeatures) is pinned without coupling to localized copy.
    const std::string golden =
        R"({"locale":"en","protocolVersion":1,"serverVersion":"1.6.0","catalogVersion":2,"controllerTypes":[)"
        R"({"id":0,"slug":"xbox360","name":"catalog.type.xbox360.name","shortName":"catalog.type.xbox360.shortName","description":"catalog.type.xbox360.description","image":{"href":"/api/catalog/images/xbox360","etag":"\"1.6.0\""},"features":{"rumble":{"supported":true},"analogTriggers":{"supported":true},"motion":{"supported":false},"lightbar":{"supported":false},"touchpad":{"supported":false}},"emulates":{"sdlType":"xbox360","usb":["045e:028e"]}},)"
        R"({"id":1,"slug":"ds4","name":"catalog.type.ds4.name","shortName":"catalog.type.ds4.shortName","description":"catalog.type.ds4.description","image":{"href":"/api/catalog/images/ds4","etag":"\"1.6.0\""},"features":{"rumble":{"supported":true},"analogTriggers":{"supported":true},"motion":{"supported":true},"lightbar":{"supported":true},"touchpad":{"supported":true,"modes":["ds4"]}},"emulates":{"sdlType":"ps4","usb":["054c:05c4"]}},)"
        R"({"id":2,"slug":"dualsense","name":"catalog.type.dualsense.name","shortName":"catalog.type.dualsense.shortName","description":"catalog.type.dualsense.description","image":{"href":"/api/catalog/images/dualsense","etag":"\"1.6.0\""},"features":{"rumble":{"supported":true},"analogTriggers":{"supported":true},"motion":{"supported":true},"lightbar":{"supported":true},"touchpad":{"supported":true,"modes":["ds4"]}},"emulates":{"sdlType":"ps5","usb":["054c:0ce6"]}},)"
        R"({"id":3,"slug":"switchpro","name":"catalog.type.switchpro.name","shortName":"catalog.type.switchpro.shortName","description":"catalog.type.switchpro.description","image":{"href":"/api/catalog/images/switchpro","etag":"\"1.6.0\""},"features":{"rumble":{"supported":true},"analogTriggers":{"supported":false},"motion":{"supported":true},"lightbar":{"supported":false},"touchpad":{"supported":false}},"emulates":{"sdlType":"switchpro","usb":["057e:2009"]}}],)"
        R"("hostFeatures":{"mouseControl":{"supported":true,"modes":["off","ds4","mouse"]},"keyboardControl":{"supported":false},"rumble":{"supported":true}}})";
    const std::string json = buildCatalogJson("en", "", "", "1.6.0", traits);
    EXPECT_EQ(json, golden);
}

static void test_catalogVersion_present() {
    TEST("buildCatalogJson: catalogVersion (schema version) is emitted for any backend");
    const std::string enJson = readFileAll(langPath("en"));
    CatalogBackendTraits full;
    full.offersXbox = true;
    full.offersDS4 = true;
    full.offersDualSense = true;
    full.offersSwitchPro = true;
    EXPECT(buildCatalogJson("en", enJson, enJson, "1.6.0", full).find("\"catalogVersion\":2") !=
           std::string::npos);
    // Present even when the backend offers nothing: it versions the schema, not the offering.
    CatalogBackendTraits inert;
    EXPECT(buildCatalogJson("en", enJson, enJson, "1.6.0", inert).find("\"catalogVersion\":2") !=
           std::string::npos);
}

// CI completeness gate: every controller-TYPE string present in all supported
// locales. Feature slugs/modes/requires codes are NOT in the lang files; they
// are protocol constants and the structure test above pins them literal.
static void test_localeCompletenessGate() {
    TEST("locale gate: every catalog string resolves in every supported locale");
    for (const auto& locale : catalogLocales()) {
        const std::string content = readFileAll(langPath(locale));
        if (content.empty()) {
            g_fail++;
            std::cerr << "  FAIL [locale gate] missing lang file: " << langPath(locale) << "\n";
            continue;
        }
        for (const auto& key : catalogStringKeys()) {
            // catalogString falls back to the KEY ITSELF when missing; assert
            // the locale file resolves it directly (no en fallback allowed).
            const std::string v = catalogString(content, "", key);
            if (v == key) {
                g_fail++;
                std::cerr << "  FAIL [locale gate] " << locale << " missing \"" << key << "\"\n";
            } else {
                g_pass++;
            }
        }
    }
}

static void test_localizedCatalogsRenderLocalizedNames() {
    TEST("locale gate: non-English catalogs serve non-marker strings");
    const std::string enJson = readFileAll(langPath("en"));
    CatalogBackendTraits traits;
    for (const auto& locale : catalogLocales()) {
        const std::string langJson = readFileAll(langPath(locale));
        std::string json = buildCatalogJson(locale, langJson, enJson, "1.6.0", traits);
        EXPECT(json.find("\"locale\":\"" + locale + "\"") != std::string::npos);
        // The missing-string marker is the raw key; it must never surface.
        EXPECT(json.find("catalog.type.xbox360.name\"") == std::string::npos ||
               json.find("\"name\":\"catalog.type.") == std::string::npos);
    }
}

int main() {
    test_resolveLocale_exactMatch();
    test_resolveLocale_primarySubtag();
    test_resolveLocale_qValueOrdering();
    test_resolveLocale_fallbackEn();
    test_catalogETag_shape();
    test_catalogJson_structure();
    test_catalogJson_inertBackend();
    test_hostBlock_liveBackend();
    test_hostBlock_supportedButDown();
    test_hostBlock_inertBackend();
    test_catalogString_fallbackChain();
    test_resolveLocale_malformedHeaders();
    test_catalogJson_escapesLocalizedStrings();
    test_imageSlugs_matchCatalogIds();
    test_emulatesNotLocalized();
    test_emulatesOnlyOnOfferedTypes();
    test_catalogJson_goldenShape_uinput();
    test_catalogVersion_present();
    test_localeCompletenessGate();
    test_localizedCatalogsRenderLocalizedNames();

    std::cout << "catalog: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
