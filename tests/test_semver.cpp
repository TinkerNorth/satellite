// SPDX-License-Identifier: LGPL-3.0-or-later
#include "../src/core/semver.h"

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

using satellite::compareSemver;

static int sign(int v) { return v < 0 ? -1 : (v > 0 ? 1 : 0); }

static void expectOrder(const std::string& lo, const std::string& hi) {
    EXPECT(sign(compareSemver(lo, hi)) == -1);
    EXPECT(sign(compareSemver(hi, lo)) == 1);
    EXPECT(sign(compareSemver(lo, lo)) == 0);
    EXPECT(sign(compareSemver(hi, hi)) == 0);
}

static void testCoreOrdering() {
    TEST("equal core versions compare equal");
    EXPECT(compareSemver("1.2.3", "1.2.3") == 0);

    TEST("major dominates");
    expectOrder("1.9.9", "2.0.0");

    TEST("minor dominates patch");
    expectOrder("1.2.9", "1.3.0");

    TEST("patch ordering");
    expectOrder("1.2.3", "1.2.4");

    TEST("numeric core not lexicographic: 1.9.0 < 1.10.0");
    expectOrder("1.9.0", "1.10.0");

    TEST("numeric core not lexicographic: 2.0.0 < 10.0.0");
    expectOrder("2.0.0", "10.0.0");

    TEST("double-digit patch: 1.0.2 < 1.0.10");
    expectOrder("1.0.2", "1.0.10");
}

static void testReleaseVsPrerelease() {
    TEST("a release outranks a prerelease of the same core");
    expectOrder("1.5.0-rc.1", "1.5.0");

    TEST("prerelease of a higher core still outranks a lower release");
    expectOrder("1.4.0", "1.5.0-rc.1");

    TEST("two equal prereleases compare equal");
    EXPECT(compareSemver("1.5.0-rc.1", "1.5.0-rc.1") == 0);
}

static void testPrereleaseNumericOrdering() {
    TEST("rc.9 < rc.10 (numeric, not lexicographic)");
    expectOrder("1.5.0-rc.9", "1.5.0-rc.10");

    TEST("rc.2 < rc.11");
    expectOrder("1.5.0-rc.2", "1.5.0-rc.11");

    TEST("beta.2 < beta.11");
    expectOrder("1.0.0-beta.2", "1.0.0-beta.11");

    TEST("leading zeros in a numeric identifier do not change precedence");
    EXPECT(compareSemver("1.5.0-rc.1", "1.5.0-rc.01") == 0);

    TEST("large numeric identifiers do not overflow");
    expectOrder("1.0.0-rc.99999999999999999999", "1.0.0-rc.999999999999999999999");
}

static void testPrereleaseIdentifierCounts() {
    TEST("a longer prerelease set outranks a prefix-equal shorter one");
    expectOrder("1.0.0-alpha", "1.0.0-alpha.1");

    TEST("alpha.1 < alpha.beta");
    expectOrder("1.0.0-alpha.1", "1.0.0-alpha.beta");
}

static void testPrereleaseNumericVsAlpha() {
    TEST("numeric identifier ranks below an alphanumeric one");
    expectOrder("1.0.0-1", "1.0.0-alpha");

    TEST("alpha < beta (ascii)");
    expectOrder("1.0.0-alpha", "1.0.0-beta");

    TEST("beta < rc");
    expectOrder("1.0.0-beta", "1.0.0-rc");
}

static void testSemverSpecChain() {
    const char* chain[] = {"1.0.0-alpha",  "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta",
                           "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-rc.1",       "1.0.0"};
    const int n = static_cast<int>(sizeof(chain) / sizeof(chain[0]));
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            TEST("spec chain monotonic");
            EXPECT(sign(compareSemver(chain[i], chain[j])) == -1);
            EXPECT(sign(compareSemver(chain[j], chain[i])) == 1);
        }
    }
}

static void testMalformedComponents() {
    TEST("unparseable core component treated as 0");
    EXPECT(compareSemver("1.x.0", "1.0.0") == 0);

    TEST("missing components default to 0");
    EXPECT(compareSemver("1", "1.0.0") == 0);
    EXPECT(compareSemver("1.2", "1.2.0") == 0);

    TEST("empty strings compare equal");
    EXPECT(compareSemver("", "") == 0);

    TEST("extra core components beyond patch are ignored");
    EXPECT(compareSemver("1.2.3.4", "1.2.3.9") == 0);

    TEST("a leading 'v' is not stripped here (caller normalizes), so v1 parses as 0");
    EXPECT(compareSemver("v1.0.0", "0.0.0") == 0);
}

static void testAntisymmetry() {
    const char* vers[] = {"1.0.0",        "1.0.1",        "1.1.0",       "2.0.0",
                          "1.0.0-rc.1",   "1.0.0-rc.2",   "1.0.0-rc.10", "1.0.0-alpha",
                          "1.0.0-beta.2", "1.0.0-beta.11"};
    const int n = static_cast<int>(sizeof(vers) / sizeof(vers[0]));
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            TEST("antisymmetry: compare(a,b) == -compare(b,a)");
            EXPECT(sign(compareSemver(vers[i], vers[j])) == -sign(compareSemver(vers[j], vers[i])));
        }
    }
}

int main() {
    std::cout << "Running semver tests...\n\n";

    testCoreOrdering();
    testReleaseVsPrerelease();
    testPrereleaseNumericOrdering();
    testPrereleaseIdentifierCounts();
    testPrereleaseNumericVsAlpha();
    testSemverSpecChain();
    testMalformedComponents();
    testAntisymmetry();

    std::cout << "\n=== Test Results ===\n";
    std::cout << "  Passed: " << g_pass << "\n";
    std::cout << "  Failed: " << g_fail << "\n";
    std::cout << "  STATUS: " << (g_fail == 0 ? "ALL PASSED" : "FAILED") << "\n";
    return g_fail == 0 ? 0 : 1;
}
