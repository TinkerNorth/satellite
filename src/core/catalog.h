// SPDX-License-Identifier: LGPL-3.0-or-later

// GET /api/catalog builder (docs/contract.md §Catalog): the static-per-version,
// localized description of what this satellite can create (controller types)
// and do (host features). Pure — callers load locale files and probe the
// backend; everything here is string-in/string-out so it unit tests without
// platform code.
#pragma once

#include <string>
#include <vector>

namespace satellite {

// Locale set kept in lockstep with dish-android; index 0 is the fallback.
inline const std::vector<std::string>& catalogLocales() {
    static const std::vector<std::string> locales = {"en", "es", "fr", "de", "bs", "pt-BR"};
    return locales;
}

// Resolve an Accept-Language header against catalogLocales(): tags ordered by
// q-value, exact match (case-insensitive) first, then primary-subtag match
// ("de-AT" → "de"), fallback "en".
std::string resolveCatalogLocale(const std::string& acceptLanguage);

// Static facts about the backend that shape the catalog. Derived from the
// backend's identity (vigem/uinput/none), NOT its live health — the catalog
// only changes on server upgrade.
struct CatalogBackendTraits {
    bool ds4MotionSupported = false;
    std::string ds4MotionRequires; // structured code, e.g. "vigembus>=1.17"; "" = none
    bool ds4TouchpadSupported = false;
    bool ds4LightbarSupported = false;
    bool mouseControlSupported = false;
};

// Localized string lookup: the locale's flat web/lang JSON first, then the
// `en` catalog, then the key itself (a visible marker that a string is
// missing — the CI completeness gate keeps that out of releases).
std::string catalogString(const std::string& langJson, const std::string& enJson,
                          const std::string& key);

// The full /api/catalog response body. `langJson`/`enJson` are the raw
// contents of the resolved locale's and English's web/lang files.
std::string buildCatalogJson(const std::string& locale, const std::string& langJson,
                             const std::string& enJson, const std::string& serverVersion,
                             const CatalogBackendTraits& traits);

// Catalog ETag: content varies only by server version and locale.
std::string catalogETag(const std::string& serverVersion, const std::string& locale);

// Slugs of the types the catalog serves images for, in catalog-id order.
const std::vector<std::string>& catalogImageSlugs();

// Keys buildCatalogJson resolves per locale file — the CI completeness gate
// asserts every locale defines all of them.
const std::vector<std::string>& catalogStringKeys();

} // namespace satellite
