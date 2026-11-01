// SPDX-License-Identifier: LGPL-3.0-or-later
#include "core/config_effects.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
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
using satellite::AppliedConfigEffects;
using satellite::applyConfigEffects;
using satellite::ConfigEffects;
using satellite::ConfigEffectSinks;

constexpr int RACING_THREADS = 8;
constexpr int ROUNDS_PER_THREAD = 300;
constexpr auto WRITER_BLOCKED_FOR = 60ms;

enum class Effect { AutoStart, CrashReporting };

struct Call {
    Effect effect;
    bool value;
};

bool operator==(const Call& a, const Call& b) { return a.effect == b.effect && a.value == b.value; }

class RecordingSinks {
  public:
    ConfigEffectSinks sinks() {
        return ConfigEffectSinks{[this](bool on) { record(Effect::AutoStart, on); },
                                 [this](bool on) { record(Effect::CrashReporting, on); }};
    }

    std::vector<Call> calls() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return calls_;
    }

    size_t count() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return calls_.size();
    }

    bool last(Effect effect, bool& value) const {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto it = calls_.rbegin(); it != calls_.rend(); ++it) {
            if (it->effect != effect) continue;
            value = it->value;
            return true;
        }
        return false;
    }

  private:
    void record(Effect effect, bool on) {
        std::lock_guard<std::mutex> lk(mtx_);
        calls_.push_back(Call{effect, on});
    }

    mutable std::mutex mtx_;
    std::vector<Call> calls_;
};

Config configWith(bool autoStart, bool crashReporting) {
    Config cfg;
    cfg.autoStart = autoStart;
    cfg.crashReporting = crashReporting;
    return cfg;
}

ConfigStore::Persist acceptingPersist() {
    return [](const Config&) { return true; };
}

bool unchanged(Config&) { return false; }

bool setPort(Config& cfg) {
    cfg.udpPort += 1;
    return true;
}

void testFirstSightAppliesBothEffects() {
    TEST("pure: nothing is assumed about the state the process started in, so the first snapshot "
         "applies both effects with its own values");
    RecordingSinks rec;
    AppliedConfigEffects applied;
    applyConfigEffects(configWith(true, false), applied, rec.sinks());
    const std::vector<Call> expected{{Effect::AutoStart, true}, {Effect::CrashReporting, false}};
    EXPECT(rec.calls() == expected);
    EXPECT(applied.autoStart.has_value() && *applied.autoStart);
    EXPECT(applied.crashReporting.has_value() && !*applied.crashReporting);
}

void testUnchangedSnapshotAppliesNothing() {
    TEST("pure: a snapshot that matches what was applied applies nothing");
    RecordingSinks rec;
    AppliedConfigEffects applied;
    applyConfigEffects(configWith(true, false), applied, rec.sinks());
    const size_t before = rec.count();
    applyConfigEffects(configWith(true, false), applied, rec.sinks());
    applyConfigEffects(configWith(true, false), applied, rec.sinks());
    EXPECT_EQ(rec.count(), before);
}

void testOnlyTheDifferingEffectIsApplied() {
    TEST("pure: only the effect whose value differs is applied");
    RecordingSinks rec;
    AppliedConfigEffects applied;
    applyConfigEffects(configWith(false, false), applied, rec.sinks());
    const size_t before = rec.count();

    applyConfigEffects(configWith(true, false), applied, rec.sinks());
    EXPECT_EQ(rec.count(), before + 1);
    EXPECT(rec.calls().back() == (Call{Effect::AutoStart, true}));

    applyConfigEffects(configWith(true, true), applied, rec.sinks());
    EXPECT_EQ(rec.count(), before + 2);
    EXPECT(rec.calls().back() == (Call{Effect::CrashReporting, true}));
}

void testSettingsWithoutEffectsApplyNothing() {
    TEST("pure: a change to a setting with no side effect applies nothing");
    RecordingSinks rec;
    AppliedConfigEffects applied;
    applyConfigEffects(configWith(false, true), applied, rec.sinks());
    const size_t before = rec.count();
    Config other = configWith(false, true);
    other.udpPort += 1;
    other.discoveryBroadcastEnabled = !other.discoveryBroadcastEnabled;
    other.networkInterface = "eth0";
    applyConfigEffects(other, applied, rec.sinks());
    EXPECT_EQ(rec.count(), before);
}

void testFlippingBackIsAppliedAgain() {
    TEST("pure: a value flipped and flipped back is applied each time it differs");
    RecordingSinks rec;
    AppliedConfigEffects applied;
    applyConfigEffects(configWith(false, true), applied, rec.sinks());
    applyConfigEffects(configWith(true, true), applied, rec.sinks());
    applyConfigEffects(configWith(false, true), applied, rec.sinks());
    std::vector<bool> autoStart;
    for (const Call& call : rec.calls()) {
        if (call.effect == Effect::AutoStart) autoStart.push_back(call.value);
    }
    const std::vector<bool> expected{false, true, false};
    EXPECT(autoStart == expected);
}

void testNoOpUpdateAppliesNothing() {
    TEST("store: an update that changes nothing applies nothing, and neither does one that "
         "changes a setting without a side effect");
    ConfigStore store(configWith(false, true), acceptingPersist());
    RecordingSinks rec;
    ConfigEffects effects(rec.sinks());
    effects.apply(store);
    EXPECT_EQ(rec.count(), size_t{2});

    store.update(unchanged);
    effects.apply(store);
    EXPECT_EQ(rec.count(), size_t{2});

    store.update(setPort);
    effects.apply(store);
    EXPECT_EQ(rec.count(), size_t{2});

    store.update([](Config& cfg) {
        cfg.autoStart = true;
        return true;
    });
    effects.apply(store);
    EXPECT_EQ(rec.count(), size_t{3});
    EXPECT(rec.calls().back() == (Call{Effect::AutoStart, true}));

    effects.apply(store);
    EXPECT_EQ(rec.count(), size_t{3});
}

class GatedSink {
  public:
    void call(bool on) {
        std::unique_lock<std::mutex> lk(mtx_);
        const bool first = !entered_;
        entered_ = true;
        changed_.notify_all();
        if (first) changed_.wait(lk, [this] { return released_; });
        finished_.push_back(on);
    }

    void waitEntered() {
        std::unique_lock<std::mutex> lk(mtx_);
        changed_.wait(lk, [this] { return entered_; });
    }

    void release() {
        std::lock_guard<std::mutex> lk(mtx_);
        released_ = true;
        changed_.notify_all();
    }

    std::vector<bool> finished() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return finished_;
    }

  private:
    mutable std::mutex mtx_;
    std::condition_variable changed_;
    bool entered_ = false;
    bool released_ = false;
    std::vector<bool> finished_;
};

void testLatestSnapshotWinsOverASlowerEarlierWriter() {
    TEST("store: a writer that applies late cannot undo a newer snapshot");
    ConfigStore store(configWith(false, true), acceptingPersist());
    GatedSink gate;
    ConfigEffects effects(ConfigEffectSinks{[&gate](bool on) { gate.call(on); }, [](bool) {}});

    std::thread first([&] {
        store.update([](Config& cfg) {
            cfg.autoStart = true;
            return true;
        });
        effects.apply(store);
    });
    gate.waitEntered();

    std::thread second([&] {
        store.update([](Config& cfg) {
            cfg.autoStart = false;
            return true;
        });
        effects.apply(store);
    });
    std::this_thread::sleep_for(WRITER_BLOCKED_FOR);
    EXPECT(gate.finished().empty());
    gate.release();
    first.join();
    second.join();

    const std::vector<bool> finished = gate.finished();
    EXPECT_EQ(finished.size(), size_t{2});
    if (finished.size() == 2) {
        EXPECT_EQ(finished[0], true);
        EXPECT_EQ(finished[1], false);
    }
    EXPECT(!store.current()->autoStart);
}

class ExclusiveSinks {
  public:
    ConfigEffectSinks sinks() {
        return ConfigEffectSinks{[this](bool on) { enter(autoStart_, on); },
                                 [this](bool on) { enter(crash_, on); }};
    }

    int maxInside() const { return maxInside_.load(); }
    bool last(Effect effect) const {
        return (effect == Effect::AutoStart ? autoStart_ : crash_).load();
    }

  private:
    void enter(std::atomic<bool>& last, bool on) {
        const int now = inside_.fetch_add(1) + 1;
        int seen = maxInside_.load();
        while (now > seen && !maxInside_.compare_exchange_weak(seen, now)) {}
        std::this_thread::yield();
        last.store(on);
        inside_.fetch_sub(1);
    }

    std::atomic<int> inside_{0};
    std::atomic<int> maxInside_{0};
    std::atomic<bool> autoStart_{false};
    std::atomic<bool> crash_{true};
};

void testRacingFlipsEndWithTheFinalSnapshotApplied() {
    TEST("store: threads racing updates that flip both effects end with the applied state equal "
         "to the final snapshot, one application at a time");
    ConfigStore store(configWith(false, true), acceptingPersist());
    ExclusiveSinks sinks;
    ConfigEffects effects(sinks.sinks());
    effects.apply(store);

    std::vector<std::thread> threads;
    for (int t = 0; t < RACING_THREADS; ++t) {
        threads.emplace_back([&, t] {
            for (int round = 0; round < ROUNDS_PER_THREAD; ++round) {
                store.update([round, t](Config& cfg) {
                    cfg.autoStart = !cfg.autoStart;
                    if ((round + t) % 2 == 0) cfg.crashReporting = !cfg.crashReporting;
                    return true;
                });
                effects.apply(store);
            }
        });
    }
    for (std::thread& thread : threads) thread.join();

    const std::shared_ptr<const Config> last = store.current();
    EXPECT_EQ(sinks.last(Effect::AutoStart), last->autoStart);
    EXPECT_EQ(sinks.last(Effect::CrashReporting), last->crashReporting);
    EXPECT_EQ(sinks.maxInside(), 1);
}

#ifdef SATELLITE_LOCK_STATS

using NameSet = std::set<std::string>;

class HeldProbe {
  public:
    ConfigEffectSinks sinks() {
        return ConfigEffectSinks{[this](bool) { probe(); }, [this](bool) { probe(); }};
    }

    int probes() const { return probes_.load(); }
    int wrongHolds() const { return wrongHolds_.load(); }

  private:
    void probe() {
        probes_++;
        const std::vector<std::string> held = lockstats::heldByThisThread();
        if (held != std::vector<std::string>{"config.effects"}) wrongHolds_++;
    }

    std::atomic<int> probes_{0};
    std::atomic<int> wrongHolds_{0};
};

void testSinksRunHoldingOnlyTheEffectsLock() {
    TEST("lock stats: every effect runs holding config.effects and nothing else, and apply "
         "returns holding nothing");
    ConfigStore store(configWith(false, true), acceptingPersist());
    HeldProbe probe;
    ConfigEffects effects(probe.sinks());
    effects.apply(store);
    store.update([](Config& cfg) {
        cfg.autoStart = true;
        cfg.crashReporting = false;
        return true;
    });
    effects.apply(store);
    EXPECT_EQ(probe.probes(), 4);
    EXPECT_EQ(probe.wrongHolds(), 0);
    EXPECT(lockstats::heldByThisThread().empty());
}

void testTheEffectsLockNestsOnlyAboveTheSnapshotLock() {
    TEST("lock stats: config.effects nests only above config.snapshot, and is held across the "
         "autostart write and the crash toggle");
    lockstats::reset();
    ConfigStore store(configWith(false, true), acceptingPersist());
    RecordingSinks rec;
    ConfigEffects effects(rec.sinks());
    effects.apply(store);
    const lockstats::Snapshot snap = lockstats::snapshot();

    std::set<std::pair<std::string, std::string>> edges;
    for (const lockstats::NamePair& edge : snap.edges) {
        if (edge.first == "config.effects" || edge.second == "config.effects") {
            edges.insert({edge.first, edge.second});
        }
    }
    const std::set<std::pair<std::string, std::string>> expected{
        {"config.effects", "config.snapshot"}};
    EXPECT(edges == expected);

    std::set<std::pair<std::string, std::string>> blocking;
    for (const lockstats::NamePair& pair : snap.blocking) {
        if (pair.second == "config.effects") blocking.insert({pair.first, pair.second});
    }
    const std::set<std::pair<std::string, std::string>> markers{
        {"autostart.write", "config.effects"}, {"crash.toggle", "config.effects"}};
    EXPECT(blocking == markers);
}

#endif

} // namespace

int main() {
    testFirstSightAppliesBothEffects();
    testUnchangedSnapshotAppliesNothing();
    testOnlyTheDifferingEffectIsApplied();
    testSettingsWithoutEffectsApplyNothing();
    testFlippingBackIsAppliedAgain();
    testNoOpUpdateAppliesNothing();
    testLatestSnapshotWinsOverASlowerEarlierWriter();
    testRacingFlipsEndWithTheFinalSnapshotApplied();
#ifdef SATELLITE_LOCK_STATS
    testSinksRunHoldingOnlyTheEffectsLock();
    testTheEffectsLockNestsOnlyAboveTheSnapshotLock();
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
