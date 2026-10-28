// SPDX-License-Identifier: LGPL-3.0-or-later
#include "test_util.h"

#include "../src/platform/windows/audio_guard_worker.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <mutex>
#include <set>
#include <thread>

using namespace satellite::audioguard;
using std::chrono::milliseconds;

namespace {

using Clock = std::chrono::steady_clock;

long long elapsedMs(Clock::time_point since) {
    return std::chrono::duration_cast<milliseconds>(Clock::now() - since).count();
}

bool waitUntil(const std::function<bool()>& done, int limitMs) {
    const auto start = Clock::now();
    while (!done()) {
        if (elapsedMs(start) > limitMs) return false;
        std::this_thread::sleep_for(milliseconds(2));
    }
    return true;
}

class FakeComBackend : public GuardBackend {
  public:
    void snapshotBeforePlug() override {
        noteThread();
        std::unique_lock<std::mutex> lk(mtx_);
        snapshotsStarted_++;
        cv_.wait(lk, [this] { return !blockSnapshots_; });
        std::this_thread::sleep_for(snapshotDelay_);
        snapshotsDone_++;
    }

    RestorePass restoreIfStolen() override {
        noteThread();
        const int pass = ++restores_;
        RestorePass out;
        out.keepPolling = keepPolling_.load() && pass < stopAfter_.load();
        return out;
    }

    void blockSnapshots(bool block) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            blockSnapshots_ = block;
        }
        cv_.notify_all();
    }

    int snapshotsStarted() const { return snapshotsStarted_.load(); }
    int snapshotsDone() const { return snapshotsDone_.load(); }
    int restores() const { return restores_.load(); }

    std::set<std::thread::id> threads() const {
        std::lock_guard<std::mutex> lk(threadsMtx_);
        return threads_;
    }

    void setSnapshotDelay(milliseconds delay) { snapshotDelay_ = delay; }
    void setKeepPolling(bool keep) { keepPolling_.store(keep); }
    void setStopAfter(int passes) { stopAfter_.store(passes); }

  private:
    void noteThread() {
        std::lock_guard<std::mutex> lk(threadsMtx_);
        threads_.insert(std::this_thread::get_id());
    }

    std::mutex mtx_;
    std::condition_variable cv_;
    bool blockSnapshots_ = false;
    std::atomic<int> snapshotsStarted_{0};
    std::atomic<int> snapshotsDone_{0};
    std::atomic<int> restores_{0};
    std::atomic<bool> keepPolling_{true};
    std::atomic<int> stopAfter_{1 << 30};
    milliseconds snapshotDelay_{0};
    mutable std::mutex threadsMtx_;
    std::set<std::thread::id> threads_;
};

GuardTiming quickTiming() {
    GuardTiming timing;
    timing.snapshotDeadline = milliseconds(2000);
    timing.pollInterval = milliseconds(5);
    timing.pollBudget = 4;
    return timing;
}

} // namespace

static void test_the_snapshot_is_taken_on_the_worker_before_the_plug_goes_on() {
    TEST("snapshot: taken on the worker thread, and finished when the plug thread goes on");
    FakeComBackend backend;
    backend.setSnapshotDelay(milliseconds(40));
    GuardWorker worker(backend, {}, quickTiming());
    worker.beforeCompositePlug();
    EXPECT_EQ(backend.snapshotsDone(), 1);
    const std::set<std::thread::id> seen = backend.threads();
    EXPECT_EQ(seen.size(), static_cast<size_t>(1));
    EXPECT(seen.count(std::this_thread::get_id()) == 0);
}

static void test_the_restore_polls_run_on_the_same_worker() {
    TEST("restore polls: run on the worker thread, never on the plug thread");
    FakeComBackend backend;
    GuardWorker worker(backend, {}, quickTiming());
    worker.beforeCompositePlug();
    worker.afterCompositePlug();
    EXPECT(waitUntil([&] { return backend.restores() >= 1; }, 5000));
    const std::set<std::thread::id> seen = backend.threads();
    EXPECT_EQ(seen.size(), static_cast<size_t>(1));
    EXPECT(seen.count(std::this_thread::get_id()) == 0);
}

static void test_a_stuck_snapshot_costs_the_plug_the_deadline_and_no_more() {
    TEST("deadline: a snapshot that does not finish lets the plug go on at the deadline");
    FakeComBackend backend;
    backend.blockSnapshots(true);
    GuardTiming timing = quickTiming();
    timing.snapshotDeadline = milliseconds(120);
    GuardWorker worker(backend, {}, timing);
    const auto start = Clock::now();
    worker.beforeCompositePlug();
    const long long took = elapsedMs(start);
    EXPECT(took >= 100);
    EXPECT(took < 3000);
    EXPECT_EQ(backend.snapshotsDone(), 0);

    TEST("deadline: the snapshot finishes whenever the audio service answers, and the next plug "
         "is served");
    backend.blockSnapshots(false);
    EXPECT(waitUntil([&] { return backend.snapshotsDone() == 1; }, 5000));
    worker.beforeCompositePlug();
    EXPECT_EQ(backend.snapshotsDone(), 2);
}

static void test_a_plug_waiting_on_a_snapshot_holds_no_lock() {
    TEST("no lock: while a plug waits on a stuck snapshot, the other entry point is not blocked");
    FakeComBackend backend;
    backend.blockSnapshots(true);
    GuardTiming timing = quickTiming();
    timing.snapshotDeadline = milliseconds(3000);
    GuardWorker worker(backend, {}, timing);
    std::atomic<bool> returned{false};
    std::thread plug([&] {
        worker.beforeCompositePlug();
        returned.store(true);
    });
    EXPECT(waitUntil([&] { return backend.snapshotsStarted() == 1; }, 5000));

    const auto start = Clock::now();
    worker.afterCompositePlug();
    EXPECT(elapsedMs(start) < 500);
    EXPECT(!returned.load());

    backend.blockSnapshots(false);
    plug.join();
    EXPECT(returned.load());
    EXPECT_EQ(backend.snapshotsDone(), 1);
}

static void test_the_window_polls_its_budget_and_no_more() {
    TEST("window: polls on, up to the budget, while a role could still be restored");
    FakeComBackend backend;
    GuardWorker worker(backend, {}, quickTiming());
    worker.beforeCompositePlug();
    worker.afterCompositePlug();
    EXPECT(waitUntil([&] { return backend.restores() == 4; }, 5000));
    std::this_thread::sleep_for(milliseconds(80));
    EXPECT_EQ(backend.restores(), 4);

    TEST("window: stops at the first pass with nothing left to restore");
    FakeComBackend finished;
    finished.setStopAfter(2);
    GuardWorker second(finished, {}, quickTiming());
    second.beforeCompositePlug();
    second.afterCompositePlug();
    EXPECT(waitUntil([&] { return finished.restores() == 2; }, 5000));
    std::this_thread::sleep_for(milliseconds(80));
    EXPECT_EQ(finished.restores(), 2);
}

static void test_a_plug_inside_an_open_window_keeps_its_snapshot() {
    TEST("window: a plug that lands while the earlier window is open takes no fresh snapshot");
    FakeComBackend backend;
    GuardTiming timing = quickTiming();
    timing.pollInterval = milliseconds(100);
    timing.pollBudget = 5;
    GuardWorker worker(backend, {}, timing);
    worker.beforeCompositePlug();
    worker.afterCompositePlug();
    EXPECT(waitUntil([&] { return backend.restores() >= 1; }, 5000));
    worker.beforeCompositePlug();
    EXPECT_EQ(backend.snapshotsDone(), 1);

    TEST("window: once the window has closed the next plug snapshots again");
    EXPECT(waitUntil([&] { return backend.restores() == 5; }, 5000));
    EXPECT(waitUntil(
        [&] {
            worker.beforeCompositePlug();
            return backend.snapshotsDone() == 2;
        },
        5000));
}

static void test_a_second_plug_restarts_the_budget() {
    TEST("window: a plug that arms mid-window restarts the budget from that plug");
    FakeComBackend backend;
    GuardTiming timing = quickTiming();
    timing.pollInterval = milliseconds(30);
    timing.pollBudget = 4;
    GuardWorker worker(backend, {}, timing);
    worker.beforeCompositePlug();
    worker.afterCompositePlug();
    EXPECT(waitUntil([&] { return backend.restores() >= 2; }, 5000));
    worker.afterCompositePlug();
    EXPECT(waitUntil([&] { return backend.restores() >= 5; }, 5000));
    EXPECT(waitUntil([&] { return backend.restores() == 6; }, 5000));
    std::this_thread::sleep_for(milliseconds(120));
    EXPECT(backend.restores() >= 6);
    EXPECT(backend.restores() <= 8);
}

static void test_the_setting_that_turns_the_guard_off_is_honoured() {
    TEST("setting: with the guard off no plug touches the backend");
    FakeComBackend backend;
    std::atomic<bool> on{false};
    GuardWorker worker(backend, [&] { return on.load(); }, quickTiming());
    worker.beforeCompositePlug();
    worker.afterCompositePlug();
    std::this_thread::sleep_for(milliseconds(80));
    EXPECT_EQ(backend.snapshotsStarted(), 0);
    EXPECT_EQ(backend.restores(), 0);

    TEST("setting: turned back on, the next plug is guarded");
    on.store(true);
    worker.beforeCompositePlug();
    EXPECT_EQ(backend.snapshotsDone(), 1);
}

static void test_destroying_the_worker_ends_an_open_window() {
    TEST("shutdown: destroying the worker ends an open window without waiting out its polls");
    FakeComBackend backend;
    GuardTiming timing = quickTiming();
    timing.pollInterval = milliseconds(5000);
    timing.pollBudget = 20;
    const auto start = Clock::now();
    {
        GuardWorker worker(backend, {}, timing);
        worker.beforeCompositePlug();
        worker.afterCompositePlug();
        EXPECT(waitUntil([&] { return backend.restores() >= 1; }, 5000));
    }
    EXPECT(elapsedMs(start) < 3000);
}

int main() {
    test_the_snapshot_is_taken_on_the_worker_before_the_plug_goes_on();
    test_the_restore_polls_run_on_the_same_worker();
    test_a_stuck_snapshot_costs_the_plug_the_deadline_and_no_more();
    test_a_plug_waiting_on_a_snapshot_holds_no_lock();
    test_the_window_polls_its_budget_and_no_more();
    test_a_plug_inside_an_open_window_keeps_its_snapshot();
    test_a_second_plug_restarts_the_budget();
    test_the_setting_that_turns_the_guard_off_is_honoured();
    test_destroying_the_worker_ends_an_open_window();

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
