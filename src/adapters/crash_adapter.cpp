// SPDX-License-Identifier: LGPL-3.0-or-later

#include "adapters/crash_adapter.h"

#include "core/crash_reporting.h"

#ifdef SATELLITE_HAS_SENTRY
#include <sentry.h>
#ifdef _WIN32
#include <windows.h>

#include <cstddef>
#endif
#endif

namespace satellite::crash {

namespace {
bool g_active = false;
// Remembered from init() so a live flip can re-arm without the admin route
// having to know where satellite keeps its state.
std::string g_databaseDir;
} // namespace

bool sdkAvailable() {
#ifdef SATELLITE_HAS_SENTRY
    return true;
#else
    return false;
#endif
}

void init([[maybe_unused]] bool userEnabled, [[maybe_unused]] const std::string& databaseDir) {
    // Remembered even when the policy says no, so a later opt-in can arm
    // without being handed the path again.
    if (!databaseDir.empty()) { g_databaseDir = databaseDir; }

    if (g_active) { return; }

    const std::string envOverride = envDsn();
    if (!shouldArm(compiledDsn(), envOverride.c_str(), userEnabled)) { return; }

#ifdef SATELLITE_HAS_SENTRY
    sentry_options_t* options = sentry_options_new();

    // Leave the DSN unset when only $SENTRY_DSN is present: the SDK reads the
    // environment itself, and setting an empty string here would override it.
    if (compiledDsn()[0] != '\0') { sentry_options_set_dsn(options, compiledDsn()); }

    // Not the working directory. A tray app launched from Explorer, or the
    // service, has no cwd worth writing run state into.
    sentry_options_set_database_path(options, g_databaseDir.c_str());

#ifdef _WIN32
    // The crashpad backend is a separate process, staged beside satellite.exe
    // by CMake and shipped by the installer. The SDK's own lookup is relative
    // to the working directory, which a tray app launched from Explorer, or
    // the service, does not control.
    {
        char exe[MAX_PATH];
        const DWORD len = GetModuleFileNameA(nullptr, exe, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            std::string handler(exe, len);
            const std::size_t cut = handler.find_last_of("\\/");
            if (cut != std::string::npos) { handler.erase(cut + 1); }
            handler += "crashpad_handler.exe";
            sentry_options_set_handler_path(options, handler.c_str());
        }
    }
#endif

    sentry_options_set_release(options, release());
    sentry_options_set_environment(options, environment());

    // Off in shipped builds: the SDK's debug channel is noisy and satellite
    // already has its own log.
    sentry_options_set_debug(options, 0);

    // The crash itself is the payload. Session tracking would report every
    // start and stop of a server meant to run unattended for weeks, which is
    // telemetry the operator did not agree to when they ticked a box labelled
    // "crash reports".
    sentry_options_set_auto_session_tracking(options, 0);

    // No sentry_options_set_send_default_pii() call here on purpose: in
    // sentry-native that setter exists only under SENTRY_PLATFORM_NX, and its
    // own documentation states that not sending PII is already the default
    // everywhere ("If false (the default), the SDK won't add PII or other
    // sensitive data to the payload"). Calling it would not compile on any
    // platform satellite ships to, and would not change behaviour if it did.

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
