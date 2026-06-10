// SPDX-License-Identifier: LGPL-3.0-or-later

// core/catalog — Accept-Language resolution, catalog JSON shape, ETag, and the
// locale completeness gate: every controller-TYPE string must resolve in every
// supported locale (mirror of Android's MissingTranslation lint), while
// feature/mode slugs stay machine-readable protocol constants.
#include "../src/core/catalog.h"
#include "../src/core/json_mini.h"

#include <fstream>
#include <iostream>
#include <string>

static int g_pass = 0;
static int g_fail = 0;
static std::string g_currentTest;

#define TEST(name)                                                                                 \
    do { g_currentTest = (name); } while (0)

#define EXPECT(cond)                                                                               \
    do {                                                                                           \
        if (cond) {                                                                                \
            g_pass++;                                                                              \
        } else {                                                                                   \
            g_fail++;                                                                              \
            std::cerr << "  FAIL [" << g_currentTest << "] " << __FILE__ << ":" << __LINE__        \
                      << "  " << #cond << "\n";                                                    \
        }                                                                                          \
    } while (0)

#define EXPECT_EQ(a, b)                                                                            \
    do {                                                                                           \
        auto _a = (a);                                                                             \
        auto _b = (b);                                                                             \
        if (_a == _b) {                                                                            \
            g_pass++;                                                                              \
        } else {                                                                                   \
            g_fail++;                                                                              \
            std::cerr << "  FAIL [" << g_currentTest << "] " << __FILE__ << ":" << __LINE__        \
                      << "  " << #a << " == " << #b << "  (got " << _a << " vs " << _b << ")\n";   \
        }                                                                                          \
    } while (0)

using namespace satellite;

static std::string readFileAll(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return "";
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static std::string langPath(const std::string& locale) {
    return std::string(WEB_LANG_DIR) + "/" + locale + ".json";
}

static void test_resolveLocale_exactMatch() {
    TEST("resolveCatalogLocale — exact tags, case-insensitive");
    EXPECT_EQ(resolveCatalogLocale("en"), std::string("en"));
    EXPECT_EQ(resolveCatalogLocale("de"), std::string("de"));
    EXPECT_EQ(resolveCatalogLocale("DE"), std::string("de"));
    EXPECT_EQ(resolveCatalogLocale("pt-BR"), std::string("pt-BR"));
    EXPECT_EQ(resolveCatalogLocale("pt-br"), std::string("pt-BR"));
}

static void test_resolveLocale_primarySubtag() {
    TEST("resolveCatalogLocale — primary-subtag fallback");
    EXPECT_EQ(resolveCatalogLocale("de-AT"), std::string("de"));
    EXPECT_EQ(resolveCatalogLocale("es-MX"), std::string("es"));
    EXPECT_EQ(resolveCatalogLocale("pt"), std::string("pt-BR"));
    EXPECT_EQ(resolveCatalogLocale("pt-PT"), std::string("pt-BR"));
}

static void test_resolveLocale_qValueOrdering() {
    TEST("resolveCatalogLocale — q-values order the candidates");
    EXPECT_EQ(resolveCatalogLocale("fr;q=0.5, de;q=0.9"), std::string("de"));
    EXPECT_EQ(resolveCatalogLocale("ja, fr;q=0.8, en;q=0.1"), std::string("fr"));
    EXPECT_EQ(resolveCatalogLocale("de-DE, de;q=0.9, en;q=0.8"), std::string("de"));
}

static void test_resolveLocale_fallbackEn() {
    TEST("resolveCatalogLocale — unknown/empty/wildcard fall back to en");
    EXPECT_EQ(resolveCatalogLocale(""), std::string("en"));
    EXPECT_EQ(resolveCatalogLocale("ja"), std::string("en"));
    EXPECT_EQ(resolveCatalogLocale("*"), std::string("en"));
    EXPECT_EQ(resolveCatalogLocale("zh-CN, ko;q=0.5"), std::string("en"));
}

static void test_catalogETag_shape() {
    TEST("catalogETag — version+locale, quoted");
    EXPECT_EQ(catalogETag("1.6.0", "de"), std::string("\"1.6.0+de\""));
    EXPECT(catalogETag("1.6.0", "de") != catalogETag("1.6.0", "en"));
    EXPECT(catalogETag("1.6.0", "de") != catalogETag("1.6.1", "de"));
}

static void test_catalogJson_structure() {
    TEST("buildCatalogJson — ids match the wire enum, slugs and layers correct");
    CatalogBackendTraits traits;
    traits.ds4MotionSupported = true;
    traits.ds4MotionRequires = "vigembus>=1.17";
    traits.ds4TouchpadSupported = true;
    traits.ds4LightbarSupported = true;
    traits.mouseControlSupported = true;

    const std::string enJson = readFileAll(langPath("en"));
    EXPECT(!enJson.empty());
    std::string json = buildCatalogJson("en", enJson, enJson, "1.6.0", traits);

    // Resolved locale echoed; version present.
    EXPECT(json.find("\"locale\":\"en\"") != std::string::npos);
    EXPECT(json.find("\"serverVersion\":\"1.6.0\"") != std::string::npos);
    EXPECT(json.find("\"protocolVersion\":1") != std::string::npos);

    // Catalog ids ARE the wire enum values (0 = xbox360, 1 = ds4).
    auto types = jsonGetArrayObjects(json, "controllerTypes");
    EXPECT_EQ(types.size(), size_t{2});
    if (types.size() == 2) {
        long id = -1;
        EXPECT(jsonGetIntKeyed(types[0], "id", &id));
        EXPECT_EQ(id, 0L);
        EXPECT(types[0].find("\"slug\":\"xbox360\"") != std::string::npos);
        EXPECT(jsonGetIntKeyed(types[1], "id", &id));
        EXPECT_EQ(id, 1L);
        EXPECT(types[1].find("\"slug\":\"ds4\"") != std::string::npos);
        // Image hrefs are served by the satellite itself (offline-capable).
        EXPECT(types[0].find("/api/catalog/images/xbox360") != std::string::npos);
        EXPECT(types[1].find("/api/catalog/images/ds4") != std::string::npos);
        // Structured requires code on DS4 motion, never prose.
        EXPECT(types[1].find("\"requires\":\"vigembus>=1.17\"") != std::string::npos);
        // Xbox 360 has no motion/lightbar/touchpad.
        EXPECT(types[0].find("\"motion\":{\"supported\":false}") != std::string::npos);
    }

    // hostFeatures: pure capability data + machine-readable mode slugs.
    EXPECT(json.find("\"hostFeatures\":{\"mouseControl\":{\"supported\":true") !=
           std::string::npos);
    EXPECT(json.find("\"modes\":[\"off\",\"ds4\",\"mouse\"]") != std::string::npos);
}

static void test_catalogJson_inertBackend() {
    TEST("buildCatalogJson — inert backend (macOS): nothing supported, no requires");
    CatalogBackendTraits traits; // all false
    const std::string enJson = readFileAll(langPath("en"));
    std::string json = buildCatalogJson("en", enJson, enJson, "1.6.0", traits);
    EXPECT(json.find("\"mouseControl\":{\"supported\":false") != std::string::npos);
    auto types = jsonGetArrayObjects(json, "controllerTypes");
    EXPECT_EQ(types.size(), size_t{2});
    if (types.size() == 2) {
        EXPECT(types[1].find("\"motion\":{\"supported\":false}") != std::string::npos);
        EXPECT(types[1].find("requires") == std::string::npos);
    }
}

static void test_catalogString_fallbackChain() {
    TEST("catalogString — locale → en → key marker");
    const std::string lang = R"({"catalog.type.ds4.name":"DualShock 4 übersetzt"})";
    const std::string en = R"({"catalog.type.ds4.name":"DualShock 4","only.en":"English"})";
    EXPECT_EQ(catalogString(lang, en, "catalog.type.ds4.name"),
              std::string("DualShock 4 übersetzt"));
    EXPECT_EQ(catalogString(lang, en, "only.en"), std::string("English"));
    EXPECT_EQ(catalogString(lang, en, "missing.key"), std::string("missing.key"));
}

// CI completeness gate: every controller-TYPE string present in all supported
// locales. Feature slugs / modes / requires codes are NOT in the lang files —
// they are protocol constants and the structure test above pins them literal.
static void test_localeCompletenessGate() {
    TEST("locale gate — every catalog string resolves in every supported locale");
    for (const auto& locale : catalogLocales()) {
        const std::string content = readFileAll(langPath(locale));
        if (content.empty()) {
            g_fail++;
            std::cerr << "  FAIL [locale gate] missing lang file: " << langPath(locale) << "\n";
            continue;
        }
        for (const auto& key : catalogStringKeys()) {
            // catalogString falls back to the KEY ITSELF when missing — assert
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
    TEST("locale gate — non-English catalogs serve non-marker strings");
    const std::string enJson = readFileAll(langPath("en"));
    CatalogBackendTraits traits;
    for (const auto& locale : catalogLocales()) {
        const std::string langJson = readFileAll(langPath(locale));
        std::string json = buildCatalogJson(locale, langJson, enJson, "1.6.0", traits);
        EXPECT(json.find("\"locale\":\"" + locale + "\"") != std::string::npos);
        // The missing-string marker is the raw key — it must never surface.
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
    test_catalogString_fallbackChain();
    test_localeCompletenessGate();
    test_localizedCatalogsRenderLocalizedNames();

    std::cout << "catalog: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
