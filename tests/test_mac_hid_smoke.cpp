// SPDX-License-Identifier: LGPL-3.0-or-later
// Real-kernel smoke test for the macOS IOHIDUserDevice backend.
//
// Two layers, split by the com.apple.developer.hid.virtual.device entitlement:
//   1. Runs EVERYWHERE (no entitlement): the probe fallback contract —
//      probeBackend() must be exactly the historical stub when the process is
//      unentitled — and the output-report fan-out (rumble/lightbar callbacks)
//      through the production adapter via the test injection seam.
//   2. Entitled hosts only: creates real kernel HID devices through the
//      production adapter, verifies they appear in (and vanish from) the
//      system HID inventory as a game would see them, and that input-report
//      submission is accepted. Unentitled processes exit 77, wired to
//      SKIP_RETURN_CODE so ctest reports "skipped", never a silent pass
//      (mirrors tests/test_uinput_smoke.cpp).
#include "../src/core/gamepad_backend.h"
#include "../src/platform/macos/ds4_report.h"
#include "../src/platform/macos/mac_hid_gamepad_adapter.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "test_util.h"

#if SATELLITE_HAS_IOHIDUSERDEVICE
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDManager.h>
#endif

namespace {

constexpr int kSkipExitCode = 77;

bool waitFor(const std::atomic<bool>& flag, int deadlineMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(deadlineMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (flag.load(std::memory_order_acquire)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return flag.load(std::memory_order_acquire);
}

#if SATELLITE_HAS_IOHIDUSERDEVICE
// Count HID devices with the DS4 v2 identity and our per-serial USB serial
// string — the consumer-side view a game's input stack enumerates.
int countVirtualPads(uint32_t serial, int deadlineMs, int expected) {
    char serialStr[18];
    ds4SerialString(serial, serialStr);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(deadlineMs);
    int found = -1;
    do {
        found = 0;
        IOHIDManagerRef mgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
        if (mgr == nullptr) return -1;
        IOHIDManagerSetDeviceMatching(mgr, nullptr);
        CFSetRef devs = IOHIDManagerCopyDevices(mgr);
        if (devs != nullptr) {
            const CFIndex n = CFSetGetCount(devs);
            std::vector<const void*> raw(static_cast<size_t>(n));
            CFSetGetValues(devs, raw.data());
            for (CFIndex i = 0; i < n; i++) {
                IOHIDDeviceRef d = (IOHIDDeviceRef)raw[static_cast<size_t>(i)];
                CFTypeRef vid = IOHIDDeviceGetProperty(d, CFSTR(kIOHIDVendorIDKey));
                CFTypeRef pid = IOHIDDeviceGetProperty(d, CFSTR(kIOHIDProductIDKey));
                CFTypeRef ser = IOHIDDeviceGetProperty(d, CFSTR(kIOHIDSerialNumberKey));
                int32_t vidV = 0, pidV = 0;
                if (vid != nullptr && CFGetTypeID(vid) == CFNumberGetTypeID())
                    CFNumberGetValue((CFNumberRef)vid, kCFNumberSInt32Type, &vidV);
                if (pid != nullptr && CFGetTypeID(pid) == CFNumberGetTypeID())
                    CFNumberGetValue((CFNumberRef)pid, kCFNumberSInt32Type, &pidV);
                char serBuf[32] = {};
                if (ser != nullptr && CFGetTypeID(ser) == CFStringGetTypeID())
                    CFStringGetCString((CFStringRef)ser, serBuf, sizeof(serBuf),
                                       kCFStringEncodingUTF8);
                if (vidV == DS4V2_VENDOR_ID && pidV == DS4V2_PRODUCT_ID &&
                    std::strcmp(serBuf, serialStr) == 0)
                    found++;
            }
            CFRelease(devs);
        }
        CFRelease(mgr);
        if (found == expected) return found;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    } while (std::chrono::steady_clock::now() < deadline);
    return found;
}
#endif

} // namespace

int main() {
    std::cout << "Running mac_hid smoke test...\n\n";

    const bool available = MacHidGamepadAdapter::runtimeAvailable();
    const BackendStatus status = probeBackend();

    // Layer 1a: probeBackend() must agree with the seam for the REAL probe
    // outcome of this machine, whatever it is.
    TEST("probeBackend agrees with the macHidBackendStatus seam");
    const BackendStatus expected = macHidBackendStatus(available);
    EXPECT_EQ(std::string(status.id), std::string(expected.id));
    EXPECT_EQ(status.supported, expected.supported);
    EXPECT_EQ(status.available, expected.available);
    EXPECT((status.errorCode == nullptr) == (expected.errorCode == nullptr));

#if SATELLITE_HAS_IOHIDUSERDEVICE
    // Layer 1b: output-report fan-out through the production adapter. The
    // kernel only delivers set-reports on entitled hosts, so the test seam
    // injects the exact bytes a DS4-adopting game would write.
    std::weak_ptr<void> hubToken;
    {
        MacHidGamepadAdapter adapter;
        TEST("callback hub is live while the adapter exists");
        hubToken = adapter.callbackHubForTest();
        EXPECT(!hubToken.expired());
        std::atomic<bool> rumbleSeen{false};
        std::atomic<bool> lightbarSeen{false};
        std::atomic<uint16_t> strong{0};
        std::atomic<uint16_t> weak{0};
        std::atomic<uint32_t> rgb{0};
        adapter.setRumbleCallback([&](uint32_t serial, const RumbleReport& r) {
            if (serial != 7) return;
            strong.store(r.strongMagnitude, std::memory_order_release);
            weak.store(r.weakMagnitude, std::memory_order_release);
            rumbleSeen.store(true, std::memory_order_release);
        });
        adapter.setLightbarCallback([&](uint32_t serial, uint8_t r, uint8_t g, uint8_t b) {
            if (serial != 7) return;
            rgb.store((uint32_t)r << 16 | (uint32_t)g << 8 | b, std::memory_order_release);
            lightbarSeen.store(true, std::memory_order_release);
        });

        TEST("output report 0x05 fans out to rumble + lightbar callbacks");
        uint8_t out[32] = {};
        out[0] = 0x05; // report id
        out[1] = 0x03; // flags: motors + lightbar valid
        out[4] = 0x20; // small/weak
        out[5] = 0x40; // large/strong
        out[6] = 1;    // R
        out[7] = 2;    // G
        out[8] = 3;    // B
        adapter.injectOutputReportForTest(7, out, sizeof(out));
        EXPECT(waitFor(rumbleSeen, 1000));
        EXPECT(waitFor(lightbarSeen, 1000));
        EXPECT_EQ(strong.load(std::memory_order_acquire), (uint16_t)(0x40 * 257));
        EXPECT_EQ(weak.load(std::memory_order_acquire), (uint16_t)(0x20 * 257));
        EXPECT_EQ(rgb.load(std::memory_order_acquire), 0x010203u);

        TEST("valid flags gate the sinks (colour-only write leaves motors alone)");
        rumbleSeen.store(false, std::memory_order_release);
        lightbarSeen.store(false, std::memory_order_release);
        out[1] = 0x02; // lightbar only
        out[6] = 9;
        out[7] = 8;
        out[8] = 7;
        adapter.injectOutputReportForTest(7, out, sizeof(out));
        EXPECT(waitFor(lightbarSeen, 1000));
        EXPECT(!rumbleSeen.load(std::memory_order_acquire));
        EXPECT_EQ(rgb.load(std::memory_order_acquire), 0x090807u);
    }
    // The kernel blocks hold this token weakly (never `this`), so a
    // quarantined device's late set-report locks null and no-ops instead of
    // dereferencing a destroyed adapter.
    TEST("callback hub expires with the adapter (late kernel block no-ops)");
    EXPECT(hubToken.expired());
#endif

    if (!available) {
        // Layer 1c: the unentitled fallback must be EXACTLY the historical
        // stub: probe values byte-identical to the pre-backend macOS build,
        // and every adapter operation inert.
        TEST("unentitled probe reports the exact legacy stub values");
        EXPECT_EQ(std::string(status.id), std::string(BACKEND_ID_NONE));
        EXPECT_EQ(std::string(status.id), std::string("none"));
        EXPECT(!status.supported);
        EXPECT(!status.available);
        EXPECT(status.errorCode == nullptr);

        TEST("unentitled adapter behaves like the legacy stub");
        MacHidGamepadAdapter adapter;
        EXPECT(!adapter.ensureBusOpen());
        EXPECT(!adapter.isBusOpen());
        EXPECT(!adapter.pluginDevice(1));
        EXPECT(!adapter.pluginDeviceDS4(1));
        EXPECT(adapter.unplugDevice(1)); // nothing existed to remove
        EXPECT(!adapter.isDevicePlugged(1));
        GamepadReport report;
        EXPECT(!adapter.submitReport(1, report));
        EXPECT(!adapter.submitDS4Report(1, report));
        MotionReport motion;
        EXPECT(!adapter.submitMotion(1, motion));
        BatteryReport battery;
        EXPECT(!adapter.submitBattery(1, battery));
        TouchpadReport touch;
        EXPECT(!adapter.submitTouchpad(1, touch));
        EXPECT(adapter.motionBackendOk(1)); // unplugged serial reads true
        adapter.closeBus();

        std::cout << "\n=== Test Results (pre-skip layer) ===\n";
        std::cout << "  Passed: " << g_pass << "\n";
        std::cout << "  Failed: " << g_fail << "\n";
        if (g_fail > 0) {
            std::cout << "  STATUS: FAIL\n";
            return 1;
        }
        std::cout << "SKIP: process lacks com.apple.developer.hid.virtual.device — "
                     "real device creation not testable here.\n";
        return kSkipExitCode;
    }

#if SATELLITE_HAS_IOHIDUSERDEVICE
    // Layer 2: entitled host — the real thing.
    MacHidGamepadAdapter adapter;
    constexpr uint32_t kDs4Serial = 11;
    constexpr uint32_t kXboxSerial = 12;

    TEST("bus opens when entitled");
    EXPECT(adapter.ensureBusOpen());
    EXPECT(adapter.isBusOpen());

    TEST("DS4 slot publishes a kernel HID device with the DS4 v2 identity");
    EXPECT(adapter.pluginDeviceDS4(kDs4Serial));
    EXPECT(adapter.isDevicePlugged(kDs4Serial));
    EXPECT_EQ(countVirtualPads(kDs4Serial, 3000, 1), 1);
    EXPECT(adapter.motionBackendOk(kDs4Serial));

    TEST("Xbox-typed slot publishes too (same DS4 identity, different routing)");
    EXPECT(adapter.pluginDevice(kXboxSerial));
    EXPECT_EQ(countVirtualPads(kXboxSerial, 3000, 1), 1);
    EXPECT(!adapter.motionBackendOk(kXboxSerial)); // no IMU routing for Xbox slots

    TEST("submissions are accepted by the kernel");
    GamepadReport report;
    report.wButtons = 0x1000; // A / Cross
    report.bLeftTrigger = 128;
    EXPECT(adapter.submitDS4Report(kDs4Serial, report));
    EXPECT(adapter.submitReport(kXboxSerial, report));
    MotionReport motion;
    motion.gyroX = 100;
    motion.accelZ = 8192;
    EXPECT(adapter.submitMotion(kDs4Serial, motion));
    EXPECT(!adapter.submitMotion(kXboxSerial, motion)); // DS4 slots only
    BatteryReport battery;
    battery.level = 60;
    battery.status = BATTERY_STATUS_DISCHARGING;
    EXPECT(adapter.submitBattery(kDs4Serial, battery));
    TouchpadReport touch;
    touch.finger0.active = true;
    touch.finger0.x = 1024;
    touch.finger0.y = -2048;
    touch.eventTimeMs = 1;
    EXPECT(adapter.submitTouchpad(kDs4Serial, touch));

    TEST("slot-family gates hold (wrong-typed submit refused)");
    EXPECT(!adapter.submitDS4Report(kXboxSerial, report));
    EXPECT(!adapter.submitReport(kDs4Serial, report));

    TEST("unplug removes the kernel devices");
    EXPECT(adapter.unplugDevice(kDs4Serial));
    EXPECT(!adapter.isDevicePlugged(kDs4Serial));
    EXPECT_EQ(countVirtualPads(kDs4Serial, 3000, 0), 0);
    EXPECT(adapter.unplugDevice(kXboxSerial));
    EXPECT_EQ(countVirtualPads(kXboxSerial, 3000, 0), 0);

    adapter.closeBus();
    TEST("bus closes");
    EXPECT(!adapter.isBusOpen());
#endif

    std::cout << "\n=== Test Results ===\n";
    std::cout << "  Passed: " << g_pass << "\n";
    std::cout << "  Failed: " << g_fail << "\n";
    std::cout << "  STATUS: " << (g_fail == 0 ? "ALL PASSED" : "FAILED") << "\n";
    return g_fail == 0 ? 0 : 1;
}
