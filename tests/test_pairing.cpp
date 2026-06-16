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
        EXPECT(list[0].pin == "1234");
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
        TEST("accept approves and hands the key back exactly once");
        submitPairRequest("dev1", "Pixel", "ip", "4821");
        std::string name, ip;
        EXPECT(acceptPairRequestConfirmed("dev1", "deadbeefkey", name, ip));
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
        EXPECT(pendingPairRequests()[0].pin == "2222"); // dashboard shows the fresh PIN
    }

    {
        resetPairRequestsForTest();
        TEST("multiple devices are tracked independently");
        submitPairRequest("a", "A", "ip", "1000");
        submitPairRequest("b", "B", "ip", "2000");
        EXPECT(pendingPairRequests().size() == 2);
        std::string name, ip;
        EXPECT(acceptPairRequestConfirmed("a", "ka", name, ip));
        EXPECT(pendingPairRequests().size() == 1); // b is still pending
        std::string key;
        EXPECT(pollPairRequest("b", key) == PairRequestState::Pending);
    }

    {
        resetPairRequestsForTest();
        TEST("pairRequestSnapshot exposes the PIN in-process for a Pending request");
        submitPairRequest("dev1", "Pixel", "10.0.0.9", "7777");
        std::string name, ip, pin;
        int secs = 0;
        EXPECT(pairRequestSnapshot("dev1", name, ip, pin, secs));
        EXPECT(name == "Pixel");
        EXPECT(ip == "10.0.0.9");
        EXPECT(pin == "7777");
        EXPECT(secs > 0);
        // No request for an unknown device.
        EXPECT(!pairRequestSnapshot("nope", name, ip, pin, secs));
    }

    {
        resetPairRequestsForTest();
        TEST("acceptPairRequestConfirmed approves without re-checking the PIN");
        submitPairRequest("dev1", "Pixel", "ip", "4821");
        std::string name, ip;
        EXPECT(acceptPairRequestConfirmed("dev1", "confirmedkey", name, ip));
        EXPECT(name == "Pixel");
        std::string key;
        EXPECT(pollPairRequest("dev1", key) == PairRequestState::Approved);
        EXPECT(key == "confirmedkey");
        // No Pending request → false.
        EXPECT(!acceptPairRequestConfirmed("ghost", "k", name, ip));
    }

    {
        resetPairRequestsForTest();
        TEST("setPairRequestListener fires once per new request with the device id");
        std::string fired;
        int count = 0;
        setPairRequestListener([&](const std::string& id) {
            fired = id;
            count++;
        });
        submitPairRequest("dev-listen", "Pixel", "ip", "1234");
        EXPECT(count == 1);
        EXPECT(fired == "dev-listen");
        setPairRequestListener(nullptr); // detach so later tests are unaffected
    }

    {
        resetPairRequestsForTest();
        TEST("the pending registry caps at kMaxPending (8), evicting the oldest");
        for (int i = 1; i <= 9; i++) {
            submitPairRequest("dev" + std::to_string(i), "D", "ip", "0000");
        }
        EXPECT(pendingPairRequests().size() == 8); // capped, not 9
        std::string key;
        EXPECT(pollPairRequest("dev1", key) == PairRequestState::None); // oldest evicted
        EXPECT(pollPairRequest("dev9", key) == PairRequestState::Pending);
    }

    {
        resetPairRequestsForTest();
        TEST("accepting an unknown device fails so the operator can be warned");
        std::string name, ip;
        EXPECT(!acceptPairRequestConfirmed("ghost", "k", name, ip));
    }

    {
        resetPairRequestsForTest();
        TEST("a second accept of an already-approved request fails (no silent double-pair)");
        submitPairRequest("dev1", "Pixel", "ip", "1234");
        std::string name, ip;
        EXPECT(acceptPairRequestConfirmed("dev1", "key-one", name, ip));
        EXPECT(!acceptPairRequestConfirmed("dev1", "key-two", name, ip));
    }

    {
        resetPairRequestsForTest();
        TEST("accepting a denied request fails (the request is gone)");
        submitPairRequest("dev1", "Pixel", "ip", "1234");
        EXPECT(denyPairRequest("dev1"));
        std::string name, ip;
        EXPECT(!acceptPairRequestConfirmed("dev1", "key", name, ip));
    }

    {
        resetPairRequestsForTest();
        TEST("a re-tap after approval clears the staged key and returns to Pending");
        submitPairRequest("dev1", "Pixel", "ip", "1111");
        std::string name, ip;
        EXPECT(acceptPairRequestConfirmed("dev1", "stale-key", name, ip));
        submitPairRequest("dev1", "Pixel", "ip", "2222");
        std::string key = "sentinel";
        EXPECT(pollPairRequest("dev1", key) == PairRequestState::Pending);
        EXPECT(key == "sentinel");
    }

    {
        resetPairRequestsForTest();
        TEST("a re-tapped request can be approved again with a fresh key");
        submitPairRequest("dev1", "Pixel", "ip", "1111");
        std::string name, ip;
        EXPECT(acceptPairRequestConfirmed("dev1", "stale-key", name, ip));
        submitPairRequest("dev1", "Pixel", "ip", "2222");
        EXPECT(acceptPairRequestConfirmed("dev1", "fresh-key", name, ip));
        std::string key;
        EXPECT(pollPairRequest("dev1", key) == PairRequestState::Approved);
        EXPECT(key == "fresh-key");
    }

    {
        resetPairRequestsForTest();
        TEST("a freshly submitted request reflects the extended 5-minute TTL");
        submitPairRequest("dev1", "Pixel", "ip", "1234");
        std::string name, ip, pin;
        int secs = 0;
        EXPECT(pairRequestSnapshot("dev1", name, ip, pin, secs));
        EXPECT(secs > 240);
        EXPECT(secs <= 300);
    }

    std::cout << "test_pairing: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
