// SPDX-License-Identifier: LGPL-3.0-or-later
#include "../src/net/mdns_rejoin.h"

#include <cstdint>
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

namespace {
mdns::RejoinDecision decide(bool force, bool haveOld, const uint8_t (&oldIp)[4], bool haveNew,
                            const uint8_t (&newIp)[4]) {
    return mdns::evaluateRejoin(force, haveOld, oldIp, haveNew, newIp);
}
} // namespace

int main() {
    const uint8_t a[4] = {192, 168, 1, 50};
    const uint8_t aCopy[4] = {192, 168, 1, 50};
    const uint8_t lastOctet[4] = {192, 168, 1, 51};
    const uint8_t firstOctet[4] = {10, 168, 1, 50};
    const uint8_t middle[4] = {192, 168, 9, 50};
    const uint8_t zero[4] = {0, 0, 0, 0};

    {
        TEST("resume with the same address still rejoins (membership was dropped) but is not a "
             "change");
        auto d = decide(true, true, a, true, aCopy);
        EXPECT(d.rejoin);
        EXPECT(!d.ipChanged);
    }

    {
        TEST("periodic tick with an unchanged address does nothing (no IGMP churn)");
        auto d = decide(false, true, a, true, aCopy);
        EXPECT(!d.rejoin);
        EXPECT(!d.ipChanged);
    }

    {
        TEST("periodic tick after a DHCP address change rejoins and re-announces");
        auto d = decide(false, true, a, true, lastOctet);
        EXPECT(d.rejoin);
        EXPECT(d.ipChanged);
    }

    {
        TEST("forced rejoin after an address change is still a change");
        auto d = decide(true, true, a, true, firstOctet);
        EXPECT(d.rejoin);
        EXPECT(d.ipChanged);
    }

    {
        TEST("a difference in only the last octet counts as changed");
        auto d = decide(false, true, a, true, lastOctet);
        EXPECT(d.ipChanged);
    }

    {
        TEST("a difference in only the first octet counts as changed");
        auto d = decide(false, true, a, true, firstOctet);
        EXPECT(d.ipChanged);
    }

    {
        TEST("a difference in a middle octet counts as changed");
        auto d = decide(false, true, a, true, middle);
        EXPECT(d.ipChanged);
    }

    {
        TEST("gaining an address (was unresolved, now resolved) is a change and rejoins");
        auto d = decide(false, false, zero, true, a);
        EXPECT(d.rejoin);
        EXPECT(d.ipChanged);
    }

    {
        TEST("losing the address (was resolved, now unresolved) is a change and rejoins");
        auto d = decide(false, true, a, false, zero);
        EXPECT(d.rejoin);
        EXPECT(d.ipChanged);
    }

    {
        TEST("no address before or after, no force: nothing to do");
        auto d = decide(false, false, zero, false, zero);
        EXPECT(!d.rejoin);
        EXPECT(!d.ipChanged);
    }

    {
        TEST("no address before or after, but forced: rejoin without claiming a change");
        auto d = decide(true, false, zero, false, zero);
        EXPECT(d.rejoin);
        EXPECT(!d.ipChanged);
    }

    {
        TEST("new-address bytes are only compared when the new address is valid");
        auto d = decide(false, false, a, false, lastOctet);
        EXPECT(!d.rejoin);
        EXPECT(!d.ipChanged);
    }

    {
        TEST("stale old bytes are ignored once we have no current address");
        auto d = decide(false, true, a, false, a);
        EXPECT(d.rejoin);
        EXPECT(d.ipChanged);
    }

    std::cout << "test_mdns_rejoin: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
