// SPDX-License-Identifier: LGPL-3.0-or-later
#include "httplib.h"

#ifdef _WIN32
#include <mmsystem.h>
#endif

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <thread>
#include <utility>

#include "test_util.h"

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

constexpr auto OBSERVE_FOR = 1000ms;
constexpr auto SHUTDOWN_LIMIT = 1000ms;
constexpr int MAX_IDLE_WAKEUPS = 25;
constexpr size_t POOL_THREADS = 2;
#ifdef _WIN32
constexpr int IDLE_INTERVAL_USEC = 100000;
constexpr int MIN_POLLED_WAKEUPS = 3;
#else
constexpr int IDLE_INTERVAL_USEC = 0;
#endif

class CountingQueue final : public httplib::TaskQueue {
  public:
    explicit CountingQueue(std::atomic<int>& idles) : idles_(idles), pool_(POOL_THREADS) {}

    bool enqueue(std::function<void()> fn) override { return pool_.enqueue(std::move(fn)); }
    void shutdown() override { pool_.shutdown(); }
    void on_idle() override { idles_.fetch_add(1); }

  private:
    std::atomic<int>& idles_;
    httplib::ThreadPool pool_;
};

void testTheIdleIntervalMacro() {
    TEST("the idle interval is 100 ms on Windows, where httplib polls every millisecond, and "
         "httplib's own blocking accept elsewhere");
    EXPECT_EQ(CPPHTTPLIB_IDLE_INTERVAL_USECOND, IDLE_INTERVAL_USEC);
}

void testAnIdleServerWakesAboutTenTimesASecond() {
    TEST("an idle server wakes at most about ten times a second, and stops within a second");
    std::atomic<int> idles{0};
    httplib::Server server;
    server.new_task_queue = [&idles] { return new CountingQueue(idles); };
    const int port = server.bind_to_any_port("127.0.0.1");
    EXPECT(port > 0);
    if (port <= 0) return;

    std::thread serving([&server] { server.listen_after_bind(); });
    server.wait_until_ready();
    std::this_thread::sleep_for(OBSERVE_FOR);
    const int wakeups = idles.load();

    const Clock::time_point stopping = Clock::now();
    server.stop();
    serving.join();
    const auto stopped =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - stopping);

    std::cout << "  idle wakeups in " << OBSERVE_FOR.count() << " ms: " << wakeups << ", stop took "
              << stopped.count() << " ms\n";
    EXPECT(wakeups <= MAX_IDLE_WAKEUPS);
#ifdef _WIN32
    EXPECT(wakeups >= MIN_POLLED_WAKEUPS);
#endif
    EXPECT(stopped < SHUTDOWN_LIMIT);
}

} // namespace

int main() {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }
    timeBeginPeriod(1);
#endif
    testTheIdleIntervalMacro();
    testAnIdleServerWakesAboutTenTimesASecond();
#ifdef _WIN32
    timeEndPeriod(1);
    WSACleanup();
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
