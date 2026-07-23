// SPDX-License-Identifier: LGPL-3.0-or-later
// Pins the ViGEm adapter submit path against the regression that broke
// PlayStation input: a fire-and-forget submit returned "success" the instant
// the IOCTL queued, so the DS4 EX-to-basic fallback never saw the driver reject
// DS4_SUBMIT_REPORT_EX and every PS frame was silently dropped. Submits must go
// through the SYNCHRONOUS helpers so the rejection stays observable. The fake
// driver layer below stands in for vigem.cpp so this links without real IOCTLs.
#include "vigem_adapter.h"

#include "vigem_submit_policy.h"

#include <iostream>
#include <string>

#include "test_util.h"

// Records what the adapter asks the driver to do and lets each test pin the
// accept/reject verdict. Single-threaded except waitNext*Notification, which the
// adapter's worker thread calls; those touch no shared counters and park on cancel.
namespace fake {
struct State {
    int pluginXboxCalls = 0;
    int pluginDs4Calls = 0;
    int unplugCalls = 0;

    int xusbSyncCalls = 0;
    int ds4ExSyncCalls = 0;
    int ds4BasicSyncCalls = 0;

    // Driver verdicts the test can flip.
    bool ds4ExAccepts = true; // false => simulate a pre-1.17 ViGEmBus
    bool ds4BasicAccepts = true;
    bool xusbAccepts = true;
    bool unplugAccepts = true; // false => driver refused the unplug IOCTL

    // Last reports the adapter handed the driver (for conversion assertions).
    DS4_REPORT_EX lastDs4Ex{};
    DS4_REPORT lastDs4Basic{};

    void reset() { *this = State{}; }

    // Zero just the call counters, preserving the driver verdicts. Lets a test
    // ignore the one-shot EX probe submit a DS4 plug-in now fires and assert
    // purely on subsequent submit behaviour.
    void resetCounts() {
        pluginXboxCalls = pluginDs4Calls = unplugCalls = 0;
        xusbSyncCalls = ds4ExSyncCalls = ds4BasicSyncCalls = 0;
    }
};
static State g;
} // namespace fake

// These signatures must match vigem.h / the adapter's extern decls exactly so
// the linker binds the adapter's calls to these fakes instead of vigem.cpp.
HANDLE openVigemBus() {
    // A real, closable handle so the adapter's CloseHandle(busHandle_) is defined.
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
bool unplugTarget(HANDLE, ULONG) {
    fake::g.unplugCalls++;
    return fake::g.unplugAccepts;
}

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

// Plugging a DS4 slot fires a one-shot EX probe submit so EX capability is known
// before the controller-add ACK is built. On a modern ViGEmBus the probe is
// accepted, EX stays latched on, and motionBackendOk reports the IMU sink up.
static void test_ds4_plugin_probes_ex_and_reports_sink_ok() {
    TEST("DS4 plug-in probes EX (accepted) → motionBackendOk true");
    fake::g.reset(); // ds4ExAccepts defaults true

    ViGEmAdapter a;
    EXPECT(a.ensureBusOpen());
    EXPECT(a.pluginDevice(1, GamepadIdentity::DS4));

    // Exactly one EX submit happened at plug-in, with no basic fallback.
    EXPECT_EQ(fake::g.ds4ExSyncCalls, 1);
    EXPECT_EQ(fake::g.ds4BasicSyncCalls, 0);
    // The honesty fix: the IMU-sink flag reflects the real (accepted) probe.
    EXPECT(a.motionBackendOk(1));
    a.closeBus();
}

// The fix: when the driver rejects the DS4 EX report, the adapter must observe
// that (only possible with a synchronous submit), latch EX off, fall back to
// the basic DS4 report so input still reaches the pad, and report the IMU sink
// as unavailable. Pre-fix this never happened and PlayStation input was dead
// while the sink flag lied "ok".
static void test_ds4_ex_rejected_falls_back_and_reports_no_sink() {
    TEST("DS4 EX rejected → falls back to basic, latches EX off, motionBackendOk false");
    fake::g.reset();
    fake::g.ds4ExAccepts = false; // simulate a pre-1.17 ViGEmBus

    ViGEmAdapter a;
    EXPECT(a.ensureBusOpen());
    EXPECT(a.pluginDevice(1, GamepadIdentity::DS4));

    // The plug-in probe already detected the EX rejection: one EX attempt, one
    // basic fallback, and the IMU-sink flag is honestly false.
    EXPECT_EQ(fake::g.ds4ExSyncCalls, 1);
    EXPECT_EQ(fake::g.ds4BasicSyncCalls, 1);
    EXPECT(!a.motionBackendOk(1));

    // Subsequent gamepad frames go straight to basic (EX latched off) and still
    // apply; input is never silently dropped.
    fake::g.resetCounts();
    GamepadReport rpt{};
    rpt.wButtons = 0x1000; // A
    EXPECT(a.submitReport(1, rpt));
    EXPECT_EQ(fake::g.ds4ExSyncCalls, 0); // not retried
    EXPECT_EQ(fake::g.ds4BasicSyncCalls, 1);
    a.closeBus();
}

// When the driver accepts EX, gamepad frames after the plug-in probe use the EX
// path and never fall back.
static void test_ds4_ex_accepted_uses_ex_path() {
    TEST("DS4 EX accepted → gamepad frames use EX submit, no basic fallback");
    fake::g.reset();

    ViGEmAdapter a;
    EXPECT(a.ensureBusOpen());
    EXPECT(a.pluginDevice(1, GamepadIdentity::DS4));
    fake::g.resetCounts(); // ignore the plug-in probe

    GamepadReport rpt{};
    EXPECT(a.submitReport(1, rpt));
    EXPECT_EQ(fake::g.ds4ExSyncCalls, 1);
    EXPECT_EQ(fake::g.ds4BasicSyncCalls, 0);
    a.closeBus();
}

// motionBackendOk is honest for the non-DS4 and not-plugged cases too: an X360
// target has no IMU surface (false); an unplugged or out-of-range serial has
// nothing to report (true), mirroring the Linux adapter's serial-keyed contract.
static void test_motion_backend_ok_nonds4_and_unplugged() {
    TEST("motionBackendOk: Xbox slot false, unplugged/invalid serial true");
    fake::g.reset();

    ViGEmAdapter a;
    EXPECT(a.ensureBusOpen());
    EXPECT(a.pluginDevice(2, GamepadIdentity::Xbox)); // X360 target, no IMU surface
    EXPECT(!a.motionBackendOk(2));

    EXPECT(a.motionBackendOk(1)); // valid serial, never plugged
    EXPECT(a.motionBackendOk(0)); // out-of-range serial
    EXPECT(a.motionBackendOk(99));
    a.closeBus();
}

// Xbox path goes through the synchronous XUSB helper (never fire-and-forget),
// and never touches the DS4 helpers.
static void test_xbox_uses_synchronous_xusb_submit() {
    TEST("Xbox submit uses submitXusbSync, never FAF or DS4 helpers");
    fake::g.reset();

    ViGEmAdapter a;
    EXPECT(a.ensureBusOpen());
    EXPECT(a.pluginDevice(2, GamepadIdentity::Xbox));

    GamepadReport rpt{};
    EXPECT(a.submitReport(2, rpt));
    EXPECT_EQ(fake::g.xusbSyncCalls, 1);
    EXPECT_EQ(fake::g.ds4ExSyncCalls, 0);
    EXPECT_EQ(fake::g.ds4BasicSyncCalls, 0);
    a.closeBus();
}

// A submit to a serial that was never plugged is rejected, not forwarded.
static void test_submit_to_unplugged_serial_is_rejected() {
    TEST("submit to unplugged serial returns false, no driver call");
    fake::g.reset();

    ViGEmAdapter a;
    EXPECT(a.ensureBusOpen());

    GamepadReport rpt{};
    EXPECT(!a.submitReport(3, rpt)); // unified submit rejects an unplugged serial
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
    EXPECT(a.pluginDevice(1, GamepadIdentity::DS4));

    GamepadReport rpt{};
    rpt.wButtons = 0x1000;   // A -> Cross
    rpt.bRightTrigger = 255; // full right trigger
    rpt.sThumbLX = 32767;    // full right -> 255
    rpt.sThumbLY = 32767;    // full up -> 0 (DS4 Y is inverted)
    EXPECT(a.submitReport(1, rpt));

    const DS4_REPORT_EX& ex = fake::g.lastDs4Ex;
    EXPECT((ex.Report.wButtons & DS4_BUTTON_CROSS) != 0);
    EXPECT_EQ((int)ex.Report.bTriggerR, 255);
    EXPECT_EQ((int)ex.Report.bThumbLX, 255);
    EXPECT_EQ((int)ex.Report.bThumbLY, 0);
    a.closeBus();
}

// DS4 extended-report submit policy (the 50/1784/259 regression).
// ds4ExSubmitLanded decides whether an EX submit reached the device from the
// GetOverlappedResult outcome. The driver routinely completes this IOCTL with a
// benign non-zero status (259 etc.) yet still applies the report, so only
// ACCESS_DENIED and a wrong-buffer-size reject count as a true miss.
static void test_ds4ExSubmitLanded_overlapped_success_always_lands() {
    TEST("ds4ExSubmitLanded: GetOverlappedResult success → landed, regardless of stale error");
    EXPECT(ds4ExSubmitLanded(true, 0));
    EXPECT(ds4ExSubmitLanded(true, ERROR_ACCESS_DENIED)); // ok wins over a stale error
    EXPECT(ds4ExSubmitLanded(true, ERROR_INVALID_PARAMETER));
}

static void test_ds4ExSubmitLanded_benign_failures_still_land() {
    TEST("ds4ExSubmitLanded: benign completion statuses still delivered the report");
    EXPECT(ds4ExSubmitLanded(false, 0));
    EXPECT(ds4ExSubmitLanded(false, ERROR_NO_MORE_ITEMS)); // 259, the one that derailed us
    EXPECT(ds4ExSubmitLanded(false, ERROR_IO_PENDING));    // 997
    EXPECT(ds4ExSubmitLanded(false, ERROR_OPERATION_ABORTED));
}

static void test_ds4ExSubmitLanded_real_failures_do_not_land() {
    TEST("ds4ExSubmitLanded: ACCESS_DENIED and wrong-size rejects are true misses");
    EXPECT(!ds4ExSubmitLanded(false, ERROR_ACCESS_DENIED));       // 5: target gone
    EXPECT(!ds4ExSubmitLanded(false, ERROR_INVALID_PARAMETER));   // 87: pre-1.17 wrong size
    EXPECT(!ds4ExSubmitLanded(false, ERROR_INVALID_USER_BUFFER)); // 1784: 1.21 wrong size
}

// The extended submit struct must be 71 bytes (packed) so the driver routes it
// to the EX path; a different size is rejected (the INVALID_USER_BUFFER bug). The
// EX report itself is the 63-byte DS4 USB input report. EX and basic submits ride
// the same IOCTL, distinguished only by size, so the two must differ.
static void test_ds4_ex_struct_abi() {
    TEST("DS4 EX ABI: EX submit is 71 bytes, EX report is 63, distinct from basic submit");
    EXPECT_EQ(sizeof(DS4_REPORT_EX), (size_t)63);
    EXPECT_EQ(sizeof(DS4_SUBMIT_REPORT_EX), (size_t)71);
    EXPECT(sizeof(DS4_SUBMIT_REPORT_EX) != sizeof(DS4_SUBMIT_REPORT));
    DS4_SUBMIT_REPORT_EX sr{};
    DS4_SUBMIT_REPORT_EX_INIT(&sr, 1);
    EXPECT_EQ((size_t)sr.Size, sizeof(DS4_SUBMIT_REPORT_EX)); // Size field the driver reads
}

// submitMotion forwards gyro/accel onto the EX report and reports the IMU sink
// as live when the driver accepts EX.
static void test_motion_submit_lands_on_ex_when_supported() {
    TEST("submitMotion: gyro/accel reach the EX report and it returns true when EX is accepted");
    fake::g.reset();

    ViGEmAdapter a;
    EXPECT(a.ensureBusOpen());
    EXPECT(a.pluginDevice(1, GamepadIdentity::DS4));
    fake::g.resetCounts(); // ignore the plug-in probe

    MotionReport m{};
    m.gyroX = 1234;
    m.gyroY = -5;
    m.gyroZ = 32000;
    m.accelX = -1;
    m.accelZ = 5678;
    EXPECT(a.submitMotion(1, m));
    EXPECT_EQ(fake::g.ds4ExSyncCalls, 1); // went through the EX path
    EXPECT_EQ(fake::g.ds4BasicSyncCalls, 0);
    const DS4_REPORT_EX& ex = fake::g.lastDs4Ex;
    EXPECT_EQ((int)ex.Report.wGyroX, 1234);
    EXPECT_EQ((int)ex.Report.wGyroY, -5);
    EXPECT_EQ((int)ex.Report.wGyroZ, 32000);
    EXPECT_EQ((int)ex.Report.wAccelX, -1);
    EXPECT_EQ((int)ex.Report.wAccelZ, 5678);
    a.closeBus();
}

// When the driver can't take EX (old ViGEmBus), motion is captured but not
// delivered: submitMotion returns false and never claims success.
static void test_motion_submit_not_delivered_when_ex_unsupported() {
    TEST("submitMotion: returns false when EX is unsupported (no IMU sink)");
    fake::g.reset();
    fake::g.ds4ExAccepts = false;

    ViGEmAdapter a;
    EXPECT(a.ensureBusOpen());
    EXPECT(a.pluginDevice(1, GamepadIdentity::DS4)); // probe latches EX off
    fake::g.resetCounts();

    MotionReport m{};
    m.gyroX = 999;
    EXPECT(!a.submitMotion(1, m));
    a.closeBus();
}

// Motion to a non-DS4 (Xbox) slot or an unplugged serial is never delivered.
static void test_motion_submit_rejected_for_xbox_and_unplugged() {
    TEST("submitMotion: false for an Xbox slot and for an unplugged serial");
    fake::g.reset();

    ViGEmAdapter a;
    EXPECT(a.ensureBusOpen());
    EXPECT(a.pluginDevice(2, GamepadIdentity::Xbox)); // Xbox target, no IMU surface

    MotionReport m{};
    EXPECT(!a.submitMotion(2, m)); // Xbox slot
    EXPECT(!a.submitMotion(7, m)); // never plugged
    a.closeBus();
}

int main() {
    std::cout << "=== test_vigem_adapter ===\n";

    test_ds4_plugin_probes_ex_and_reports_sink_ok();
    test_ds4_ex_rejected_falls_back_and_reports_no_sink();
    test_ds4_ex_accepted_uses_ex_path();
    test_motion_backend_ok_nonds4_and_unplugged();
    test_xbox_uses_synchronous_xusb_submit();
    test_submit_to_unplugged_serial_is_rejected();
    test_xusb_to_ds4_conversion_maps_input();

    test_ds4ExSubmitLanded_overlapped_success_always_lands();
    test_ds4ExSubmitLanded_benign_failures_still_land();
    test_ds4ExSubmitLanded_real_failures_do_not_land();
    test_ds4_ex_struct_abi();
    test_motion_submit_lands_on_ex_when_supported();
    test_motion_submit_not_delivered_when_ex_unsupported();
    test_motion_submit_rejected_for_xbox_and_unplugged();

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
