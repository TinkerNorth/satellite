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
    // Host features beyond mouse. rumble is RECEIVE (the host streams rumble back to
    // the client); keyboardControl is SEND (the host injects keystrokes). A backend
    // that cannot do one reports it so the client gates instead of assuming.
    bool rumbleSupported = false;
    bool keyboardControlSupported = false;
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

// The /api/server/capabilities `host` block: the receiver's own capability inventory,
// readable pre-pairing. `supported` mirrors the catalog hostFeatures; `available` ANDs
// it with live backend liveness so a feature can read present-but-currently-down.
std::string buildHostBlockJson(const CatalogBackendTraits& traits, bool backendAvailable);

// Catalog ETag: content varies only by server version and locale.
std::string catalogETag(const std::string& serverVersion, const std::string& locale);

// Slugs of the types the catalog serves images for, in catalog-id order.
const std::vector<std::string>& catalogImageSlugs();

// Keys buildCatalogJson resolves per locale file — the CI completeness gate
// asserts every locale defines all of them.
const std::vector<std::string>& catalogStringKeys();

} // namespace satellite
