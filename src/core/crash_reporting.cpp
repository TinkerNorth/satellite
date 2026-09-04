// SPDX-License-Identifier: LGPL-3.0-or-later

#include "core/crash_reporting.h"

#include <cstdlib>

// Injected by CMake. The fallbacks keep this translation unit compilable on
// its own (the pure policy test builds it without the rest of the build).
#ifndef SATELLITE_SENTRY_DSN
#define SATELLITE_SENTRY_DSN ""
#endif
#ifndef SATELLITE_SENTRY_ENVIRONMENT
#define SATELLITE_SENTRY_ENVIRONMENT "development"
#endif
#ifndef SATELLITE_SENTRY_RELEASE
#define SATELLITE_SENTRY_RELEASE "satellite@unknown"
#endif

#ifdef SATELLITE_HAS_SENTRY
#include <sentry.h>
#endif

namespace satellite::crash {

namespace {
bool g_active = false;
// Remembered from init() so a live flip can re-arm without the admin route
// having to know where satellite keeps its state.
std::string g_databaseDir;
} // namespace

const char* compiledDsn() { return SATELLITE_SENTRY_DSN; }
const char* environment() { return SATELLITE_SENTRY_ENVIRONMENT; }
const char* release() { return SATELLITE_SENTRY_RELEASE; }

bool sdkAvailable() {
#ifdef SATELLITE_HAS_SENTRY
    return true;
#else
    return false;
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

void init([[maybe_unused]] bool userEnabled, [[maybe_unused]] const std::string& databaseDir) {
    // Remembered even when the policy says no, so a later opt-in can arm
    // without being handed the path again.
    if (!databaseDir.empty()) { g_databaseDir = databaseDir; }

    if (g_active) { return; }
    if (!shouldArm(compiledDsn(), std::getenv("SENTRY_DSN"), userEnabled)) { return; }

#ifdef SATELLITE_HAS_SENTRY
    sentry_options_t* options = sentry_options_new();

    // Leave the DSN unset when only $SENTRY_DSN is present: the SDK reads the
    // environment itself, and setting an empty string here would override it.
    if (compiledDsn()[0] != '\0') { sentry_options_set_dsn(options, compiledDsn()); }

    // Not the working directory. A tray app launched from Explorer, or the
    // service, has no cwd worth writing run state into.
    sentry_options_set_database_path(options, databaseDir.c_str());

    sentry_options_set_release(options, release());
    sentry_options_set_environment(options, environment());

    // Off in shipped builds: the SDK's debug channel is noisy and satellite
    // already has its own log.
    sentry_options_set_debug(options, 0);

    // The crash itself is the payload. Session tracking would report every
    // start and stop of a server that is meant to run unattended for weeks,
    // which is telemetry the operator did not agree to when they ticked a box
    // labelled "crash reports".
    sentry_options_set_auto_session_tracking(options, 0);

    // Backtraces routinely pick up the home directory, and satellite's runs
    // next to paired-device state. Sentry's own IP inference is off for the
    // same reason: nothing here needs to know who the operator is.
    sentry_options_set_send_default_pii(options, 0);

    if (sentry_init(options) == 0) { g_active = true; }
#endif
}

void setEnabled(bool userEnabled) {
    if (!userEnabled) {
        shutdown();
        return;
    }
    init(true, g_databaseDir);
}

void shutdown() {
    if (!g_active) { return; }
    g_active = false;
#ifdef SATELLITE_HAS_SENTRY
    sentry_close();
#endif
}

bool active() { return g_active; }

} // namespace satellite::crash
