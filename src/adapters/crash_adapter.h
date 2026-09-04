// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Binds the crash-reporting policy in core/crash_reporting.h to the Sentry
// native SDK. The third-party header lives here rather than in src/core
// because core is std-only by gate (scripts/check_core_purity.sh) and has to
// stay compilable on every platform with no external surface.
//
// Every entry point is inert when the SDK was not found at configure time
// (SATELLITE_HAS_SENTRY undefined), so a source checkout with no Sentry
// package still builds and behaves.

#pragma once

#include <string>

namespace satellite::crash {

// True when this build was linked against the Sentry SDK at all. False means
// everything below is a no-op.
bool sdkAvailable();

// Arms the SDK if the policy in core/crash_reporting.h says so. `databaseDir`
// is where Sentry keeps its run state and any pending envelope; it must be a
// writable absolute path that survives restarts, NOT the working directory (a
// tray app launched from Explorer, or the service, has no predictable cwd).
//
// Idempotent. Safe to call when the SDK is absent or the policy says no.
void init(bool userEnabled, const std::string& databaseDir);

// Applies a live flip of the operator's opt-in, reusing the database path the
// last init() was given.
//
// Disarming is immediate and deliberate: withdrawing consent has to stop the
// next crash from being sent, not the one after that. Arming is also
// immediate, so the switch is not a lie in either direction.
void setEnabled(bool userEnabled);

// Flushes pending events and closes the SDK. Safe when init() never armed.
void shutdown();

// True only when init() actually armed the SDK. Drives what the admin UI is
// allowed to claim: a build with no DSN must not show "reports are being sent".
bool active();

} // namespace satellite::crash
