// SPDX-License-Identifier: LGPL-3.0-or-later
#include "core/event_broadcaster.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "test_util.h"

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using satellite::EventBroadcaster;
using satellite::EventBroadcasterOptions;
using Frame = EventBroadcaster::Frame;
using Subscription = EventBroadcaster::Subscription;

constexpr size_t STREAM_CAP = 8;
constexpr int STREAM_COUNT = 8;
constexpr int FRAMES_EACH = 40;
constexpr auto TICK_PERIOD = 5ms;
constexpr auto NO_TICKS = 1h;
constexpr auto NOT_YET = 60ms;
constexpr auto WAIT_LIMIT = 10s;
constexpr auto IDLE_SETTLE = 40ms;
constexpr auto IDLE_WATCH = 60ms;

EventBroadcasterOptions manual() { return EventBroadcasterOptions{STREAM_CAP, NO_TICKS, false}; }

EventBroadcasterOptions timed() { return EventBroadcasterOptions{STREAM_CAP, TICK_PERIOD, true}; }

#ifdef SATELLITE_LOCK_STATS
bool onBackgroundRole() { return currentThreadRole() == ThreadRole::Background; }
size_t locksHeldHere() { return lockstats::heldByThisThread().size(); }
#else
bool onBackgroundRole() { return true; }
size_t locksHeldHere() { return 0; }
#endif

std::string text(const Frame& frame) { return frame ? *frame : std::string("<none>"); }

class CountingBuild {
  public:
    explicit CountingBuild(bool expectBackground = false) : expectBackground_(expectBackground) {}

    std::string operator()() {
        if (locksHeldHere() != 0) lockHeld_++;
        if (expectBackground_ && !onBackgroundRole()) wrongRole_++;
        return "frame-" + std::to_string(++builds_);
    }

    int builds() const { return builds_.load(); }
    int lockHeld() const { return lockHeld_.load(); }
    int wrongRole() const { return wrongRole_.load(); }

  private:
    const bool expectBackground_;
    std::atomic<int> builds_{0};
    std::atomic<int> lockHeld_{0};
    std::atomic<int> wrongRole_{0};
};

class PendingNext {
  public:
    explicit PendingNext(Subscription& stream)
        : thread_([this, &stream] {
              frame_ = stream.next();
              done_.store(true);
          }) {}

    ~PendingNext() { thread_.join(); }
    PendingNext(const PendingNext&) = delete;
    PendingNext& operator=(const PendingNext&) = delete;

    bool waitDone(std::chrono::milliseconds limit) const {
        const Clock::time_point deadline = Clock::now() + limit;
        while (!done_.load() && Clock::now() < deadline) std::this_thread::sleep_for(1ms);
        return done_.load();
    }

    Frame frame() const { return frame_; }

  private:
    Frame frame_;
    std::atomic<bool> done_{false};
    std::thread thread_;
};

void testOneBuildPerTickWhateverTheSubscriberCount() {
    TEST("a tick builds once whatever the number of streams, and every stream is handed that "
         "same frame");
    CountingBuild build;
    EventBroadcaster hub([&build] { return build(); }, manual());
    EXPECT(!hub.tick());
    EXPECT_EQ(build.builds(), 0);

    std::vector<std::unique_ptr<Subscription>> streams;
    streams.push_back(hub.subscribe());
    EXPECT(hub.tick());
    EXPECT_EQ(build.builds(), 1);
    for (int i = 1; i < STREAM_COUNT; ++i) streams.push_back(hub.subscribe());
    for (int tick = 0; tick < 5; ++tick) EXPECT(hub.tick());
    EXPECT_EQ(build.builds(), 6);

    std::vector<Frame> frames;
    for (const std::unique_ptr<Subscription>& stream : streams) {
        EXPECT(stream != nullptr);
        if (stream) frames.push_back(stream->next());
    }
    EXPECT_EQ(frames.size(), static_cast<size_t>(STREAM_COUNT));
    for (const Frame& frame : frames) {
        EXPECT_EQ(text(frame), std::string("frame-6"));
        EXPECT(frame == frames.front());
    }
    EXPECT_EQ(build.builds(), 6);
    hub.close();
}

void testASlowSubscriberSkipsToTheLatestFrame() {
    TEST("a slow subscriber skips to the latest frame instead of replaying the ones it missed");
    CountingBuild build;
    EventBroadcaster hub([&build] { return build(); }, manual());
    const std::unique_ptr<Subscription> fast = hub.subscribe();
    const std::unique_ptr<Subscription> slow = hub.subscribe();
    EXPECT(fast != nullptr && slow != nullptr);
    if (!fast || !slow) return;

    std::vector<std::string> fastSaw;
    for (int tick = 0; tick < 4; ++tick) {
        EXPECT(hub.tick());
        fastSaw.push_back(text(fast->next()));
    }
    const std::vector<std::string> everyFrame{"frame-1", "frame-2", "frame-3", "frame-4"};
    EXPECT(fastSaw == everyFrame);

    EXPECT_EQ(text(slow->next()), std::string("frame-4"));
    PendingNext caughtUp(*slow);
    EXPECT(!caughtUp.waitDone(NOT_YET));
    EXPECT(hub.tick());
    EXPECT(caughtUp.waitDone(WAIT_LIMIT));
    EXPECT_EQ(text(caughtUp.frame()), std::string("frame-5"));
    EXPECT_EQ(build.builds(), 5);
    hub.close();
}

void testALateSubscriberIsHandedTheCurrentFrameAtOnce() {
    TEST("a stream that joins while others are live is handed the current frame at once");
    CountingBuild build;
    EventBroadcaster hub([&build] { return build(); }, manual());
    const std::unique_ptr<Subscription> first = hub.subscribe();
    for (int tick = 0; tick < 3; ++tick) EXPECT(hub.tick());
    const std::unique_ptr<Subscription> late = hub.subscribe();
    EXPECT(late != nullptr);
    if (!late) return;
    PendingNext joined(*late);
    EXPECT(joined.waitDone(WAIT_LIMIT));
    EXPECT_EQ(text(joined.frame()), std::string("frame-3"));
    EXPECT_EQ(build.builds(), 3);
    hub.close();
}

void testTheFirstSubscriberAfterAnIdleSpellWaitsForAFreshFrame() {
    TEST("the first stream after an idle spell waits for a fresh frame, not the one left from "
         "before, and nothing is built while nobody listens");
    CountingBuild build;
    EventBroadcaster hub([&build] { return build(); }, manual());
    {
        const std::unique_ptr<Subscription> before = hub.subscribe();
        EXPECT(hub.tick());
        EXPECT_EQ(text(before->next()), std::string("frame-1"));
    }
    EXPECT(!hub.tick());
    EXPECT(!hub.tick());
    EXPECT_EQ(build.builds(), 1);

    const std::unique_ptr<Subscription> after = hub.subscribe();
    EXPECT(after != nullptr);
    if (!after) return;
    PendingNext waiting(*after);
    EXPECT(!waiting.waitDone(NOT_YET));
    EXPECT(hub.tick());
    EXPECT(waiting.waitDone(WAIT_LIMIT));
    EXPECT_EQ(text(waiting.frame()), std::string("frame-2"));
    hub.close();
}

void testTheCapHolds() {
    TEST("the ninth concurrent stream is refused, and a slot frees when a stream leaves");
    CountingBuild build;
    EventBroadcaster hub([&build] { return build(); }, manual());
    std::vector<std::unique_ptr<Subscription>> streams;
    for (size_t i = 0; i < STREAM_CAP; ++i) {
        streams.push_back(hub.subscribe());
        EXPECT(streams.back() != nullptr);
    }
    EXPECT(hub.subscribe() == nullptr);
    EXPECT(hub.subscribe() == nullptr);
    streams.pop_back();
    const std::unique_ptr<Subscription> replacement = hub.subscribe();
    EXPECT(replacement != nullptr);
    EXPECT(hub.subscribe() == nullptr);
    hub.close();
}

void testCloseWakesBlockedStreamsAndRefusesNewOnes() {
    TEST("closing wakes a stream that is waiting for a frame, and refuses new streams");
    CountingBuild build;
    EventBroadcaster hub([&build] { return build(); }, manual());
    const std::unique_ptr<Subscription> stream = hub.subscribe();
    EXPECT(stream != nullptr);
    if (!stream) return;
    PendingNext waiting(*stream);
    EXPECT(!waiting.waitDone(NOT_YET));
    hub.close();
    EXPECT(waiting.waitDone(WAIT_LIMIT));
    EXPECT(waiting.frame() == nullptr);
    EXPECT(hub.subscribe() == nullptr);
    EXPECT(!hub.tick());
}

void testAStreamOutlivingTheBroadcasterIsHarmless() {
    TEST("a stream that outlives its broadcaster is told it is closed and releases cleanly");
    CountingBuild build;
    std::unique_ptr<Subscription> stream;
    {
        EventBroadcaster hub([&build] { return build(); }, manual());
        stream = hub.subscribe();
    }
    EXPECT(stream != nullptr);
    if (!stream) return;
    EXPECT(stream->next() == nullptr);
    stream.reset();
}

void testTheThreadBuildsOncePerPeriodForAnyNumberOfStreams() {
    TEST("the broadcaster thread builds once per period for every stream, on the Background "
         "role holding no lock, and never waits for a stream that does not read");
    CountingBuild build(true);
    EventBroadcaster hub([&build] { return build(); }, timed());
    const std::unique_ptr<Subscription> stalled = hub.subscribe();
    EXPECT(stalled != nullptr);

    std::atomic<int> delivered{0};
    std::vector<std::thread> viewers;
    for (int i = 1; i < STREAM_COUNT; ++i) {
        viewers.emplace_back([&hub, &delivered] {
            const std::unique_ptr<Subscription> stream = hub.subscribe();
            if (!stream) return;
            for (int frame = 0; frame < FRAMES_EACH; ++frame) {
                if (!stream->next()) return;
                delivered++;
            }
        });
    }
    for (std::thread& viewer : viewers) viewer.join();

    EXPECT_EQ(delivered.load(), (STREAM_COUNT - 1) * FRAMES_EACH);
    EXPECT(build.builds() >= FRAMES_EACH);
    EXPECT(build.builds() * 2 < delivered.load());
    EXPECT_EQ(build.lockHeld(), 0);
    EXPECT_EQ(build.wrongRole(), 0);
    hub.close();
}

void testTheThreadIdlesWhenTheLastStreamLeaves() {
    TEST("the broadcaster thread stops building when the last stream leaves, and builds a fresh "
         "frame at once for the next one");
    CountingBuild build(true);
    EventBroadcaster hub([&build] { return build(); }, timed());
    {
        const std::unique_ptr<Subscription> stream = hub.subscribe();
        EXPECT(stream != nullptr);
        if (!stream) return;
        EXPECT(stream->next() != nullptr);
    }
    std::this_thread::sleep_for(IDLE_SETTLE);
    const int settled = build.builds();
    std::this_thread::sleep_for(IDLE_WATCH);
    EXPECT_EQ(build.builds(), settled);

    const std::unique_ptr<Subscription> again = hub.subscribe();
    EXPECT(again != nullptr);
    if (!again) return;
    PendingNext fresh(*again);
    EXPECT(fresh.waitDone(WAIT_LIMIT));
    EXPECT_EQ(text(fresh.frame()), "frame-" + std::to_string(settled + 1));
    hub.close();
}

#ifdef SATELLITE_LOCK_STATS

void testTheFrameLockNestsWithNothing() {
    TEST("lock stats: events.broadcast is taken on its own, never above or below another lock");
    lockstats::reset();
    CountingBuild build(true);
    EventBroadcaster hub([&build] { return build(); }, timed());
    std::vector<std::thread> viewers;
    for (int i = 0; i < 3; ++i) {
        viewers.emplace_back([&hub] {
            const std::unique_ptr<Subscription> stream = hub.subscribe();
            for (int frame = 0; stream && frame < 5; ++frame) stream->next();
        });
    }
    for (std::thread& viewer : viewers) viewer.join();
    hub.close();

    const lockstats::Snapshot snap = lockstats::snapshot();
    int nested = 0;
    for (const lockstats::NamePair& edge : snap.edges) {
        if (edge.first == "events.broadcast" || edge.second == "events.broadcast") nested++;
    }
    EXPECT_EQ(nested, 0);
    const lockstats::LockStats* lock = snap.find("events.broadcast");
    EXPECT(lock != nullptr && lock->role(ThreadRole::Background).acquisitions > 0);
}

#endif

} // namespace

int main() {
    testOneBuildPerTickWhateverTheSubscriberCount();
    testASlowSubscriberSkipsToTheLatestFrame();
    testALateSubscriberIsHandedTheCurrentFrameAtOnce();
    testTheFirstSubscriberAfterAnIdleSpellWaitsForAFreshFrame();
    testTheCapHolds();
    testCloseWakesBlockedStreamsAndRefusesNewOnes();
    testAStreamOutlivingTheBroadcasterIsHarmless();
    testTheThreadBuildsOncePerPeriodForAnyNumberOfStreams();
    testTheThreadIdlesWhenTheLastStreamLeaves();
#ifdef SATELLITE_LOCK_STATS
    testTheFrameLockNestsWithNothing();
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
