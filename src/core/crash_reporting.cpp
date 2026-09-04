// SPDX-License-Identifier: LGPL-3.0-or-later

#include "core/crash_reporting.h"

#include <cstddef>
#include <cstdlib>

// Injected by CMake. The fallbacks keep this translation unit compilable on
// its own, which is exactly the state the pure policy test builds it in: an
// empty DSN, so a test run can never transmit.
#ifndef SATELLITE_SENTRY_DSN
#define SATELLITE_SENTRY_DSN ""
#endif
#ifndef SATELLITE_SENTRY_ENVIRONMENT
#define SATELLITE_SENTRY_ENVIRONMENT "development"
#endif
#ifndef SATELLITE_SENTRY_RELEASE
#define SATELLITE_SENTRY_RELEASE "satellite@unknown"
#endif

namespace satellite::crash {

const char* compiledDsn() { return SATELLITE_SENTRY_DSN; }
const char* environment() { return SATELLITE_SENTRY_ENVIRONMENT; }
const char* release() { return SATELLITE_SENTRY_RELEASE; }

std::string envDsn() {
#ifdef _MSC_VER
    // The hardened MSVC lane builds with warnings as errors, and std::getenv
    // trips C4996 there. _dupenv_s is the sanctioned spelling; same contract.
    char* raw = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&raw, &len, "SENTRY_DSN") != 0 || raw == nullptr) { return {}; }
    std::string value(raw);
    std::free(raw);
    return value;
#else
    const char* raw = std::getenv("SENTRY_DSN");
    return raw != nullptr ? std::string(raw) : std::string();
#endif
}

bool shouldArm(const char* compiled, const char* envOverride, bool userEnabled) {
    // The opt-in is checked first and is never bypassed. $SENTRY_DSN lets a
    // developer aim a build at a scratch project; it is not a way to report
    // from a machine whose operator declined.
    if (!userEnabled) { return false; }
    const bool haveCompiled = compiled != nullptr && compiled[0] != '\0';
    const bool haveOverride = envOverride != nullptr && envOverride[0] != '\0';
    return haveCompiled || haveOverride;
}

std::string databaseDirFor(const std::string& configFilePath) {
    const std::size_t cut = configFilePath.find_last_of("/\\");
    if (cut == std::string::npos) {
        // No directory component at all: keep it beside whatever the caller
        // named rather than inventing an absolute path.
        return "sentry";
    }
    // Reuse the separator already in the path so a Windows caller does not get
    // a mixed one back.
    return configFilePath.substr(0, cut + 1) + "sentry";
}

} // namespace satellite::crash
