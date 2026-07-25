// SPDX-License-Identifier: LGPL-3.0-or-later

// GET /api/catalog builder: the static-per-version, localized description of
// what this satellite can create (controller types) and do (host features).
// Pure: callers load locale files and probe the backend; everything here is
// string-in/string-out so it unit tests without platform code.
#pragma once

#include <string>
#include <vector>

namespace satellite {

// Catalog schema version, bumped when the /api/catalog payload shape evolves in a
// way clients may branch on (distinct from protocolVersion / serverVersion). v2
// offers up to four types per backend + per-type `emulates` hints; a response
// WITHOUT this field is the legacy v1 catalog (xbox360 + ds4, no emulates). Additive
// within protocolVersion 1: a client reads an absent field as 1.
inline constexpr int kCatalogVersion = 2;

// Locale set kept in lockstep with dish-android; index 0 is the fallback.
inline const std::vector<std::string>& catalogLocales() {
    static const std::vector<std::string> locales = {"en", "es", "fr", "de", "bs", "pt-BR"};
    return locales;
}

// Resolve an Accept-Language header against catalogLocales(): tags ordered by
// q-value, exact match (case-insensitive) first, then primary-subtag match
// ("de-AT" to "de"), fallback "en".
std::string resolveCatalogLocale(const std::string& acceptLanguage);

// Static facts about the backend that shape the catalog. Derived from the
// backend's identity, NOT its live health (the catalog only changes on server
// upgrade).
struct CatalogBackendTraits {
    bool ds4MotionSupported = false;
    std::string ds4MotionRequires; // structured code, e.g. "vigembus>=1.17"; "" = none
    bool ds4TouchpadSupported = false;
    bool ds4LightbarSupported = false;
    bool mouseControlSupported = false;
    // rumble is RECEIVE (host streams rumble back); keyboardControl is SEND
    // (host injects keystrokes). A backend reports what it can't do so the
    // client gates instead of assuming.
    bool rumbleSupported = false;
    bool keyboardControlSupported = false;
    // Which controller types this backend materializes. The catalog offering and
    // the invalidType apply gate stay in lockstep with these (ViGEm has no
    // DualSense/Switch target; macOS has no DualSense codec yet).
    bool offersXbox = false;
    bool offersDS4 = false;
    bool offersDualSense = false;
    bool offersSwitchPro = false;
};

// Localized string lookup: the locale's flat web/lang JSON first, then the `en`
// catalog, then the key itself (a visible missing-string marker; the CI
// completeness gate keeps it out of releases).
std::string catalogString(const std::string& langJson, const std::string& enJson,
                          const std::string& key);

// The full /api/catalog response body. `langJson`/`enJson` are the raw contents
// of the resolved locale's and English's web/lang files.
std::string buildCatalogJson(const std::string& locale, const std::string& langJson,
                             const std::string& enJson, const std::string& serverVersion,
                             const CatalogBackendTraits& traits);

// The /api/server/capabilities `host` block, readable pre-pairing. `supported`
// mirrors the catalog hostFeatures; `available` ANDs it with live backend
// liveness so a feature can read present-but-currently-down.
std::string buildHostBlockJson(const CatalogBackendTraits& traits, bool backendAvailable);

// Catalog ETag: content varies only by server version and locale.
std::string catalogETag(const std::string& serverVersion, const std::string& locale);

// Slugs of the types the catalog serves images for, in catalog-id order.
const std::vector<std::string>& catalogImageSlugs();

// Keys buildCatalogJson resolves per locale file; the CI completeness gate
// asserts every locale defines all of them.
const std::vector<std::string>& catalogStringKeys();

} // namespace satellite
