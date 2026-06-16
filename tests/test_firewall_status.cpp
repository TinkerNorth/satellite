// SPDX-License-Identifier: LGPL-3.0-or-later
#include "../src/core/firewall_status.h"

#include <iostream>
#include <string>

static int g_pass = 0;
static int g_fail = 0;
static std::string g_currentTest;

#define TEST(name)                                                                                 \
    do { g_currentTest = (name); } while (0)

#define EXPECT(cond)                                                                               \
    do {                                                                                           \
        if (cond) {                                                                                \
            g_pass++;                                                                              \
        } else {                                                                                   \
            g_fail++;                                                                              \
            std::cerr << "  FAIL [" << g_currentTest << "] " << __FILE__ << ":" << __LINE__        \
                      << "  " << #cond << "\n";                                                    \
        }                                                                                          \
    } while (0)

int main() {
    {
        TEST("profileBit maps the NLM category names");
        EXPECT(fw::profileBit("domain") == fw::PROFILE_DOMAIN);
        EXPECT(fw::profileBit("private") == fw::PROFILE_PRIVATE);
        EXPECT(fw::profileBit("public") == fw::PROFILE_PUBLIC);
        EXPECT(fw::profileBit("") == 0);
        EXPECT(fw::profileBit("unknown") == 0);
    }

    {
        TEST("no matching rule for this binary is Missing, whatever the network");
        EXPECT(fw::evaluateFirewall(fw::PROFILE_PRIVATE, 0, false) == fw::FirewallState::Missing);
        EXPECT(fw::evaluateFirewall(0, 0, false) == fw::FirewallState::Missing);
    }

    {
        TEST("rule covers the active profile -> Configured");
        EXPECT(fw::evaluateFirewall(fw::PROFILE_PRIVATE, fw::PROFILE_PRIVATE, true) ==
               fw::FirewallState::Configured);
        EXPECT(fw::evaluateFirewall(fw::PROFILE_PRIVATE, fw::PROFILE_PRIVATE | fw::PROFILE_DOMAIN,
                                    true) == fw::FirewallState::Configured);
    }

    {
        TEST("rule exists but only on the wrong profile -> WrongProfile (the Public-only dev bug)");
        EXPECT(fw::evaluateFirewall(fw::PROFILE_PRIVATE, fw::PROFILE_PUBLIC, true) ==
               fw::FirewallState::WrongProfile);
        EXPECT(fw::evaluateFirewall(fw::PROFILE_DOMAIN, fw::PROFILE_PUBLIC, true) ==
               fw::FirewallState::WrongProfile);
    }

    {
        TEST("an all-profiles rule mask (domain|private|public) covers any active profile");
        const int all = fw::PROFILE_DOMAIN | fw::PROFILE_PRIVATE | fw::PROFILE_PUBLIC;
        EXPECT(fw::evaluateFirewall(fw::PROFILE_PRIVATE, all, true) ==
               fw::FirewallState::Configured);
        EXPECT(fw::evaluateFirewall(fw::PROFILE_PUBLIC, all, true) ==
               fw::FirewallState::Configured);
    }

    {
        TEST("rule present but the active profile is unknown -> Unknown, not a false alarm");
        EXPECT(fw::evaluateFirewall(0, fw::PROFILE_PRIVATE, true) == fw::FirewallState::Unknown);
    }

    {
        TEST("state strings are stable wire values");
        EXPECT(fw::firewallStateString(fw::FirewallState::Configured) == "configured");
        EXPECT(fw::firewallStateString(fw::FirewallState::WrongProfile) == "wrong-profile");
        EXPECT(fw::firewallStateString(fw::FirewallState::Missing) == "missing");
        EXPECT(fw::firewallStateString(fw::FirewallState::Unknown) == "unknown");
    }

    std::cout << "test_firewall_status: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
