// SPDX-License-Identifier: LGPL-3.0-or-later
#include "../src/net/pairing.h"

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
        resetPairRequestsForTest();
        TEST("submit registers a pending request the dashboard can see");
        submitPairRequest("dev1", "Pixel", "192.168.1.5", "1234");
        std::string key;
        EXPECT(pollPairRequest("dev1", key) == PairRequestState::Pending);
        auto list = pendingPairRequests();
        EXPECT(list.size() == 1);
        EXPECT(list[0].deviceId == "dev1");
        EXPECT(list[0].deviceName == "Pixel");
        EXPECT(list[0].clientIP == "192.168.1.5");
        EXPECT(list[0].secondsRemaining > 0);
    }

    {
        resetPairRequestsForTest();
        TEST("poll for an unknown device is None");
        std::string key;
        EXPECT(pollPairRequest("nope", key) == PairRequestState::None);
        EXPECT(key.empty());
    }

    {
        resetPairRequestsForTest();
        TEST("matched accept approves and hands the key back exactly once");
        submitPairRequest("dev1", "Pixel", "ip", "4821");
        std::string name, ip;
        EXPECT(acceptPairRequest("dev1", "4821", "deadbeefkey", name, ip));
        EXPECT(name == "Pixel");
        EXPECT(ip == "ip");
        std::string key;
        EXPECT(pollPairRequest("dev1", key) == PairRequestState::Approved);
        EXPECT(key == "deadbeefkey");
        // Single-use: once the key is read the request is gone.
        std::string key2;
        EXPECT(pollPairRequest("dev1", key2) == PairRequestState::None);
        EXPECT(key2.empty());
        EXPECT(pendingPairRequests().empty());
    }

    {
        resetPairRequestsForTest();
        TEST("wrong PIN is rejected and leaves the request pending for a retry");
        submitPairRequest("dev1", "Pixel", "ip", "1111");
        std::string name, ip;
        EXPECT(!acceptPairRequest("dev1", "9999", "key", name, ip));
        std::string key;
        EXPECT(pollPairRequest("dev1", key) == PairRequestState::Pending);
        // A subsequent correct accept still works.
        EXPECT(acceptPairRequest("dev1", "1111", "key2", name, ip));
        EXPECT(pollPairRequest("dev1", key) == PairRequestState::Approved);
        EXPECT(key == "key2");
    }

    {
        resetPairRequestsForTest();
        TEST("accept for an unknown device is false");
        std::string name, ip;
        EXPECT(!acceptPairRequest("ghost", "0000", "k", name, ip));
    }

    {
        resetPairRequestsForTest();
        TEST("deny removes the request");
        submitPairRequest("dev1", "Pixel", "ip", "1234");
        EXPECT(denyPairRequest("dev1"));
        std::string key;
        EXPECT(pollPairRequest("dev1", key) == PairRequestState::None);
        EXPECT(!denyPairRequest("dev1")); // already gone
    }

    {
        resetPairRequestsForTest();
        TEST("a re-tap refreshes in place and rotates the PIN (no duplicate row)");
        submitPairRequest("dev1", "Pixel", "ip", "1111");
        submitPairRequest("dev1", "Pixel 8", "ip2", "2222");
        EXPECT(pendingPairRequests().size() == 1);
        EXPECT(pendingPairRequests()[0].deviceName == "Pixel 8");
        std::string name, ip;
        EXPECT(!acceptPairRequest("dev1", "1111", "k", name, ip)); // stale PIN no longer valid
        EXPECT(acceptPairRequest("dev1", "2222", "k", name, ip));
    }

    {
        resetPairRequestsForTest();
        TEST("multiple devices are tracked independently");
        submitPairRequest("a", "A", "ip", "1000");
        submitPairRequest("b", "B", "ip", "2000");
        EXPECT(pendingPairRequests().size() == 2);
        std::string name, ip;
        EXPECT(acceptPairRequest("a", "1000", "ka", name, ip));
        EXPECT(pendingPairRequests().size() == 1); // b is still pending
        std::string key;
        EXPECT(pollPairRequest("b", key) == PairRequestState::Pending);
    }

    std::cout << "test_pairing: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
