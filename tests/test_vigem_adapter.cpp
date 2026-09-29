// SPDX-License-Identifier: LGPL-3.0-or-later
// Pins the ViGEm adapter submit path against the regression that broke
// PlayStation input: a fire-and-forget submit returned "success" the instant
// the IOCTL queued, so the DS4 EX-to-basic fallback never saw the driver reject
// DS4_SUBMIT_REPORT_EX and every PS frame was silently dropped. Submits must go
// through the SYNCHRONOUS helpers so the rejection stays observable. The fake
// driver layer below stands in for vigem.cpp so this links without real IOCTLs.
#include "vigem_adapter.h"

#include "vigem_submit_policy.h"

#include "core/ds4_report.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <future>
#include <iostream>
#include <string>
#include <vector>

#include "test_util.h"

// Records what the adapter asks the driver to do and lets each test pin the
// accept/reject verdict. Single-threaded except waitNext*Notification, which the
// adapter's worker thread calls; those take the one pending notification (set
// before plugin, so the worker's start orders it) and then park on cancel.
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

    // A notification the driver completes once, as the bytes it writes.
    std::vector<uint8_t> xusbNotification;
    std::vector<uint8_t> ds4Notification;

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

// Hands the worker the pending notification, byte for byte, the first time it asks.
static bool completeOnce(std::vector<uint8_t>& pending, void* out, size_t size) {
    if (pending.size() != size) return false;
    std::memcpy(out, pending.data(), size);
    pending.clear();
    return true;
}
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

// The notification worker the adapter spawns at plugin time blocks here; after
// the pending notification, if any, park on the cancel event so unplug/closeBus
// joins cleanly without real IOCTLs.
bool waitNextXusbNotification(HANDLE, ULONG, HANDLE cancel, XUSB_REQUEST_NOTIFICATION& out) {
    if (fake::completeOnce(fake::g.xusbNotification, &out, sizeof(out))) return true;
    WaitForSingleObject(cancel, INFINITE);
    return false;
}
bool waitNextDS4Notification(HANDLE, ULONG, HANDLE cancel, DS4_REQUEST_NOTIFICATION& out) {
    if (fake::completeOnce(fake::g.ds4Notification, &out, sizeof(out))) return true;
    WaitForSingleObject(cancel, INFINITE);
    return false;
}

// ViGEmBus v1.22.0's side of the contract, from the sdk header it builds against
// (ViGEmClient cb8c9f4, whose notification codes and structs are those of 2018's
// e8bbbe6e). Its DMF table matches the whole IOCTL code, access bits included,
// and any other code is completed with STATUS_NOT_SUPPORTED; it fills each
// notification by field name in these layouts.
namespace driver {
struct Ioctl {
    const char* name;
    unsigned long sent;
    unsigned long dispatched;
};
constexpr Ioctl IOCTLS[] = {
    {"IOCTL_VIGEM_PLUGIN_TARGET", IOCTL_VIGEM_PLUGIN_TARGET, 0x2AA004},
    {"IOCTL_VIGEM_UNPLUG_TARGET", IOCTL_VIGEM_UNPLUG_TARGET, 0x2AA008},
    {"IOCTL_VIGEM_CHECK_VERSION", IOCTL_VIGEM_CHECK_VERSION, 0x2AA00C},
    {"IOCTL_VIGEM_WAIT_DEVICE_READY", IOCTL_VIGEM_WAIT_DEVICE_READY, 0x2AA010},
    {"IOCTL_XUSB_REQUEST_NOTIFICATION", IOCTL_XUSB_REQUEST_NOTIFICATION, 0x2AE804},
    {"IOCTL_XUSB_SUBMIT_REPORT", IOCTL_XUSB_SUBMIT_REPORT, 0x2AA808},
    {"IOCTL_DS4_SUBMIT_REPORT", IOCTL_DS4_SUBMIT_REPORT, 0x2AA80C},
    {"IOCTL_DS4_REQUEST_NOTIFICATION", IOCTL_DS4_REQUEST_NOTIFICATION, 0x2AA810},
};

// XUSB_REQUEST_NOTIFICATION: {Size, SerialNo, LargeMotor, SmallMotor, LedNumber}.
constexpr size_t XUSB_NOTIFICATION_SIZE = 12;
constexpr size_t XUSB_LARGE_MOTOR_AT = 8;
constexpr size_t XUSB_SMALL_MOTOR_AT = 9;
constexpr size_t XUSB_LED_NUMBER_AT = 10;

// DS4_REQUEST_NOTIFICATION: {Size, SerialNo, {SmallMotor, LargeMotor, Red, Green, Blue}}.
constexpr size_t DS4_NOTIFICATION_SIZE = 16;
constexpr size_t DS4_SMALL_MOTOR_AT = 8;
constexpr size_t DS4_LARGE_MOTOR_AT = 9;
constexpr size_t DS4_RED_AT = 10;
constexpr size_t DS4_GREEN_AT = 11;
constexpr size_t DS4_BLUE_AT = 12;

static std::vector<uint8_t> xusbNotification(uint8_t largeMotor, uint8_t smallMotor,
                                             uint8_t ledNumber) {
    std::vector<uint8_t> bytes(XUSB_NOTIFICATION_SIZE, 0);
    bytes[XUSB_LARGE_MOTOR_AT] = largeMotor;
    bytes[XUSB_SMALL_MOTOR_AT] = smallMotor;
    bytes[XUSB_LED_NUMBER_AT] = ledNumber;
    return bytes;
}

static std::vector<uint8_t> ds4Notification(uint8_t smallMotor, uint8_t largeMotor, uint8_t red,
                                            uint8_t green, uint8_t blue) {
    std::vector<uint8_t> bytes(DS4_NOTIFICATION_SIZE, 0);
    bytes[DS4_SMALL_MOTOR_AT] = smallMotor;
    bytes[DS4_LARGE_MOTOR_AT] = largeMotor;
    bytes[DS4_RED_AT] = red;
    bytes[DS4_GREEN_AT] = green;
    bytes[DS4_BLUE_AT] = blue;
    return bytes;
}
} // namespace driver

// What the adapter's notification worker forwards, delivered to the test thread.
struct ForwardedFeedback {
    std::promise<RumbleReport> rumble;
    std::promise<std::array<uint8_t, 3>> lightbar;
};

constexpr auto FORWARD_TIMEOUT = std::chrono::seconds(5);

template <typename T> static bool arrivesInTime(const std::future<T>& forwarded) {
    return forwarded.wait_for(FORWARD_TIMEOUT) == std::future_status::ready;
}

// A motor byte as the adapter scales it onto the u16 rumble range.
static uint16_t rumbleMagnitude(uint8_t motor) { return static_cast<uint16_t>(motor * 257); }

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

// The DS4 report the driver receives is the one Sony layout the core packs for
// every DS4 shell: same button word, same hat, same stick bytes, same PS bit.
static void test_ds4_report_is_the_shared_sony_layout() {
    fake::g.reset();
    ViGEmAdapter a;
    EXPECT(a.ensureBusOpen());
    EXPECT(a.pluginDevice(1, GamepadIdentity::DS4));

    TEST("DS4 report: every XUSB button lands as the shared Sony button word");
    for (int bit = 0; bit < 16; bit++) {
        GamepadReport rpt{};
        rpt.wButtons = static_cast<uint16_t>(1u << bit);
        EXPECT(a.submitReport(1, rpt));
        EXPECT_EQ((int)fake::g.lastDs4Ex.Report.wButtons,
                  (int)sonyButtonsFromXusb(rpt.wButtons, 0, 0));
    }

    TEST("DS4 report: contradictory d-pad combinations resolve as the shared hat does");
    for (const int dpad : {0x0003, 0x000C, 0x000F, 0x0009, 0x0006}) {
        GamepadReport rpt{};
        rpt.wButtons = static_cast<uint16_t>(dpad);
        EXPECT(a.submitReport(1, rpt));
        EXPECT_EQ((int)(fake::g.lastDs4Ex.Report.wButtons & 0x000F),
                  (int)ds4HatFromButtons(rpt.wButtons));
    }

    TEST("DS4 report: the digital L2/R2 bits are left to the bus, whatever the triggers");
    {
        GamepadReport rpt{};
        rpt.bLeftTrigger = 255;
        rpt.bRightTrigger = 255;
        EXPECT(a.submitReport(1, rpt));
        const DS4_REPORT_EX& ex = fake::g.lastDs4Ex;
        EXPECT_EQ((int)(ex.Report.wButtons & (DS4_BUTTON_TRIGGER_LEFT | DS4_BUTTON_TRIGGER_RIGHT)),
                  0);
        EXPECT_EQ((int)ex.Report.bTriggerL, 255);
        EXPECT_EQ((int)ex.Report.bTriggerR, 255);
    }

    TEST("DS4 report: sticks are the shared byte conversion, Y inverted");
    {
        GamepadReport rpt{};
        rpt.sThumbLX = -32768;
        rpt.sThumbLY = -32768;
        rpt.sThumbRX = 12345;
        rpt.sThumbRY = -12345;
        EXPECT(a.submitReport(1, rpt));
        const DS4_REPORT_EX& ex = fake::g.lastDs4Ex;
        EXPECT_EQ((int)ex.Report.bThumbLX, (int)ds4StickByte(-32768));
        EXPECT_EQ((int)ex.Report.bThumbLY, (int)ds4StickByteInverted(-32768));
        EXPECT_EQ((int)ex.Report.bThumbRX, (int)ds4StickByte(12345));
        EXPECT_EQ((int)ex.Report.bThumbRY, (int)ds4StickByteInverted(-12345));
    }

    TEST("DS4 report: Guide is the PS bit, and the touchpad click bit is not disturbed");
    {
        GamepadReport rpt{};
        rpt.wButtons = 0x0400;
        EXPECT(a.submitReport(1, rpt));
        EXPECT_EQ((int)fake::g.lastDs4Ex.Report.bSpecial, 0x01);
        TouchpadReport tp{};
        tp.buttonPressed = true;
        EXPECT(a.submitTouchpad(1, tp));
        EXPECT(a.submitReport(1, rpt));
        EXPECT_EQ((int)fake::g.lastDs4Ex.Report.bSpecial, 0x03);
        rpt.wButtons = 0;
        EXPECT(a.submitReport(1, rpt));
        EXPECT_EQ((int)fake::g.lastDs4Ex.Report.bSpecial, 0x02);
    }
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
    static_assert(sizeof(DS4_SUBMIT_REPORT_EX) != sizeof(DS4_SUBMIT_REPORT),
                  "the EX submit must not be mistakable for the basic one by size");
    DS4_SUBMIT_REPORT_EX sr{};
    DS4_SUBMIT_REPORT_EX_INIT(&sr, 1);
    EXPECT_EQ((size_t)sr.Size, sizeof(DS4_SUBMIT_REPORT_EX)); // Size field the driver reads
}

static void test_every_ioctl_is_a_code_the_driver_dispatches() {
    for (const driver::Ioctl& ioctl : driver::IOCTLS) {
        TEST(std::string("ViGEm bus ABI: ") + ioctl.name + " is the code ViGEmBus dispatches");
        EXPECT_EQ(ioctl.sent, ioctl.dispatched);
    }
    TEST("ViGEm bus ABI: both notification buffers are the size the driver fills");
    EXPECT_EQ(sizeof(XUSB_REQUEST_NOTIFICATION), driver::XUSB_NOTIFICATION_SIZE);
    EXPECT_EQ(sizeof(DS4_REQUEST_NOTIFICATION), driver::DS4_NOTIFICATION_SIZE);
}

// The game's large (low-frequency) motor is the strong one on every Dish; the
// player-LED slot the driver reports beside the motors is not a motor.
static void test_xusb_rumble_forwards_the_large_motor_as_strong() {
    TEST("XUSB rumble: the driver's large motor arrives as strong, its small motor as weak");
    constexpr uint8_t largeMotor = 200;
    constexpr uint8_t smallMotor = 50;
    constexpr uint8_t ledNumber = 3;
    fake::g.reset();
    fake::g.xusbNotification = driver::xusbNotification(largeMotor, smallMotor, ledNumber);
    ForwardedFeedback forwarded;
    std::future<RumbleReport> rumble = forwarded.rumble.get_future();
    ViGEmAdapter a;
    a.setRumbleCallback(
        [&forwarded](uint32_t, const RumbleReport& r) { forwarded.rumble.set_value(r); });
    EXPECT(a.ensureBusOpen());
    EXPECT(a.pluginDevice(1, GamepadIdentity::Xbox));

    const bool arrived = arrivesInTime(rumble);
    EXPECT(arrived);
    const RumbleReport report = arrived ? rumble.get() : RumbleReport{};
    EXPECT_EQ(report.strongMagnitude, rumbleMagnitude(largeMotor));
    EXPECT_EQ(report.weakMagnitude, rumbleMagnitude(smallMotor));
    a.closeBus();
}

// A DS4 output report names the small (right) motor first; the strong one is
// still the large motor, and the lightbar colour rides the same notification.
static void test_ds4_feedback_forwards_the_large_motor_as_strong_and_the_colour() {
    TEST("DS4 feedback: the driver's large motor arrives as strong, its small motor as weak");
    constexpr uint8_t smallMotor = 50;
    constexpr uint8_t largeMotor = 200;
    constexpr std::array<uint8_t, 3> colour = {10, 20, 30};
    fake::g.reset();
    fake::g.ds4Notification =
        driver::ds4Notification(smallMotor, largeMotor, colour[0], colour[1], colour[2]);
    ForwardedFeedback forwarded;
    std::future<RumbleReport> rumble = forwarded.rumble.get_future();
    std::future<std::array<uint8_t, 3>> lightbar = forwarded.lightbar.get_future();
    ViGEmAdapter a;
    a.setRumbleCallback(
        [&forwarded](uint32_t, const RumbleReport& r) { forwarded.rumble.set_value(r); });
    a.setLightbarCallback([&forwarded](uint32_t, uint8_t r, uint8_t g, uint8_t b) {
        forwarded.lightbar.set_value({r, g, b});
    });
    EXPECT(a.ensureBusOpen());
    EXPECT(a.pluginDevice(1, GamepadIdentity::DS4));

    const bool rumbleArrived = arrivesInTime(rumble);
    EXPECT(rumbleArrived);
    const RumbleReport report = rumbleArrived ? rumble.get() : RumbleReport{};
    EXPECT_EQ(report.strongMagnitude, rumbleMagnitude(largeMotor));
    EXPECT_EQ(report.weakMagnitude, rumbleMagnitude(smallMotor));

    TEST("DS4 feedback: the driver's lightbar colour arrives with the rumble");
    const bool lightbarArrived = arrivesInTime(lightbar);
    EXPECT(lightbarArrived);
    const std::array<uint8_t, 3> forwardedColour =
        lightbarArrived ? lightbar.get() : std::array<uint8_t, 3>{};
    EXPECT(forwardedColour == colour);
    a.closeBus();
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
    test_ds4_report_is_the_shared_sony_layout();

    test_ds4ExSubmitLanded_overlapped_success_always_lands();
    test_ds4ExSubmitLanded_benign_failures_still_land();
    test_ds4ExSubmitLanded_real_failures_do_not_land();
    test_ds4_ex_struct_abi();
    test_every_ioctl_is_a_code_the_driver_dispatches();
    test_xusb_rumble_forwards_the_large_motor_as_strong();
    test_ds4_feedback_forwards_the_large_motor_as_strong_and_the_colour();
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
