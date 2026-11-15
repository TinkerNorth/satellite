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
    std::string title = "Satellite single instance test";
    lifecycle::SingleInstanceNames names() const {
        return {mutex.c_str(), windowClass.c_str(), title.c_str()};
    }
};

TestNames namesFor(const std::string& test) {
    const std::string suffix = test + "." + std::to_string(GetCurrentProcessId());
    return {"Local\\TinkerNorth.Satellite.Test." + suffix, "SatelliteSingleInstanceTest." + suffix};
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
        DWORD shownAt = 0;
        if (how_.windowAfterMs != NEVER) {
            Sleep(how_.windowAfterMs);
            shownAt = how_.windowAfterMs;
            window = makeWindow(names_);
        }
        const DWORD rest = how_.holdMs > shownAt ? how_.holdMs - shownAt : 0;
        if (how_.pumps) {
            pumpFor(rest);
        } else {
            Sleep(rest);
            // Delivers what was sent to it meanwhile, so the taps can be counted.
            MSG msg;
            while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
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
    TEST("single instance: the window that answered nothing is tapped once, not on every wait");
    EXPECT(g_taps.load() <= 1);
    release(held);
}

static void test_a_slow_answer_is_tapped_once() {
    TEST("single instance: a Satellite that answers a tap later than the tap waits is tapped once, "
         "not on every wait, so it opens one dashboard, and the launch waits for it to exit");
    const TestNames n = namesFor("slow");
    g_taps = 0;
    g_answerDelayMs = 400;
    HANDLE held = nullptr;
    bool started = false;
    {
        OtherInstance other(n, {0, true, 1200});
        Sleep(50);
        started = lifecycle::acquireSingleInstance(n.names(), QUICK, held);
    }
    g_answerDelayMs = 0;
    EXPECT(started);
    EXPECT_EQ(g_taps.load(), 1);
    release(held);
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
    test_a_slow_answer_is_tapped_once();
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
