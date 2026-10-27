// SPDX-License-Identifier: LGPL-3.0-or-later
#include "../src/core/tracked_mutex.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "test_util.h"

namespace {

using namespace std::chrono_literals;

TrackedMutex g_constantInitialized("test.global");

#ifdef SATELLITE_LOCK_STATS

constexpr auto HOLD = std::chrono::milliseconds(100);
constexpr uint64_t HOLD_FLOOR_NS = 90'000'000;
constexpr uint64_t WAIT_FLOOR_NS = 50'000'000;
constexpr uint64_t UNCONTENDED_CEILING_NS = 20'000'000;

const lockstats::RoleStats& statsFor(const lockstats::Snapshot& snap, const char* lock,
                                     ThreadRole role) {
    static const lockstats::RoleStats none{};
    const lockstats::LockStats* found = snap.find(lock);
    return found != nullptr ? found->role(role) : none;
}

uint64_t histogramTotal(const lockstats::Histogram& histogram) {
    uint64_t total = 0;
    for (const uint64_t n : histogram) total += n;
    return total;
}

uint64_t pairCount(const std::vector<lockstats::NamePair>& pairs, const std::string& first,
                   const std::string& second) {
    for (const lockstats::NamePair& pair : pairs) {
        if (pair.first == first && pair.second == second) return pair.count;
    }
    return 0;
}

size_t pairsNaming(const std::vector<lockstats::NamePair>& pairs, const std::string& prefix) {
    return static_cast<size_t>(
        std::count_if(pairs.begin(), pairs.end(), [&](const lockstats::NamePair& pair) {
            return pair.first.rfind(prefix, 0) == 0 || pair.second.rfind(prefix, 0) == 0;
        }));
}

uint64_t regionHits(const lockstats::Snapshot& snap, const std::string& region) {
    for (const lockstats::RegionHit& hit : snap.regions) {
        if (hit.region == region) return hit.hits;
    }
    return 0;
}

void test_countsEveryAcquisitionAgainstItsRole() {
    TEST("stats: every acquisition is counted against the role of the thread that took it");
    lockstats::reset();
    TrackedMutex m("test.counts");
    {
        ScopedThreadRole role(ThreadRole::Maintenance);
        for (int i = 0; i < 5; ++i) { std::lock_guard<TrackedMutex> lk(m); }
        EXPECT(m.try_lock());
        m.unlock();
        std::unique_lock<TrackedMutex> deferred(m, std::try_to_lock);
        EXPECT(deferred.owns_lock());
    }
    std::thread other([&] {
        ScopedThreadRole role(ThreadRole::HttpAdmin);
        std::lock_guard<TrackedMutex> lk(m);
    });
    other.join();

    const lockstats::Snapshot snap = lockstats::snapshot();
    const lockstats::RoleStats& maintenance =
        statsFor(snap, "test.counts", ThreadRole::Maintenance);
    EXPECT_EQ(maintenance.acquisitions, 7u);
    EXPECT_EQ(maintenance.contended, 0u);
    EXPECT_EQ(maintenance.tryFailures, 0u);
    EXPECT_EQ(histogramTotal(maintenance.waitHistogram), 7u);
    EXPECT_EQ(histogramTotal(maintenance.holdHistogram), 7u);
    EXPECT_EQ(statsFor(snap, "test.counts", ThreadRole::HttpAdmin).acquisitions, 1u);
    EXPECT_EQ(statsFor(snap, "test.counts", ThreadRole::Other).acquisitions, 0u);
    EXPECT(statsFor(snap, "test.counts", ThreadRole::Maintenance).touched());
    EXPECT(!statsFor(snap, "test.counts", ThreadRole::Receive).touched());
}

void test_failedTryLockIsCountedWithoutAnAcquisition() {
    TEST("stats: a failed try_lock is counted as a failure, not as an acquisition");
    lockstats::reset();
    TrackedMutex m("test.try");
    std::atomic<bool> held{false};
    std::atomic<bool> release{false};
    std::thread holder([&] {
        std::lock_guard<TrackedMutex> lk(m);
        held = true;
        while (!release) std::this_thread::yield();
    });
    while (!held) std::this_thread::yield();
    {
        ScopedThreadRole role(ThreadRole::BackendWorker);
        EXPECT(!m.try_lock());
        std::unique_lock<TrackedMutex> attempt(m, std::try_to_lock);
        EXPECT(!attempt.owns_lock());
    }
    release = true;
    holder.join();

    const lockstats::Snapshot snap = lockstats::snapshot();
    const lockstats::RoleStats& worker = statsFor(snap, "test.try", ThreadRole::BackendWorker);
    EXPECT_EQ(worker.tryFailures, 2u);
    EXPECT_EQ(worker.acquisitions, 0u);
    EXPECT(worker.touched());
    EXPECT_EQ(statsFor(snap, "test.try", ThreadRole::Other).acquisitions, 1u);
}

void test_contentionChargesTheWaitToTheWaiterAndTheHoldToTheHolder() {
    TEST("stats: under contention the waiter records the wait and the holder records the hold");
    lockstats::reset();
    TrackedMutex m("test.contention");
    std::atomic<bool> held{false};
    std::atomic<bool> waiterReady{false};
    std::thread holder([&] {
        ScopedThreadRole role(ThreadRole::Topology);
        std::lock_guard<TrackedMutex> lk(m);
        held = true;
        while (!waiterReady) std::this_thread::yield();
        std::this_thread::sleep_for(HOLD);
    });
    while (!held) std::this_thread::yield();
    {
        ScopedThreadRole role(ThreadRole::Receive);
        waiterReady = true;
        std::lock_guard<TrackedMutex> lk(m);
    }
    holder.join();

    const lockstats::Snapshot snap = lockstats::snapshot();
    const lockstats::RoleStats& waiter = statsFor(snap, "test.contention", ThreadRole::Receive);
    const lockstats::RoleStats& owner = statsFor(snap, "test.contention", ThreadRole::Topology);
    EXPECT_EQ(waiter.acquisitions, 1u);
    EXPECT_EQ(waiter.contended, 1u);
    EXPECT(waiter.waitMaxNs >= WAIT_FLOOR_NS);
    EXPECT_EQ(waiter.waitTotalNs, waiter.waitMaxNs);
    EXPECT(waiter.holdMaxNs < UNCONTENDED_CEILING_NS);
    EXPECT(lockstats::histogramPercentileNs(waiter.waitHistogram, 0.99) >= waiter.waitMaxNs);
    EXPECT_EQ(owner.acquisitions, 1u);
    EXPECT_EQ(owner.contended, 0u);
    EXPECT(owner.waitMaxNs < UNCONTENDED_CEILING_NS);
    EXPECT(owner.holdMaxNs >= HOLD_FLOOR_NS);
    EXPECT_EQ(owner.holdTotalNs, owner.holdMaxNs);
}

void test_nestingRecordsAnEdgeFromEveryHeldLock() {
    TEST("stats: a blocking lock taken under others records an edge from each held lock");
    lockstats::reset();
    TrackedMutex a("test.nest.a");
    TrackedMutex b("test.nest.b");
    TrackedMutex c("test.nest.c");
    {
        std::lock_guard<TrackedMutex> la(a);
        std::lock_guard<TrackedMutex> lb(b);
        std::lock_guard<TrackedMutex> lc(c);
    }
    {
        std::lock_guard<TrackedMutex> lc(c);
        std::unique_lock<TrackedMutex> tried(a, std::try_to_lock);
        EXPECT(tried.owns_lock());
    }
    { std::lock_guard<TrackedMutex> lb(b); }

    const lockstats::Snapshot snap = lockstats::snapshot();
    EXPECT_EQ(pairCount(snap.edges, "test.nest.a", "test.nest.b"), 1u);
    EXPECT_EQ(pairCount(snap.edges, "test.nest.a", "test.nest.c"), 1u);
    EXPECT_EQ(pairCount(snap.edges, "test.nest.b", "test.nest.c"), 1u);
    EXPECT_EQ(pairCount(snap.edges, "test.nest.c", "test.nest.a"), 0u);
    EXPECT_EQ(pairsNaming(snap.edges, "test.nest."), 3u);
}

void test_outOfOrderUnlockKeepsTheStackStraight() {
    TEST("stats: releasing an outer lock first leaves the inner one on the thread's stack");
    lockstats::reset();
    TrackedMutex outer("test.order.outer");
    TrackedMutex inner("test.order.inner");
    TrackedMutex later("test.order.later");
    outer.lock();
    inner.lock();
    outer.unlock();
    EXPECT(lockstats::heldByThisThread() == std::vector<std::string>{"test.order.inner"});
    later.lock();
    later.unlock();
    inner.unlock();
    EXPECT(lockstats::heldByThisThread().empty());

    const lockstats::Snapshot snap = lockstats::snapshot();
    EXPECT_EQ(pairCount(snap.edges, "test.order.inner", "test.order.later"), 1u);
    EXPECT_EQ(pairCount(snap.edges, "test.order.outer", "test.order.later"), 0u);
}

void test_blockingRegionRecordsEveryHeldLock() {
    TEST("stats: a blocking region records the region against every lock the thread holds");
    lockstats::reset();
    TrackedMutex a("test.region.a");
    TrackedMutex b("test.region.b");
    blockingRegion("test.idle");
    {
        std::lock_guard<TrackedMutex> la(a);
        std::lock_guard<TrackedMutex> lb(b);
        blockingRegion("test.busy");
    }
    {
        std::lock_guard<TrackedMutex> la(a);
        blockingRegion("test.busy");
    }

    const lockstats::Snapshot snap = lockstats::snapshot();
    EXPECT_EQ(regionHits(snap, "test.idle"), 1u);
    EXPECT_EQ(regionHits(snap, "test.busy"), 2u);
    EXPECT_EQ(pairCount(snap.blocking, "test.busy", "test.region.a"), 2u);
    EXPECT_EQ(pairCount(snap.blocking, "test.busy", "test.region.b"), 1u);
    EXPECT_EQ(pairsNaming(snap.blocking, "test.idle"), 0u);
}

void test_scopedRoleRestoresThePreviousRole() {
    TEST("roles: a scoped role restores the role that was in effect before it");
    EXPECT(currentThreadRole() == ThreadRole::Other);
    {
        ScopedThreadRole receive(ThreadRole::Receive);
        EXPECT(currentThreadRole() == ThreadRole::Receive);
        {
            ScopedThreadRole logger(ThreadRole::Logger);
            EXPECT(currentThreadRole() == ThreadRole::Logger);
        }
        EXPECT(currentThreadRole() == ThreadRole::Receive);
    }
    EXPECT(currentThreadRole() == ThreadRole::Other);

    lockstats::reset();
    TrackedMutex m("test.role");
    std::thread worker([&] {
        setThreadRole(ThreadRole::HttpClient);
        std::lock_guard<TrackedMutex> lk(m);
    });
    worker.join();
    const lockstats::Snapshot snap = lockstats::snapshot();
    EXPECT_EQ(statsFor(snap, "test.role", ThreadRole::HttpClient).acquisitions, 1u);
    EXPECT_EQ(std::string(threadRoleName(ThreadRole::BackendWorker)), std::string("BackendWorker"));
}

void test_conditionVariableAnyReleasesTheLockWhileWaiting() {
    TEST("stats: a condition_variable_any wait releases the tracked lock and takes it back");
    lockstats::reset();
    TrackedMutex m("test.cv");
    std::condition_variable_any cv;
    bool ready = false;
    std::atomic<bool> waiting{false};
    std::vector<std::string> heldAfterWait;
    std::thread waiter([&] {
        ScopedThreadRole role(ThreadRole::Update);
        std::unique_lock<TrackedMutex> lk(m);
        waiting = true;
        cv.wait(lk, [&] { return ready; });
        heldAfterWait = lockstats::heldByThisThread();
    });
    while (!waiting) std::this_thread::yield();
    std::this_thread::sleep_for(HOLD);
    {
        std::lock_guard<TrackedMutex> lk(m);
        ready = true;
    }
    cv.notify_all();
    waiter.join();

    const lockstats::Snapshot snap = lockstats::snapshot();
    const lockstats::RoleStats& update = statsFor(snap, "test.cv", ThreadRole::Update);
    EXPECT(update.acquisitions >= 2u);
    EXPECT(update.holdMaxNs < HOLD_FLOOR_NS);
    EXPECT(heldAfterWait == std::vector<std::string>{"test.cv"});
}

void test_resetClearsTheCountsAndKeepsTheNames() {
    TEST("stats: reset clears every count but keeps the registered names");
    TrackedMutex a("test.reset.a");
    TrackedMutex b("test.reset.b");
    {
        std::lock_guard<TrackedMutex> la(a);
        std::lock_guard<TrackedMutex> lb(b);
        blockingRegion("test.reset.region");
    }
    lockstats::reset();

    const lockstats::Snapshot snap = lockstats::snapshot();
    const lockstats::LockStats* kept = snap.find("test.reset.a");
    EXPECT(kept != nullptr);
    if (kept != nullptr) {
        for (const lockstats::RoleStats& role : kept->roles) {
            EXPECT_EQ(role.acquisitions, 0u);
            EXPECT_EQ(histogramTotal(role.holdHistogram), 0u);
        }
    }
    EXPECT(snap.edges.empty());
    EXPECT(snap.blocking.empty());
    EXPECT(snap.regions.empty());
    EXPECT_EQ(snap.overflows, 0u);
}

void test_instancesSharingANameShareTheirStats() {
    TEST("stats: two locks with one name are counted as one lock");
    lockstats::reset();
    TrackedMutex first("test.shared");
    TrackedMutex second("test.shared");
    { std::lock_guard<TrackedMutex> lk(first); }
    { std::lock_guard<TrackedMutex> lk(second); }
    { std::lock_guard<TrackedMutex> lk(g_constantInitialized); }
    const lockstats::Snapshot snap = lockstats::snapshot();
    EXPECT_EQ(statsFor(snap, "test.shared", ThreadRole::Other).acquisitions, 2u);
    EXPECT_EQ(statsFor(snap, "test.global", ThreadRole::Other).acquisitions, 1u);
    size_t named = 0;
    for (const lockstats::LockStats& lock : snap.locks) named += lock.name == "test.shared" ? 1 : 0;
    EXPECT_EQ(named, 1u);
}

void test_percentileReadsTheUpperEdgeOfItsBucket() {
    TEST("stats: a histogram percentile is the upper edge of the bucket that holds it");
    lockstats::Histogram histogram{};
    EXPECT_EQ(lockstats::histogramPercentileNs(histogram, 0.99), 0u);
    histogram[10] = 99;
    histogram[20] = 1;
    EXPECT_EQ(lockstats::histogramPercentileNs(histogram, 0.5), lockstats::bucketUpperNs(10));
    EXPECT_EQ(lockstats::histogramPercentileNs(histogram, 0.99), lockstats::bucketUpperNs(10));
    EXPECT_EQ(lockstats::histogramPercentileNs(histogram, 0.999), lockstats::bucketUpperNs(20));
    EXPECT_EQ(lockstats::bucketUpperNs(10), 1023u);
    EXPECT_EQ(lockstats::bucketUpperNs(0), 0u);
}

#else

static_assert(sizeof(TrackedMutex) == sizeof(std::mutex) + sizeof(const char*),
              "without lock statistics a tracked mutex is a mutex and a name pointer");
static_assert(std::is_empty_v<ScopedThreadRole>,
              "without lock statistics a thread role carries no state");

void test_plainMutexIsLockable() {
    TEST("plain: the tracked mutex works with the standard lock types");
    TrackedMutex m("test.plain");
    EXPECT_EQ(std::string(m.name()), std::string("test.plain"));
    {
        std::lock_guard<TrackedMutex> lk(m);
        std::unique_lock<TrackedMutex> second(m, std::try_to_lock);
        EXPECT(!second.owns_lock());
    }
    std::unique_lock<TrackedMutex> lk(m, std::try_to_lock);
    EXPECT(lk.owns_lock());
    std::condition_variable_any cv;
    EXPECT(!cv.wait_for(lk, 1ms, [] { return false; }));
    EXPECT(lk.owns_lock());
}

void test_plainRolesAndRegionsDoNothing() {
    TEST("plain: roles and blocking regions compile to nothing");
    ScopedThreadRole role(ThreadRole::Receive);
    setThreadRole(ThreadRole::Gui);
    blockingRegion("test.plain.region");
    std::lock_guard<TrackedMutex> lk(g_constantInitialized);
    EXPECT_EQ(std::string(g_constantInitialized.name()), std::string("test.global"));
}

#endif

} // namespace

int main() {
#ifdef SATELLITE_LOCK_STATS
    test_scopedRoleRestoresThePreviousRole();
    test_countsEveryAcquisitionAgainstItsRole();
    test_failedTryLockIsCountedWithoutAnAcquisition();
    test_contentionChargesTheWaitToTheWaiterAndTheHoldToTheHolder();
    test_nestingRecordsAnEdgeFromEveryHeldLock();
    test_outOfOrderUnlockKeepsTheStackStraight();
    test_blockingRegionRecordsEveryHeldLock();
    test_conditionVariableAnyReleasesTheLockWhileWaiting();
    test_resetClearsTheCountsAndKeepsTheNames();
    test_instancesSharingANameShareTheirStats();
    test_percentileReadsTheUpperEdgeOfItsBucket();
#else
    test_plainMutexIsLockable();
    test_plainRolesAndRegionsDoNothing();
#endif

    std::cout << "\n=== Test Results ===\n";
    std::cout << "  Passed: " << g_pass << "\n";
    std::cout << "  Failed: " << g_fail << "\n";
    if (g_fail > 0) {
        std::cout << "  STATUS: FAIL\n";
        return 1;
    }
    std::cout << "  STATUS: ALL PASSED\n";
    return 0;
}
