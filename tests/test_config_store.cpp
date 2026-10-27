// SPDX-License-Identifier: LGPL-3.0-or-later
#include "core/config_store.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "test_util.h"

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

constexpr int FIRST_VALUE = 1000;
constexpr int BURST_UPDATES = 100;
constexpr auto BURST_STEP = 500us;
constexpr int MAX_BURST_SAVES = 3;
constexpr auto SETTLE_LIMIT = 5s;
constexpr auto SHORT_DEADLINE = 100ms;
constexpr auto SAVE_DELAY = 200ms;
constexpr auto PROMPT_FLUSH = 150ms;
constexpr int READER_THREADS = 4;
constexpr int TORN_WRITES = 20000;
constexpr int WRITER_THREADS = 4;
constexpr int INCREMENTS_PER_WRITER = 500;
constexpr int FAILED_ATTEMPTS = 2;
constexpr int CHANGED_PORT = 4242;

Config counterConfig(int value) {
    Config cfg;
    cfg.udpPort = value;
    cfg.webPort = value;
    return cfg;
}

struct SetCounter {
    int value;

    bool operator()(Config& cfg) const {
        cfg.udpPort = value;
        cfg.webPort = value;
        return true;
    }
};

struct SetAudio {
    bool mic;
    bool speaker;
    bool haptics;
    bool publish;

    bool operator()(Config& cfg) const {
        cfg.controllerAudioMic = mic;
        cfg.controllerAudioSpeaker = speaker;
        cfg.controllerAudioHaptics = haptics;
        return publish;
    }
};

class ReadingMutation {
  public:
    explicit ReadingMutation(ConfigStore& store) : store_(store) {}

    bool operator()(Config& cfg) {
        seen_ = store_.current()->udpPort;
        cfg.udpPort = seen_ + 1;
        return true;
    }

    int seen() const { return seen_; }

  private:
    ConfigStore& store_;
    int seen_ = 0;
};

bool incremented(Config& cfg) {
    cfg.webPort += 1;
    return true;
}

bool unchanged(Config&) { return false; }

void spinUntil(Clock::time_point when) {
    while (Clock::now() < when) std::this_thread::yield();
}

class RecordingLog : public ILogPort {
  public:
    void logMsg(LogLevel level, const std::string&, const std::string&) override {
        std::lock_guard<std::mutex> lk(mtx_);
        levels_.push_back(level);
    }

    int count(LogLevel level) const {
        std::lock_guard<std::mutex> lk(mtx_);
        return static_cast<int>(std::count(levels_.begin(), levels_.end(), level));
    }

  private:
    mutable std::mutex mtx_;
    std::vector<LogLevel> levels_;
};

class Persister {
  public:
    ConfigStore::Persist hook() {
        return [this](const Config& cfg) { return persist(cfg); };
    }

    void failNext(int attempts) {
        std::lock_guard<std::mutex> lk(mtx_);
        failuresLeft_ = attempts;
    }

    void block() {
        std::lock_guard<std::mutex> lk(mtx_);
        blocked_ = true;
    }

    void release() {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            blocked_ = false;
        }
        changed_.notify_all();
    }

    bool waitForCalls(int want) {
        std::unique_lock<std::mutex> lk(mtx_);
        return changed_.wait_for(lk, SETTLE_LIMIT, [this, want] { return calls_ >= want; });
    }

    bool waitForSaves(int want) {
        std::unique_lock<std::mutex> lk(mtx_);
        return changed_.wait_for(lk, SETTLE_LIMIT, [this, want] { return saves_ >= want; });
    }

    int calls() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return calls_;
    }

    int saves() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return saves_;
    }

    int lastSaved() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return lastSaved_;
    }

    int torn() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return torn_;
    }

    int callsHoldingALock() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return holdingALock_;
    }

    int callsOffTheSaverThread() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return offTheSaver_;
    }

  private:
    bool persist(const Config& cfg) {
#ifdef SATELLITE_LOCK_STATS
        const bool holdsALock = !lockstats::heldByThisThread().empty();
        const bool onTheSaver = currentThreadRole() == ThreadRole::ConfigSaver;
#endif
        std::unique_lock<std::mutex> lk(mtx_);
        ++calls_;
#ifdef SATELLITE_LOCK_STATS
        if (holdsALock) ++holdingALock_;
        if (!onTheSaver) ++offTheSaver_;
#endif
        changed_.notify_all();
        changed_.wait(lk, [this] { return !blocked_; });
        if (failuresLeft_ > 0) {
            --failuresLeft_;
            return false;
        }
        ++saves_;
        lastSaved_ = cfg.udpPort;
        if (cfg.udpPort != cfg.webPort) ++torn_;
        changed_.notify_all();
        return true;
    }

    mutable std::mutex mtx_;
    std::condition_variable changed_;
    bool blocked_ = false;
    int failuresLeft_ = 0;
    int calls_ = 0;
    int saves_ = 0;
    int lastSaved_ = 0;
    int torn_ = 0;
    int holdingALock_ = 0;
    int offTheSaver_ = 0;
};

struct ReaderTally {
    std::atomic<uint64_t> reads{0};
    std::atomic<uint64_t> torn{0};
};

void readCounters(ConfigStore& store, const std::atomic<bool>& stop, ReaderTally& tally) {
    while (!stop.load(std::memory_order_acquire)) {
        const std::shared_ptr<const Config> snapshot = store.current();
        tally.reads.fetch_add(1, std::memory_order_relaxed);
        if (snapshot->udpPort != snapshot->webPort) {
            tally.torn.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void writeCounters(ConfigStore& store, int count) {
    for (int value = 1; value <= count; ++value) store.update(SetCounter{value});
}

void incrementCounter(ConfigStore& store, int times) {
    for (int i = 0; i < times; ++i) store.update(incremented);
}

void test_anOldSnapshotIsUnchangedByLaterUpdates() {
    TEST("an old snapshot is unchanged by later updates");
    ConfigStore store(counterConfig(FIRST_VALUE), nullptr);
    const std::shared_ptr<const Config> before = store.current();
    const std::shared_ptr<const Config> published = store.update(SetCounter{FIRST_VALUE + 1});
    store.update(SetCounter{FIRST_VALUE + 2});
    EXPECT_EQ(before->udpPort, FIRST_VALUE);
    EXPECT_EQ(before->webPort, FIRST_VALUE);
    EXPECT_EQ(published->udpPort, FIRST_VALUE + 1);
    EXPECT_EQ(published->webPort, FIRST_VALUE + 1);
    EXPECT_EQ(store.current()->udpPort, FIRST_VALUE + 2);
    EXPECT(before != store.current());
    EXPECT(published != store.current());
}

void test_updateReturnsTheSnapshotItPublished() {
    TEST("update returns the snapshot it published, and current serves the same one");
    ConfigStore store(counterConfig(FIRST_VALUE), nullptr);
    const std::shared_ptr<const Config> published = store.update(SetCounter{FIRST_VALUE + 1});
    EXPECT(published == store.current());
    EXPECT_EQ(published->udpPort, FIRST_VALUE + 1);
}

void test_aNoChangeUpdatePublishesAndPersistsNothing() {
    TEST("a no-change update publishes nothing and persists nothing");
    Persister persister;
    ConfigStore store(counterConfig(FIRST_VALUE), persister.hook());
    const std::shared_ptr<const Config> before = store.current();
    const std::shared_ptr<const Config> after = store.update(unchanged);
    EXPECT(after == before);
    EXPECT(store.current() == before);
    EXPECT(store.flush(SETTLE_LIMIT));
    EXPECT_EQ(persister.calls(), 0);

    TEST("a mutate that edits its copy and then declines leaves the snapshot and the policy alone");
    const std::shared_ptr<const Config> declined =
        store.update(SetAudio{false, false, false, false});
    EXPECT(declined == before);
    EXPECT(store.current()->controllerAudioMic);
    EXPECT(store.audioPolicy().mic);
    EXPECT(store.flush(SETTLE_LIMIT));
    EXPECT_EQ(persister.calls(), 0);
}

void test_aMutateMayReadTheStoreItIsUpdating() {
    TEST("a mutate can read the store it is updating without deadlocking");
    ConfigStore store(counterConfig(FIRST_VALUE), nullptr);
    ReadingMutation mutation(store);
    store.update(std::ref(mutation));
    EXPECT_EQ(mutation.seen(), FIRST_VALUE);
    EXPECT_EQ(store.current()->udpPort, FIRST_VALUE + 1);
}

void test_readersNeverSeeATornSnapshot() {
    TEST("readers racing a writer never see a torn snapshot, and nothing torn is persisted");
    Persister persister;
    ConfigStore store(counterConfig(0), persister.hook());
    std::atomic<bool> stop{false};
    ReaderTally tally;
    std::vector<std::thread> readers;
    for (int i = 0; i < READER_THREADS; ++i) {
        readers.emplace_back(readCounters, std::ref(store), std::cref(stop), std::ref(tally));
    }
    writeCounters(store, TORN_WRITES);
    stop.store(true, std::memory_order_release);
    for (std::thread& reader : readers) reader.join();
    EXPECT(tally.reads.load() > 0);
    EXPECT_EQ(tally.torn.load(), uint64_t{0});
    EXPECT_EQ(store.current()->udpPort, TORN_WRITES);
    EXPECT_EQ(store.current()->webPort, TORN_WRITES);
    EXPECT(store.flush(SETTLE_LIMIT));
    EXPECT_EQ(persister.torn(), 0);
    EXPECT_EQ(persister.lastSaved(), TORN_WRITES);
}

void test_concurrentUpdatesAreNeverLost() {
    TEST("updates from several threads are all applied, none lost to a race");
    ConfigStore store(counterConfig(0), nullptr);
    std::vector<std::thread> writers;
    for (int i = 0; i < WRITER_THREADS; ++i) {
        writers.emplace_back(incrementCounter, std::ref(store), INCREMENTS_PER_WRITER);
    }
    for (std::thread& writer : writers) writer.join();
    EXPECT_EQ(store.current()->webPort, WRITER_THREADS * INCREMENTS_PER_WRITER);
}

void test_aBurstOfUpdatesIsCoalescedAndTheLastOneWins() {
    TEST("100 updates within 50 ms persist at most three times, the last one the latest");
    Persister persister;
    ConfigStore store(counterConfig(0), persister.hook());
    const Clock::time_point start = Clock::now();
    for (int value = 1; value <= BURST_UPDATES; ++value) {
        store.update(SetCounter{value});
        spinUntil(start + BURST_STEP * value);
    }
    EXPECT(store.flush(SETTLE_LIMIT));
    EXPECT(persister.saves() >= 1);
    EXPECT(persister.saves() <= MAX_BURST_SAVES);
    EXPECT_EQ(persister.lastSaved(), BURST_UPDATES);
    EXPECT_EQ(store.current()->udpPort, BURST_UPDATES);
}

void test_theSaveWaitsForTheBurstToSettleOnItsOwn() {
    TEST("without a flush the latest snapshot is persisted on its own, after the delay");
    Persister persister;
    ConfigStore store(counterConfig(0), persister.hook());
    const Clock::time_point began = Clock::now();
    store.update(SetCounter{FIRST_VALUE});
    EXPECT(persister.waitForSaves(1));
    EXPECT(Clock::now() - began >= SAVE_DELAY - 10ms);
    EXPECT_EQ(persister.lastSaved(), FIRST_VALUE);
    EXPECT_EQ(persister.saves(), 1);
}

void test_flushReturnsOnceTheGenerationBeforeItIsPersisted() {
    TEST("flush returns true at once when nothing is waiting to be persisted");
    Persister persister;
    ConfigStore store(counterConfig(0), persister.hook());
    EXPECT(store.flush(0ms));
    EXPECT_EQ(persister.calls(), 0);

    TEST("flush returns true only after persist ran for what was published before it");
    store.update(SetCounter{FIRST_VALUE + 1});
    EXPECT(store.flush(PROMPT_FLUSH));
    EXPECT_EQ(persister.lastSaved(), FIRST_VALUE + 1);
    EXPECT_EQ(persister.saves(), 1);
}

void test_flushGivesUpAtItsDeadlineWhilePersistIsBlocked() {
    TEST("flush returns false at its deadline while persist is blocked, true once it is let go");
    Persister persister;
    ConfigStore store(counterConfig(0), persister.hook());
    store.update(SetCounter{FIRST_VALUE + 1});
    EXPECT(store.flush(PROMPT_FLUSH));

    persister.block();
    store.update(SetCounter{FIRST_VALUE + 2});
    const Clock::time_point began = Clock::now();
    EXPECT(!store.flush(SHORT_DEADLINE));
    EXPECT(Clock::now() - began >= SHORT_DEADLINE);
    EXPECT(persister.waitForCalls(2));
    EXPECT_EQ(persister.lastSaved(), FIRST_VALUE + 1);

    persister.release();
    EXPECT(store.flush(SETTLE_LIMIT));
    EXPECT_EQ(persister.lastSaved(), FIRST_VALUE + 2);
}

void test_anUpdateDuringASlowPersistIsPersistedNext() {
    TEST("an update published while persist is running is persisted by the next pass");
    Persister persister;
    ConfigStore store(counterConfig(0), persister.hook());
    persister.block();
    store.update(SetCounter{FIRST_VALUE + 1});
    EXPECT(!store.flush(SHORT_DEADLINE));
    EXPECT(persister.waitForCalls(1));
    store.update(SetCounter{FIRST_VALUE + 2});
    persister.release();
    EXPECT(store.flush(SETTLE_LIMIT));
    EXPECT_EQ(persister.lastSaved(), FIRST_VALUE + 2);
    EXPECT(persister.saves() <= 2);
}

void test_aFailingPersistIsRetriedAndTheNextSaveCarriesTheLatest() {
    TEST("a failing persist is retried, logged, and the next success carries the latest snapshot");
    RecordingLog log;
    Persister persister;
    persister.failNext(FAILED_ATTEMPTS);
    ConfigStore store(counterConfig(0), persister.hook(), &log);
    store.update(SetCounter{FIRST_VALUE + 1});
    EXPECT(persister.waitForCalls(1));
    EXPECT_EQ(persister.saves(), 0);
    store.update(SetCounter{FIRST_VALUE + 2});
    EXPECT(persister.waitForSaves(1));
    EXPECT(persister.calls() > FAILED_ATTEMPTS);
    EXPECT_EQ(persister.lastSaved(), FIRST_VALUE + 2);
    EXPECT(store.flush(SETTLE_LIMIT));
    EXPECT(log.count(LogLevel::WARN) >= 1);
    EXPECT(log.count(LogLevel::INFO) >= 1);
}

void test_theAudioPolicyFollowsUpdates() {
    TEST("the audio policy read from the store starts from the initial config");
    Config initial;
    initial.controllerAudioSpeaker = false;
    ConfigStore store(initial, nullptr);
    EXPECT(store.audioPolicy().mic);
    EXPECT(!store.audioPolicy().speaker);
    EXPECT(store.audioPolicy().haptics);

    TEST("the audio policy follows every update, one field at a time");
    store.update(SetAudio{false, true, true, true});
    EXPECT(!store.audioPolicy().mic);
    EXPECT(store.audioPolicy().speaker);
    EXPECT(store.audioPolicy().haptics);
    store.update(SetAudio{false, false, true, true});
    EXPECT(!store.audioPolicy().mic);
    EXPECT(!store.audioPolicy().speaker);
    EXPECT(store.audioPolicy().haptics);
    store.update(SetAudio{true, true, false, true});
    EXPECT(store.audioPolicy().mic);
    EXPECT(store.audioPolicy().speaker);
    EXPECT(!store.audioPolicy().haptics);

    TEST("the receive port follows updates too");
    EXPECT_EQ(store.udpPort(), DEFAULT_UDP_PORT);
    store.update(SetCounter{CHANGED_PORT});
    EXPECT_EQ(store.udpPort(), CHANGED_PORT);
}

void test_stopFlushesThenJoins() {
    TEST("stop persists what is pending before it returns, and nothing is persisted after");
    Persister persister;
    ConfigStore store(counterConfig(0), persister.hook());
    store.update(SetCounter{FIRST_VALUE + 1});
    store.stop();
    EXPECT_EQ(persister.lastSaved(), FIRST_VALUE + 1);
    EXPECT_EQ(persister.saves(), 1);

    store.update(SetCounter{FIRST_VALUE + 2});
    EXPECT(!store.flush(SHORT_DEADLINE));
    EXPECT_EQ(store.current()->udpPort, FIRST_VALUE + 2);
    store.stop();
    EXPECT_EQ(persister.saves(), 1);
}

void test_destroyingTheStoreFlushesIt() {
    TEST("destroying a store persists what is pending");
    Persister persister;
    {
        ConfigStore store(counterConfig(0), persister.hook());
        store.update(SetCounter{FIRST_VALUE + 3});
    }
    EXPECT_EQ(persister.lastSaved(), FIRST_VALUE + 3);
    EXPECT_EQ(persister.saves(), 1);
}

void test_aStopThatCannotPersistGivesUpInsteadOfHanging() {
    TEST("stop gives up on a persist that fails rather than retrying forever");
    RecordingLog log;
    Persister persister;
    persister.failNext(1000);
    ConfigStore store(counterConfig(0), persister.hook(), &log);
    store.update(SetCounter{FIRST_VALUE + 1});
    store.stop();
    EXPECT_EQ(persister.saves(), 0);
    EXPECT(persister.calls() >= 1);
    EXPECT(log.count(LogLevel::WARN) >= 1);
}

#ifdef SATELLITE_LOCK_STATS

std::set<std::string> locksTakenByReceive() {
    std::set<std::string> names;
    for (const lockstats::LockStats& lock : lockstats::snapshot().locks) {
        if (lock.role(ThreadRole::Receive).touched()) names.insert(lock.name);
    }
    return names;
}

void test_persistRunsOnTheSaverThreadHoldingNoLock() {
    TEST("persist runs on the saver thread with no lock held");
    Persister persister;
    ConfigStore store(counterConfig(0), persister.hook());
    store.update(SetCounter{FIRST_VALUE + 1});
    store.update(SetCounter{FIRST_VALUE + 2});
    EXPECT(store.flush(SETTLE_LIMIT));
    EXPECT(persister.calls() >= 1);
    EXPECT_EQ(persister.callsHoldingALock(), 0);
    EXPECT_EQ(persister.callsOffTheSaverThread(), 0);
}

void test_theHotFieldsTakeNoLockAndASnapshotTakesOnlyTheSnapshotLock() {
    TEST("the audio policy and the port take no lock, and a snapshot only the snapshot lock");
    ConfigStore store(Config{}, nullptr);
    lockstats::reset();
    {
        ScopedThreadRole role(ThreadRole::Receive);
        const ControllerAudioPolicy policy = store.audioPolicy();
        const int port = store.udpPort();
        EXPECT(policy.mic);
        EXPECT_EQ(port, DEFAULT_UDP_PORT);
    }
    EXPECT(locksTakenByReceive().empty());
    {
        ScopedThreadRole role(ThreadRole::Receive);
        const std::shared_ptr<const Config> snapshot = store.current();
        EXPECT_EQ(snapshot->udpPort, DEFAULT_UDP_PORT);
    }
    EXPECT(locksTakenByReceive() == std::set<std::string>{"config.snapshot"});
}

#endif

} // namespace

int main() {
    test_anOldSnapshotIsUnchangedByLaterUpdates();
    test_updateReturnsTheSnapshotItPublished();
    test_aNoChangeUpdatePublishesAndPersistsNothing();
    test_aMutateMayReadTheStoreItIsUpdating();
    test_readersNeverSeeATornSnapshot();
    test_concurrentUpdatesAreNeverLost();
    test_aBurstOfUpdatesIsCoalescedAndTheLastOneWins();
    test_theSaveWaitsForTheBurstToSettleOnItsOwn();
    test_flushReturnsOnceTheGenerationBeforeItIsPersisted();
    test_flushGivesUpAtItsDeadlineWhilePersistIsBlocked();
    test_anUpdateDuringASlowPersistIsPersistedNext();
    test_aFailingPersistIsRetriedAndTheNextSaveCarriesTheLatest();
    test_theAudioPolicyFollowsUpdates();
    test_stopFlushesThenJoins();
    test_destroyingTheStoreFlushesIt();
    test_aStopThatCannotPersistGivesUpInsteadOfHanging();
#ifdef SATELLITE_LOCK_STATS
    test_persistRunsOnTheSaverThreadHoldingNoLock();
    test_theHotFieldsTakeNoLockAndASnapshotTakesOnlyTheSnapshotLock();
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
