// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Crash reporting: the policy, with no SDK anywhere near it.
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
// Everything here is std-only by design: this is the half the core purity gate
// allows, and the half worth testing. The Sentry binding lives in
// adapters/crash_adapter.h, which is where a third-party header belongs.

#pragma once

#include <string>

namespace satellite::crash {

// Compiled-in identity. These come from CMake, never from runtime config.
const char* compiledDsn();
const char* environment(); // "production" | "development"
const char* release();     // "satellite@<display version>"

// $SENTRY_DSN, or empty when unset: the deliberate escape hatch for pointing a
// local build at a scratch project. Read through _dupenv_s on MSVC, whose
// warnings-as-errors lane rejects std::getenv outright.
std::string envDsn();

// The pure policy. `compiled` is the baked-in DSN (may be empty), `envOverride`
// is $SENTRY_DSN (may be null or empty), `userEnabled` is the operator's opt-in.
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
// Windows passes lifecycle::sentryDir() instead, so crash state lands beside
// the existing dumps\ and logs\ in LocalAppData rather than in the roaming
// profile.
std::string databaseDirFor(const std::string& configFilePath);

} // namespace satellite::crash
