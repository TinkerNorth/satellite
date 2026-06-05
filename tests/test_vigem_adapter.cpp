// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * tests/test_vigem_adapter.cpp — Unit tests for the Windows ViGEm adapter's
 * submit path, with the ViGEmBus driver layer (src/platform/windows/vigem.cpp)
 * replaced by an in-test fake.
 *
 * Why this exists: the adapter's submit IO is the one place the SessionService
 * tests (which stub the whole IGamepadPort) can't reach. A regression here is
 * what broke PlayStation input — the adapter was wired to a fire-and-forget
 * submit that returned "success" the instant the IOCTL was queued, so the DS4
 * EX→basic fallback never observed the driver rejecting DS4_SUBMIT_REPORT_EX
 * and every PlayStation frame was silently dropped. Xbox has no such fallback,
 * so it survived. These tests pin the fix:
 *
 *   1. Submits go through the SYNCHRONOUS helpers (submitXusbSync /
 *      submitDs4ExSync / submitDs4Sync) and NEVER the fire-and-forget ones —
 *      a re-wire to FAF is exactly the regression that caused the bug.
 *   2. When the driver rejects the DS4 EX report, the adapter latches EX off
 *      and falls back to the basic DS4 report so sticks/buttons still apply.
 *   3. The XUSB→DS4 conversion maps sticks, triggers and face buttons.
 *
 * The fake driver layer below provides every free function the adapter pulls
 * from vigem.cpp/vigem.h, so this target links without the real driver IOCTLs.
 *
 * Self-contained: no external test framework required.
 */
#include "vigem_adapter.h"

#include <iostream>
#include <string>

// ── Tiny assertion harness (mirrors tests/test_session_service.cpp) ──────────
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

#define EXPECT_EQ(a, b)                                                                            \
    do {                                                                                           \
        auto _a = (a);                                                                             \
        auto _b = (b);                                                                             \
        if (_a == _b) {                                                                            \
            g_pass++;                                                                              \
        } else {                                                                                   \
            g_fail++;                                                                              \
            std::cerr << "  FAIL [" << g_currentTest << "] " << __FILE__ << ":" << __LINE__        \
                      << "  " << #a << " == " << #b << "  (got " << _a << " vs " << _b << ")\n";   \
        }                                                                                          \
    } while (0)

// ── Fake ViGEmBus driver layer ──────────────────────────────────────────────
// Records what the adapter asks the driver to do and lets each test pin the
// driver's accept/reject verdict. Single-threaded access from the test thread,
// except waitNext*Notification which the adapter's notification worker calls
// from its own thread; those touch no shared counters and just park on cancel.
namespace fake {
struct State {
    int pluginXboxCalls = 0;
    int pluginDs4Calls = 0;
    int unplugCalls = 0;

    int xusbSyncCalls = 0;
    int ds4ExSyncCalls = 0;
    int ds4BasicSyncCalls = 0;
    // Any fire-and-forget call is a regression: the adapter must use the
    // synchronous helpers so the DS4 EX rejection is observable.
    int fafCalls = 0;

    // Driver verdicts the test can flip.
    bool ds4ExAccepts = true; // false => simulate a pre-1.17 ViGEmBus
    bool ds4BasicAccepts = true;
    bool xusbAccepts = true;

    // Last reports the adapter handed the driver (for conversion assertions).
    DS4_REPORT_EX lastDs4Ex{};
    DS4_REPORT lastDs4Basic{};

    void reset() { *this = State{}; }
};
static State g;
} // namespace fake

// These signatures must match vigem.h / the adapter's extern decls exactly so
// the linker binds the adapter's calls to these fakes instead of vigem.cpp.
HANDLE openVigemBus() {
    // A real, closable handle so the adapter's CloseHandle(busHandle_) in
    // closeBus() is well-defined.
    return CreateEventW(nullptr, FALSE, FALSE, nullptr);
}
bool pluginTarget(HANDLE, ULONG) {
    fake::g.pluginXboxCalls++;
    return true;
}
bool pluginTargetDS4(HANDLE, ULONG) {
    fake::g.pluginDs4Calls++;
    return true;
}
void unplugTarget(HANDLE, ULONG) { fake::g.unplugCalls++; }

bool submitXusbSync(HANDLE, ULONG, XUSB_SUBMIT_REPORT&, HANDLE, const void*) {
    fake::g.xusbSyncCalls++;
    return fake::g.xusbAccepts;
}
bool submitDs4Sync(HANDLE, ULONG, DS4_SUBMIT_REPORT&, HANDLE, const DS4_REPORT& rpt) {
    fake::g.ds4BasicSyncCalls++;
    fake::g.lastDs4Basic = rpt;
    return fake::g.ds4BasicAccepts;
}
bool submitDs4ExSync(HANDLE, ULONG, DS4_SUBMIT_REPORT_EX&, HANDLE, const DS4_REPORT_EX& rpt) {
    fake::g.ds4ExSyncCalls++;
    fake::g.lastDs4Ex = rpt;
    return fake::g.ds4ExAccepts;
}

// Fire-and-forget fakes: present only so a regression that re-wires the adapter
// back to FAF links and trips fafCalls instead of failing as an undefined ref.
bool submitXusbFireAndForget(HANDLE, ULONG, XUSB_SUBMIT_REPORT&, OVERLAPPED&, HANDLE, const void*) {
    fake::g.fafCalls++;
    return true;
}
bool submitDs4FireAndForget(HANDLE, ULONG, DS4_SUBMIT_REPORT&, OVERLAPPED&, HANDLE,
                            const DS4_REPORT&) {
    fake::g.fafCalls++;
    return true;
}
bool submitDs4ExFireAndForget(HANDLE, ULONG, DS4_SUBMIT_REPORT_EX&, OVERLAPPED&, HANDLE,
                              const DS4_REPORT_EX&) {
    fake::g.fafCalls++;
    return true;
}

// The notification worker the adapter spawns at plugin time blocks here; park
// on the cancel event so unplug/closeBus joins cleanly without real IOCTLs.
bool waitNextXusbNotification(HANDLE, ULONG, HANDLE cancel, XUSB_REQUEST_NOTIFICATION&) {
    WaitForSingleObject(cancel, INFINITE);
    return false;
}
bool waitNextDS4Notification(HANDLE, ULONG, HANDLE cancel, DS4_REQUEST_NOTIFICATION&) {
    WaitForSingleObject(cancel, INFINITE);
    return false;
}

// ── Tests ───────────────────────────────────────────────────────────────────

// The fix: when the driver rejects the DS4 EX report, the adapter must observe
// that (only possible with a synchronous submit), latch EX off, and fall back
// to the basic DS4 report so input still reaches the pad. Pre-fix this never
// happened and PlayStation input was dead.
static void test_ds4_ex_rejected_falls_back_to_basic() {
    TEST("DS4 EX rejected → falls back to basic submit and latches EX off");
    fake::g.reset();
    fake::g.ds4ExAccepts = false; // simulate an older ViGEmBus

    ViGEmAdapter a;
    EXPECT(a.ensureBusOpen());
    EXPECT(a.pluginDeviceDS4(1));

    GamepadReport rpt{};
    rpt.wButtons = 0x1000; // A

    // First frame: EX tried, rejected, basic used. Adapter returns the basic
    // submit's result (true) — input applied, not silently dropped.
    EXPECT(a.submitDS4Report(1, rpt));
    EXPECT_EQ(fake::g.ds4ExSyncCalls, 1);
    EXPECT_EQ(fake::g.ds4BasicSyncCalls, 1);

    // Second frame: EX is latched off, so it goes straight to basic.
    EXPECT(a.submitDS4Report(1, rpt));
    EXPECT_EQ(fake::g.ds4ExSyncCalls, 1); // not retried
    EXPECT_EQ(fake::g.ds4BasicSyncCalls, 2);

    EXPECT_EQ(fake::g.fafCalls, 0); // never fire-and-forget
    a.closeBus();
}

// When the driver accepts EX (modern ViGEmBus), the adapter uses the EX path
// and does not fall back.
static void test_ds4_ex_accepted_uses_ex_path() {
    TEST("DS4 EX accepted → uses EX submit, no basic fallback");
    fake::g.reset();

    ViGEmAdapter a;
    EXPECT(a.ensureBusOpen());
    EXPECT(a.pluginDeviceDS4(1));

    GamepadReport rpt{};
    EXPECT(a.submitDS4Report(1, rpt));
    EXPECT_EQ(fake::g.ds4ExSyncCalls, 1);
    EXPECT_EQ(fake::g.ds4BasicSyncCalls, 0);
    EXPECT_EQ(fake::g.fafCalls, 0);
    a.closeBus();
}

// Xbox path goes through the synchronous XUSB helper (never fire-and-forget),
// and never touches the DS4 helpers.
static void test_xbox_uses_synchronous_xusb_submit() {
    TEST("Xbox submit uses submitXusbSync, never FAF or DS4 helpers");
    fake::g.reset();

    ViGEmAdapter a;
    EXPECT(a.ensureBusOpen());
    EXPECT(a.pluginDevice(2));

    GamepadReport rpt{};
    EXPECT(a.submitReport(2, rpt));
    EXPECT_EQ(fake::g.xusbSyncCalls, 1);
    EXPECT_EQ(fake::g.ds4ExSyncCalls, 0);
    EXPECT_EQ(fake::g.ds4BasicSyncCalls, 0);
    EXPECT_EQ(fake::g.fafCalls, 0);
    a.closeBus();
}

// A submit to a serial that was never plugged is rejected, not forwarded.
static void test_submit_to_unplugged_serial_is_rejected() {
    TEST("submit to unplugged serial returns false, no driver call");
    fake::g.reset();

    ViGEmAdapter a;
    EXPECT(a.ensureBusOpen());

    GamepadReport rpt{};
    EXPECT(!a.submitDS4Report(3, rpt));
    EXPECT(!a.submitReport(3, rpt));
    EXPECT_EQ(fake::g.ds4ExSyncCalls, 0);
    EXPECT_EQ(fake::g.ds4BasicSyncCalls, 0);
    EXPECT_EQ(fake::g.xusbSyncCalls, 0);
    a.closeBus();
}

// The XUSB→DS4 conversion maps sticks, triggers and face buttons onto the DS4
// report the driver receives. Verified on the EX report the adapter submits.
static void test_xusb_to_ds4_conversion_maps_input() {
    TEST("XUSB→DS4 conversion maps stick, trigger and button input");
    fake::g.reset();

    ViGEmAdapter a;
    EXPECT(a.ensureBusOpen());
    EXPECT(a.pluginDeviceDS4(1));

    GamepadReport rpt{};
    rpt.wButtons = 0x1000;   // A  → Cross
    rpt.bRightTrigger = 255; // full right trigger
    rpt.sThumbLX = 32767;    // full right  → 255
    rpt.sThumbLY = 32767;    // full up     → 0 (DS4 Y is inverted)
    EXPECT(a.submitDS4Report(1, rpt));

    const DS4_REPORT_EX& ex = fake::g.lastDs4Ex;
    EXPECT((ex.Report.wButtons & DS4_BUTTON_CROSS) != 0);
    EXPECT_EQ((int)ex.Report.bTriggerR, 255);
    EXPECT_EQ((int)ex.Report.bThumbLX, 255);
    EXPECT_EQ((int)ex.Report.bThumbLY, 0);
    a.closeBus();
}

int main() {
    std::cout << "=== test_vigem_adapter ===\n";

    test_ds4_ex_rejected_falls_back_to_basic();
    test_ds4_ex_accepted_uses_ex_path();
    test_xbox_uses_synchronous_xusb_submit();
    test_submit_to_unplugged_serial_is_rejected();
    test_xusb_to_ds4_conversion_maps_input();

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
