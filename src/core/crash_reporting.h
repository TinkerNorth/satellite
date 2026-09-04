// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Crash reporting: the decision layer over the Sentry native SDK.
//
// Satellite transmits nothing unless BOTH of these are true:
//
//   1. The build carries a DSN. SATELLITE_SENTRY_DSN is empty by default and
//      is only injected by release.yml from a repository secret, so a local
//      build, a PR build and a fork build all physically cannot report,
//      whatever environment string they claim. A label can be passed on the
//      command line; a missing DSN cannot be argued with.
//   2. The operator opted in. Config::crashReporting defaults to false, so an
//      existing install that never saw the switch stays silent.
//
// The environment string separates the two worlds in Sentry. It is derived
// from SATELLITE_RELEASE_VERSION, which only release.yml sets, so "production"
// cannot be reached by a developer build even by accident.
//
// The policy itself (shouldArm) is a pure function kept out of the SDK-guarded
// section so the tests can drive it without linking Sentry.

#pragma once

#include <string>

namespace satellite::crash {

// Compiled-in identity. These come from CMake, never from runtime config.
const char* compiledDsn();
const char* environment(); // "production" | "development"
const char* release();     // "satellite@<display version>"

// True when this build was linked against the Sentry SDK at all. False means
// every entry point below is an inert stub.
bool sdkAvailable();

// The pure policy. `compiled` is the baked-in DSN (may be empty), `envOverride`
// is $SENTRY_DSN (may be null: the deliberate escape hatch for testing a local
// build against a scratch project), `userEnabled` is the operator's opt-in.
//
// Reporting arms only when some DSN exists AND the operator said yes. The
// escape hatch deliberately still respects the opt-in: a developer pointing a
// build at their own project is not a reason to bypass a user's choice on a
// machine that is not theirs.
bool shouldArm(const char* compiled, const char* envOverride, bool userEnabled);

// Derives the SDK's state directory from the config file's path: the config
// file's own directory, plus a "sentry" subdirectory. Pure and separator-aware
// so it can be checked against explicit inputs.
//
// Windows uses ensureSubdir() instead, so crash state lands beside the existing
// dumps\ and logs\ in LocalAppData rather than in the roaming profile.
std::string databaseDirFor(const std::string& configFilePath);

// Arms the SDK if shouldArm() says so. `databaseDir` is where Sentry keeps its
// run state and any pending envelope; it must be a writable absolute path that
// survives restarts, NOT the working directory (a tray app launched from
// Explorer or run as a service has no predictable cwd).
//
// Idempotent. Safe to call when the SDK is absent or the policy says no.
void init(bool userEnabled, const std::string& databaseDir);

// Applies a live flip of the operator's opt-in, reusing the database path the
// last init() was given.
//
// Disarming is immediate and deliberate: withdrawing consent has to stop the
// next crash from being sent, not the next-restart-after-that one. Arming is
// also immediate, so the switch is not a lie in either direction.
void setEnabled(bool userEnabled);

// Flushes pending events and closes the SDK. Safe when init() never armed.
void shutdown();

// True only when init() actually armed the SDK. Drives what the admin UI is
// allowed to claim: a build with no DSN must not show "reports are being sent".
bool active();

} // namespace satellite::crash
