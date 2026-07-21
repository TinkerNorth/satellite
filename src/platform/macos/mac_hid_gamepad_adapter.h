// SPDX-License-Identifier: LGPL-3.0-or-later
// Virtual DualShock 4 backend via IOHIDUserDevice (macOS 10.15+). Each plugged
// serial publishes a kernel HID device carrying the DS4 v2 descriptor from
// ds4_report.h; macOS's native DualShock support and GameController.framework
// adopt it like real hardware, so games get sticks/buttons/triggers plus
// rumble, lightbar, touchpad, and motion with no per-game work.
//
// Entitlement: creating the kernel device requires
// com.apple.developer.hid.virtual.device (assumed granted for production
// builds). runtimeAvailable() probes ONCE per process by attempting a minimal
// create; unentitled processes (dev machines, CI) get the historical inert
// stub behavior: ensureBusOpen() false, plugs refused, submits dropped, and
// probeBackend() reporting exactly the pre-backend values (macHidBackendStatus
// below). Builds against SDKs without the IOHIDUserDevice header compile a
// stub with identical behavior.
//
// Slot identity: BOTH pluginDevice (Xbox) and pluginDeviceDS4 publish the same
// DS4 v2 hardware identity. macOS has no in-box driver for an XUSB-shaped HID
// device, so a DS4 is the one identity the whole mac game stack adopts; the
// slot keeps its declared family (isDS4) to gate motion/touchpad/battery
// routing exactly like the ViGEm and uinput adapters, and appliedType
// semantics in SessionService are untouched.
//
// Locking protocol (mirrors vigem_adapter.h / linux gamepad_adapter.h):
//   - mtx_ guards busOpen_, the slot map, and per-slot pack state.
//   - The rumble/lightbar callbacks live in a shared CallbackHub with its own
//     mutex; kernel blocks capture the hub WEAKLY and never `this`. A
//     cancel-timeout quarantine leaks the device (and its blocks) past
//     adapter destruction, so a late set-report must find an expired hub and
//     no-op instead of touching a destroyed adapter.
//   - Submits run entirely under mtx_ (fold state, pack, HandleReport). A
//     slot's IOHIDUserDeviceRef is therefore only ever used while it is still
//     in the map, so teardown can never release a ref out from under an
//     in-flight submit.
//   - The set-report block (rumble/lightbar, on the slot's dispatch queue)
//     takes the hub mutex only to snapshot the callbacks, then invokes them
//     with the lock DROPPED, so a callback can re-enter the adapter without
//     deadlock.
//   - Teardown removes the slot from the map under mtx_, then cancels the
//     device and waits for its cancel handler OUTSIDE mtx_ (same "never join
//     while holding the lock" doctrine as stopReader/stopNotificationWorker):
//     an in-flight block contending for the hub mutex can finish, and after
//     the cancel handler fires no further block runs, making the CFRelease
//     safe. An unconfirmed cancel (timeout) returns false from unplugDevice
//     so SessionService quarantines the serial; the ref is deliberately
//     leaked rather than freed under a possibly-live callback.
#pragma once

#include "core/gamepad_backend.h"
#include "core/ports.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

// Compile-time availability of the IOHIDUserDevice API (public since macOS
// 10.15; the header moved between hid/ and hidsystem/ across SDKs). Without
// it the .cpp builds a stub whose behavior is identical to the historical
// inert macOS backend, so the build never breaks on older SDKs. IOKit itself
// is only included by the .cpp; this header stays IOKit-free.
#if defined(__APPLE__) && (__has_include(<IOKit/hidsystem/IOHIDUserDevice.h>) ||                   \
                           __has_include(<IOKit/hid/IOHIDUserDevice.h>))
#define SATELLITE_HAS_IOHIDUSERDEVICE 1
#else
#define SATELLITE_HAS_IOHIDUSERDEVICE 0
#endif

// Probe outcome for probeBackend(), split out as a pure seam so tests pin BOTH
// branches without an entitled machine. The unavailable branch MUST stay
// byte-identical to the historical macOS stub (web UI hides the backend panel
// on supported == false and clients see backendUnavailable apply results).
inline BackendStatus macHidBackendStatus(bool hidVirtualDeviceAvailable) {
    BackendStatus status;
    if (hidVirtualDeviceAvailable) {
        status.id = BACKEND_ID_MAC_HID;
        status.supported = true;
        status.available = true;
        status.errorCode = nullptr;
    } else {
        status.id = BACKEND_ID_NONE;
        status.supported = false;
        status.available = false;
        status.errorCode = nullptr;
    }
    return status;
}

class MacHidGamepadAdapter : public IGamepadPort {
  public:
    MacHidGamepadAdapter();
    ~MacHidGamepadAdapter() override;

    // One-shot per-process capability probe (cached): attempts a minimal
    // vendor-page IOHIDUserDevice create and releases it immediately. False
    // when the SDK header was absent at build time or the process lacks the
    // HID virtual-device entitlement.
    static bool runtimeAvailable();

    bool ensureBusOpen() override;
    void closeBus() override;
    bool isBusOpen() const override;
    bool pluginDevice(uint32_t serial) override;
    bool pluginDeviceDS4(uint32_t serial) override;
    bool unplugDevice(uint32_t serial) override;
    bool isDevicePlugged(uint32_t serial) const override;
    bool submitReport(uint32_t serial, const GamepadReport& report) override;
    bool submitDS4Report(uint32_t serial, const GamepadReport& report) override;
    void setRumbleCallback(RumbleCallback cb) override;
    void setLightbarCallback(LightbarCallback cb) override;

    // DS4 slots only: IMU rides the same input report (no separate sink to
    // fail), so motionBackendOk is true for plugged DS4 slots and, matching
    // the ViGEm adapter, false for plugged Xbox-typed slots.
    bool submitMotion(uint32_t serial, const MotionReport& report) override;
    bool supportsMotionForType(uint8_t controllerType) const override;
    bool motionBackendOk(uint32_t serial) const override;

    bool submitBattery(uint32_t serial, const BatteryReport& report) override;
    bool submitTouchpad(uint32_t serial, const TouchpadReport& report) override;

#ifdef SATELLITE_BUILD_TESTS
    // Drives the output-report path synthetically (as if a game wrote report
    // 0x05), since firing the real kernel path needs an entitled host plus a
    // running game. Thread-safe; mirrors invokeLightbarForTest on Linux.
    void injectOutputReportForTest(uint32_t serial, const uint8_t* data, size_t len);

    // The liveness token the kernel blocks capture weakly; tests pin that it
    // expires with the adapter (a quarantined device's late block no-ops).
    std::weak_ptr<void> callbackHubForTest() const;
#endif

  private:
    struct Slot;        // IOKit members live in the .cpp; header stays IOKit-free
    struct CallbackHub; // rumble/lightbar sinks + their mutex; blocks hold it weakly

    bool plugCommon(uint32_t serial, bool isDS4);
    bool submitLocked(Slot& slot); // pack + HandleReport; caller holds mtx_
    void handleOutputReport(uint32_t serial, uint32_t reportId, const uint8_t* data, size_t len);

    mutable std::mutex mtx_;
    bool busOpen_ = false;
    std::unordered_map<uint32_t, std::unique_ptr<Slot>> slots_;
    std::shared_ptr<CallbackHub> hub_;
};
