// SPDX-License-Identifier: LGPL-3.0-or-later
// A launch that finds another Satellite: one that answers its tap takes the
// click, one on its way out (no message loop, waiting for its helper) is waited
// for, one still starting is tapped once its window shows, and one that never
// exits is given up on. Each "other Satellite" is a thread holding the mutex,
// whose end lets it go as a process's exit does.
#include "test_util.h"

#include "single_instance.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;

long long elapsedMs(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

// Names of this run's own, so nothing here meets a Satellite on the machine.
struct TestNames {
    std::string mutex;
    std::string windowClass;
    std::string waiting;
    std::string title = "Satellite single instance test";
    lifecycle::SingleInstanceNames names() const {
        return {mutex.c_str(), windowClass.c_str(), title.c_str(), waiting.c_str()};
    }
};

TestNames namesFor(const std::string& test) {
    const std::string suffix = test + "." + std::to_string(GetCurrentProcessId());
    return {"Local\\TinkerNorth.Satellite.Test." + suffix, "SatelliteSingleInstanceTest." + suffix,
            "Local\\TinkerNorth.Satellite.TestWaiting." + suffix};
}

// Whether a launch still holds the right to wait, after every launch is done.
bool someoneWaits(const TestNames& n) {
    HANDLE probe = CreateMutexA(nullptr, FALSE, n.waiting.c_str());
    const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
    if (probe != nullptr) CloseHandle(probe);
    return existed;
}

// A tap that UIPI keeps out, as from a launch below a Satellite running
// elevated without UIAccess.
lifecycle::TapAnswer deniedTap(HWND, UINT, DWORD) { return lifecycle::TapAnswer::Denied; }

lifecycle::TapAnswer deniedTapProbeAnswered(HWND, UINT message, DWORD) {
    return message == WM_NULL ? lifecycle::TapAnswer::Answered : lifecycle::TapAnswer::Denied;
}

lifecycle::TapAnswer deniedTapProbeUnanswered(HWND, UINT message, DWORD) {
    return message == WM_NULL ? lifecycle::TapAnswer::NoAnswer : lifecycle::TapAnswer::Denied;
}

constexpr lifecycle::SingleInstanceWaits QUICK{200, 50, 5000};

std::atomic<int> g_taps{0};
// How long the other Satellite takes to answer a tap.
std::atomic<DWORD> g_answerDelayMs{0};

LRESULT CALLBACK countingProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == lifecycle::WM_SECOND_INSTANCE) {
        g_taps++;
        if (const DWORD delay = g_answerDelayMs.load()) Sleep(delay);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

HWND makeWindow(const TestNames& n) {
    WNDCLASSA wc{};
    wc.lpfnWndProc = countingProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = n.windowClass.c_str();
    RegisterClassA(&wc);
    return CreateWindowExA(WS_EX_TOOLWINDOW, wc.lpszClassName, n.title.c_str(), WS_OVERLAPPED, 0, 0,
                           0, 0, nullptr, nullptr, wc.hInstance, nullptr);
}

void pumpFor(DWORD ms) {
    const ULONGLONG until = GetTickCount64() + ms;
    while (GetTickCount64() < until) {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    }
}

// How the other Satellite behaves while it holds the singleton.
struct Other {
    DWORD windowAfterMs = 0; // when its window shows; ~0 for never
    bool pumps = false;      // whether its message loop runs
    DWORD holdMs = 0;        // how long until it exits
    DWORD loopAfterMs = 0;   // when its message loop starts, if it pumps
};

constexpr DWORD NEVER = ~DWORD{0};

// Another Satellite on a thread of its own, holding the singleton from the
// moment it is made until its thread ends.
class OtherInstance {
  public:
    OtherInstance(const TestNames& names, Other how)
        : names_(names), how_(how), thread_([this] { run(); }) {
        WaitForSingleObject(ready_, INFINITE);
    }
    ~OtherInstance() {
        thread_.join();
        CloseHandle(ready_);
    }
    OtherInstance(const OtherInstance&) = delete;
    OtherInstance& operator=(const OtherInstance&) = delete;

  private:
    void run() {
        HANDLE mutex = CreateMutexA(nullptr, TRUE, names_.mutex.c_str());
        SetEvent(ready_);
        HWND window = nullptr;
        DWORD at = 0;
        if (how_.windowAfterMs != NEVER) {
            Sleep(how_.windowAfterMs);
            at = how_.windowAfterMs;
            window = makeWindow(names_);
        }
        if (how_.pumps && how_.loopAfterMs > at) {
            Sleep(how_.loopAfterMs - at);
            at = how_.loopAfterMs;
        }
        const DWORD rest = how_.holdMs > at ? how_.holdMs - at : 0;
        if (how_.pumps) {
            pumpFor(rest);
        } else {
            // A Satellite on its way out never runs its loop again, so it
            // answers nothing, a probe included, until it is gone.
            Sleep(rest);
        }
        if (window != nullptr) DestroyWindow(window);
        CloseHandle(mutex);
    }

    TestNames names_;
    Other how_;
    HANDLE ready_ = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    std::thread thread_;
};

void release(HANDLE held) {
    if (held == nullptr) return;
    ReleaseMutex(held);
    CloseHandle(held);
}

} // namespace

static void test_the_first_launch_takes_the_singleton() {
    TEST("single instance: the first launch takes the singleton, and holds it against a later one");
    const TestNames n = namesFor("first");
    HANDLE held = nullptr;
    EXPECT(lifecycle::acquireSingleInstance(n.names(), QUICK, held));
    EXPECT(held != nullptr);
    bool later = true;
    std::thread second([&n, &later] {
        HANDLE mine = nullptr;
        later = lifecycle::acquireSingleInstance(n.names(), {50, 20, 300}, mine);
        if (mine != nullptr) CloseHandle(mine);
    });
    second.join();
    EXPECT(!later);
    release(held);
}

static void test_a_live_satellite_takes_the_click() {
    TEST("single instance: a launch while a live Satellite answers its tap ends at once, and the "
         "live one gets the one tap");
    const TestNames n = namesFor("live");
    g_taps = 0;
    OtherInstance other(n, {0, true, 1500});
    Sleep(100);
    HANDLE held = nullptr;
    const auto start = Clock::now();
    EXPECT(!lifecycle::acquireSingleInstance(n.names(), QUICK, held));
    EXPECT(elapsedMs(start) < 1000);
    EXPECT(held == nullptr);
    EXPECT_EQ(g_taps.load(), 1);
}

static void test_a_satellite_on_its_way_out_is_waited_for() {
    TEST("single instance: a launch while a Satellite is on its way out, its window answering "
         "nothing, waits for it to exit and then starts, rather than ending with nothing shown");
    const TestNames n = namesFor("exiting");
    g_taps = 0;
    HANDLE held = nullptr;
    bool started = false;
    long long tookMs = 0;
    {
        OtherInstance other(n, {0, false, 800});
        Sleep(50);
        const auto start = Clock::now();
        started = lifecycle::acquireSingleInstance(n.names(), QUICK, held);
        tookMs = elapsedMs(start);
    }
    EXPECT(started);
    EXPECT(held != nullptr);
    EXPECT(tookMs >= 600);
    EXPECT(tookMs < 4000);
    EXPECT(!someoneWaits(n));
    release(held);
}

static void test_a_late_answer_still_ends_the_launch() {
    TEST("single instance: a Satellite that answers a tap later than the tap waits is a running "
         "one: the launch ends once it answers a probe, before it exits, and it gets the one tap");
    const TestNames n = namesFor("slow");
    g_taps = 0;
    g_answerDelayMs = 400;
    HANDLE held = nullptr;
    bool started = true;
    long long tookMs = 0;
    {
        OtherInstance other(n, {0, true, 1500});
        Sleep(50);
        const auto start = Clock::now();
        started = lifecycle::acquireSingleInstance(n.names(), QUICK, held);
        tookMs = elapsedMs(start);
    }
    g_answerDelayMs = 0;
    EXPECT(!started);
    EXPECT(held == nullptr);
    EXPECT(tookMs < 1300);
    EXPECT_EQ(g_taps.load(), 1);
    EXPECT(!someoneWaits(n));
}

static void test_a_satellite_whose_loop_starts_late_is_not_taken_for_one_exiting() {
    TEST("single instance: a launch while another Satellite has its window but not yet its "
         "message loop, as at login, ends once that loop answers, and never starts a second one");
    const TestNames n = namesFor("loop-late");
    g_taps = 0;
    HANDLE held = nullptr;
    bool started = true;
    long long tookMs = 0;
    {
        OtherInstance other(n, {0, true, 4500, 3000});
        Sleep(50);
        const auto start = Clock::now();
        started = lifecycle::acquireSingleInstance(n.names(), QUICK, held);
        tookMs = elapsedMs(start);
    }
    EXPECT(!started);
    EXPECT(held == nullptr);
    EXPECT(tookMs >= 2500);
    EXPECT(tookMs < 4200);
    TEST("single instance: that Satellite is tapped once, then only probed");
    EXPECT(g_taps.load() <= 1);
    EXPECT(!someoneWaits(n));
}

static void test_a_tap_that_uipi_refuses_ends_the_launch() {
    TEST("single instance: a tap UIPI refuses, as to a Satellite running elevated without "
         "UIAccess, is a running Satellite: the launch ends at once and never waits");
    const TestNames n = namesFor("uipi");
    HANDLE held = nullptr;
    bool started = true;
    long long tookMs = 0;
    {
        OtherInstance other(n, {0, false, 1500});
        Sleep(50);
        const auto start = Clock::now();
        started = lifecycle::acquireSingleInstance(n.names(), QUICK, held, deniedTap);
        tookMs = elapsedMs(start);
    }
    EXPECT(!started);
    EXPECT(held == nullptr);
    EXPECT(tookMs < 150);
    EXPECT(!someoneWaits(n));
}

static void test_a_refused_tap_is_followed_by_a_probe() {
    TEST("single instance: a tap UIPI refuses is followed by a probe UIPI admits: an elevated "
         "Satellite that answers the probe is running, and the launch ends at once");
    const TestNames n = namesFor("uipi-probe-live");
    HANDLE held = nullptr;
    bool started = true;
    long long tookMs = 0;
    {
        OtherInstance other(n, {0, false, 1500});
        Sleep(50);
        const auto start = Clock::now();
        started = lifecycle::acquireSingleInstance(n.names(), QUICK, held, deniedTapProbeAnswered);
        tookMs = elapsedMs(start);
    }
    EXPECT(!started);
    EXPECT(held == nullptr);
    EXPECT(tookMs < 150);
    EXPECT(!someoneWaits(n));

    TEST("single instance: an elevated Satellite that answers the probe no more is on its way "
         "out, and the launch waits for it and then starts");
    const TestNames m = namesFor("uipi-probe-exiting");
    started = false;
    {
        OtherInstance other(m, {0, false, 800});
        Sleep(50);
        const auto start = Clock::now();
        started =
            lifecycle::acquireSingleInstance(m.names(), QUICK, held, deniedTapProbeUnanswered);
        tookMs = elapsedMs(start);
    }
    EXPECT(started);
    EXPECT(held != nullptr);
    EXPECT(tookMs >= 600);
    EXPECT(tookMs < 4000);
    EXPECT(!someoneWaits(m));
    release(held);
}

static void test_only_one_launch_waits() {
    TEST("single instance: while one launch waits for a Satellite on its way out, a later launch "
         "ends at once instead of waiting too");
    const TestNames n = namesFor("one-waiter");
    HANDLE firstHeld = nullptr;
    bool firstStarted = false;
    bool laterStarted = true;
    long long laterMs = 0;
    {
        OtherInstance other(n, {NEVER, false, 1500});
        std::thread first([&n, &firstHeld, &firstStarted] {
            firstStarted = lifecycle::acquireSingleInstance(n.names(), QUICK, firstHeld);
            if (firstStarted) {
                ReleaseMutex(firstHeld);
                CloseHandle(firstHeld);
            }
        });
        Sleep(300);
        HANDLE laterHeld = nullptr;
        const auto start = Clock::now();
        laterStarted = lifecycle::acquireSingleInstance(n.names(), QUICK, laterHeld);
        laterMs = elapsedMs(start);
        if (laterHeld != nullptr) release(laterHeld);
        first.join();
    }
    EXPECT(!laterStarted);
    EXPECT(laterMs < 600);
    TEST("single instance: the launch that waited starts once the old Satellite is gone, and lets "
         "the right to wait go");
    EXPECT(firstStarted);
    EXPECT(!someoneWaits(n));
}

static void test_a_satellite_still_starting_is_tapped_once_its_window_shows() {
    TEST("single instance: a launch while another Satellite is still starting, its window not "
         "shown yet, taps the window once it shows, and that one takes the click");
    const TestNames n = namesFor("starting");
    g_taps = 0;
    HANDLE held = nullptr;
    bool started = true;
    long long tookMs = 0;
    {
        OtherInstance other(n, {300, true, 1500});
        const auto start = Clock::now();
        started = lifecycle::acquireSingleInstance(n.names(), QUICK, held);
        tookMs = elapsedMs(start);
    }
    EXPECT(!started);
    EXPECT(held == nullptr);
    EXPECT(tookMs >= 200);
    EXPECT(tookMs < 1400);
    EXPECT_EQ(g_taps.load(), 1);
}

static void test_a_satellite_that_never_exits_is_given_up_on() {
    TEST("single instance: a launch gives up at its bound on a Satellite that answers nothing and "
         "never exits, and ends, so two never run");
    const TestNames n = namesFor("hung");
    HANDLE held = nullptr;
    bool started = true;
    long long tookMs = 0;
    {
        OtherInstance other(n, {NEVER, false, 2000});
        const auto start = Clock::now();
        started = lifecycle::acquireSingleInstance(n.names(), {200, 50, 600}, held);
        tookMs = elapsedMs(start);
    }
    EXPECT(!started);
    EXPECT(held == nullptr);
    EXPECT(tookMs >= 550);
    EXPECT(tookMs < 1500);
}

int main() {
    std::cout << "=== test_single_instance ===\n";
    test_the_first_launch_takes_the_singleton();
    test_a_live_satellite_takes_the_click();
    test_a_satellite_on_its_way_out_is_waited_for();
    test_a_late_answer_still_ends_the_launch();
    test_a_satellite_whose_loop_starts_late_is_not_taken_for_one_exiting();
    test_a_tap_that_uipi_refuses_ends_the_launch();
    test_a_refused_tap_is_followed_by_a_probe();
    test_only_one_launch_waits();
    test_a_satellite_still_starting_is_tapped_once_its_window_shows();
    test_a_satellite_that_never_exits_is_given_up_on();

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
