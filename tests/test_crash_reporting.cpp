// SPDX-License-Identifier: LGPL-3.0-or-later
//
// The arming policy is the whole privacy property of crash reporting, so it is
// tested as a pure function rather than inferred from an integration run. Two
// independent gates have to hold: a DSN must exist, and the operator must have
// said yes. Neither one alone is enough, in either direction.

#include "adapters/crash_adapter.h"
#include "core/config_json.h"
#include "core/crash_reporting.h"

#include <iostream>
#include <string>

#include "test_util.h"

using namespace satellite;

static const char* kDsn = "https://key@o1.ingest.de.sentry.io/2";

static void test_no_dsn_never_arms() {
    TEST("shouldArm: a build with no DSN cannot report, even when opted in");
    // This is every local build, every PR build and every build from a fork,
    // because only release.yml injects the secret. Opting in is not a way
    // around it.
    EXPECT(!crash::shouldArm("", nullptr, true));
    EXPECT(!crash::shouldArm(nullptr, nullptr, true));
    EXPECT(!crash::shouldArm("", "", true));
}

static void test_opt_out_never_arms() {
    TEST("shouldArm: a declined opt-in beats any DSN");
    EXPECT(!crash::shouldArm(kDsn, nullptr, false));
    // Including the developer escape hatch: aiming a build at your own project
    // is not a reason to transmit from a machine that is not yours.
    EXPECT(!crash::shouldArm("", kDsn, false));
    EXPECT(!crash::shouldArm(kDsn, kDsn, false));
}

static void test_arms_when_both_hold() {
    TEST("shouldArm: a compiled DSN plus consent arms");
    EXPECT(crash::shouldArm(kDsn, nullptr, true));
}

static void test_env_override_is_the_dev_hatch() {
    TEST("shouldArm: $SENTRY_DSN substitutes for a compiled DSN, with consent");
    EXPECT(crash::shouldArm("", kDsn, true));
    EXPECT(crash::shouldArm(nullptr, kDsn, true));
}

static void test_test_build_carries_no_dsn() {
    TEST("the test binary itself has no DSN compiled in");
    // If this ever fails, a build has been configured in a way that would let
    // `ctest` transmit, which no test run should ever be able to do.
    EXPECT(std::string(crash::compiledDsn()).empty());
    EXPECT(!crash::sdkAvailable());
    EXPECT_EQ(std::string(crash::environment()), std::string("development"));
}

static void test_default_is_off() {
    TEST("Config: crash reporting defaults off");
    // An install upgrading into this feature has an operator who was never
    // asked, so the default has to answer "no" on their behalf.
    Config cfg;
    EXPECT(!cfg.crashReporting);
}

static void test_absent_key_reads_as_off() {
    TEST("Config: a config predating the key reads as off, not as opt-in");
    Config cfg;
    parseConfigInto(R"({"udpPort":9876})", cfg);
    EXPECT(!cfg.crashReporting);
    EXPECT_EQ(cfg.udpPort, 9876);
}

static void test_round_trip() {
    TEST("Config: crash reporting survives a serialize/parse round-trip");
    Config in;
    in.crashReporting = true;
    Config out;
    parseConfigInto(serializeConfig(in), out);
    EXPECT(out.crashReporting);

    // An explicit false must survive against a struct whose field is already
    // true, or a user's opt-out would silently revert on the next load.
    Config off;
    off.crashReporting = false;
    Config wasOn;
    wasOn.crashReporting = true;
    parseConfigInto(serializeConfig(off), wasOn);
    EXPECT(!wasOn.crashReporting);
}

static void test_inactive_without_sdk() {
    TEST("crash::active() stays false when the SDK is absent");
    EXPECT(!crash::active());
    // The test binary compiles the facade without SATELLITE_HAS_SENTRY and
    // with an empty DSN, so these must be no-ops rather than crashes.
    crash::init(true, std::string());
    EXPECT(!crash::active());
    crash::setEnabled(true);
    EXPECT(!crash::active());
    crash::setEnabled(false);
    EXPECT(!crash::active());
    crash::shutdown();
    EXPECT(!crash::active());
}

int main() {
    test_no_dsn_never_arms();
    test_opt_out_never_arms();
    test_arms_when_both_hold();
    test_env_override_is_the_dev_hatch();
    test_test_build_carries_no_dsn();
    test_default_is_off();
    test_absent_key_reads_as_off();
    test_round_trip();
    test_inactive_without_sdk();

    std::cout << "crash_reporting: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
