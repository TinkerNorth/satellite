// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * adapters/vigem_adapter.h — IGamepadPort implementation wrapping ViGEm driver IOCTLs.
 *
 * Owns: bus handle, per-serial submitEvent handles.
 */
#pragma once

#include "core/ports.h"

#include <winsock2.h>
#include <windows.h>
#include <setupapi.h>
#include "ViGEm/BusShared.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_map>

class ViGEmAdapter : public IGamepadPort {
  public:
    ViGEmAdapter();
    ~ViGEmAdapter() override;

    bool ensureBusOpen() override;
    void closeBus() override;
    bool isBusOpen() const override;
    bool pluginDevice(uint32_t serial) override;
    bool pluginDeviceDS4(uint32_t serial) override;
    void unplugDevice(uint32_t serial) override;
    bool submitReport(uint32_t serial, const GamepadReport& report) override;
    bool submitDS4Report(uint32_t serial, const GamepadReport& report) override;
    void setRumbleCallback(RumbleCallback cb) override;
    void setLightbarCallback(LightbarCallback cb) override;

    // Forward an IMU sample to the virtual DualShock 4 device's gyro/accel
    // fields (DS4_REPORT_EX). Xbox 360 virtual pads have no IMU surface and
    // return false. Returns true only when the sample reached the device via
    // the extended report path (false on an Xbox pad, an unknown serial, or a
    // ViGEmBus too old for IOCTL_DS4_SUBMIT_REPORT_EX).
    bool submitMotion(uint32_t serial, const MotionReport& report) override;

    // Forward a battery update to the virtual DualShock 4 device's battery
    // byte (DS4_REPORT_EX). Xbox 360 virtual pads have no battery surface and
    // return false. Returns true only when the value reached the device via
    // the extended-report path (false on an Xbox pad, an unknown serial, or a
    // ViGEmBus too old for IOCTL_DS4_SUBMIT_REPORT_EX).
    bool submitBattery(uint32_t serial, const BatteryReport& report) override;

    // Forward a touchpad sample to the virtual DualShock 4 device's touchpad
    // fields (DS4_REPORT_EX sCurrentTouch + the clicky-button bit of bSpecial).
    // Xbox 360 virtual pads have no touchpad surface and return false. Returns
    // true only when the sample reached the device via the extended-report
    // path.
    bool submitTouchpad(uint32_t serial, const TouchpadReport& report) override;

    // Inject a relative mouse movement onto the Windows desktop via SendInput
    // (the TOUCHPAD_MODE_MOUSE routing path). Host-global — independent of the
    // ViGEm bus, so it works with no virtual controllers plugged in. Tracks the
    // mouse-button level so a held click fires press/release only once.
    bool submitRelativeMouse(int dx, int dy, bool leftButton) override;

    // ViGEm exposes an IMU surface only for the DualShock 4 virtual device
    // (DS4_REPORT_EX gyro/accel fields). Xbox 360 emulation has no IMU.
    // The web UI uses this to render an "Xbox virtual pad has no motion
    // sink" warning when the dish advertises motion on an Xbox-typed slot.
    bool supportsMotionForType(uint8_t controllerType) const override;

  private:
    // Per-controller notification worker. Each plugged virtual device gets a
    // thread that loops on `wait*Notification` and forwards each fired event
    // to the registered RumbleCallback. The thread exits when `cancel` is
    // signalled (driven by closeBus or unplugDevice).
    struct NotificationWorker {
        std::thread th;
        HANDLE cancel = nullptr; // manual-reset event signalled to quit
        bool isDS4 = false;
    };

    HANDLE busHandle_ = INVALID_HANDLE_VALUE;
    mutable std::mutex busMtx_;

    // Per-serial pre-allocated overlapped events for fast submission.
    std::unordered_map<uint32_t, HANDLE> submitEvents_;
    // Per-serial notification worker. Owned by the adapter; joined under
    // `busMtx_` (via `stopNotificationWorker`) on unplug / closeBus.
    std::unordered_map<uint32_t, NotificationWorker> notifWorkers_;

    // Per-DS4-serial extended-report state. A DS4 virtual device submits the
    // full DS4_REPORT_EX so gyro/accel ride alongside buttons/sticks; the
    // cached report is the running union of the latest gamepad frame and the
    // latest IMU sample. Only DS4 serials get an entry — its presence is the
    // "is this serial a DualShock 4" test. Guarded by busMtx_.
    struct DS4State {
        DS4_REPORT_EX report{};  // running report; leading fields = buttons/sticks
        bool exSupported = true; // false once IOCTL_DS4_SUBMIT_REPORT_EX is rejected
        std::chrono::steady_clock::time_point lastSubmit{}; // for wTimestamp advance
        // Touchpad passthrough state (Task 1.3). `touchPacket` free-runs as the
        // DS4 touch-frame counter. `trackingId{0,1}` bump on each finger
        // touch-down so a consumer distinguishes a fresh contact from a drag;
        // `fingerDown{0,1}` is the previous active state used to detect that
        // up→down transition. `touchpadButton` is cached separately because the
        // gamepad-report path rewrites bSpecial every frame and would otherwise
        // clobber the clicky-button bit between touch samples.
        uint8_t touchPacket = 0;
        uint8_t trackingId0 = 0;
        uint8_t trackingId1 = 0;
        bool fingerDown0 = false;
        bool fingerDown1 = false;
        bool touchpadButton = false;
    };
    std::unordered_map<uint32_t, DS4State> ds4State_;

    // Installed by the SessionService; called from the notification thread.
    // Stored under busMtx_ but read on every notification firing — kept simple
    // because the lifetime invariant ("adapter outlives callback") makes a
    // copy under lock + invoke unlocked safe.
    RumbleCallback rumbleCb_;
    // Installed alongside rumbleCb_ (Task 1.4). The DS4 notification worker
    // fires it with the lightbar colour on every notification — the same
    // driver event that carries rumble. Identical-colour coalescing lives
    // downstream in SessionService::handleLightbarFromBackend. Same lifetime
    // invariant and locking discipline as rumbleCb_.
    LightbarCallback lightbarCb_;

    // TOUCHPAD_MODE_MOUSE button-level cache for submitRelativeMouse — so a
    // held click doesn't re-emit MOUSEEVENTF_LEFTDOWN every frame. Touched only
    // from the single UDP receiver thread; std::atomic keeps it lock-free.
    std::atomic<bool> relMouseBtnDown_{false};

    void startNotificationWorker(uint32_t serial, bool isDS4); // caller holds busMtx_
    void stopNotificationWorker(uint32_t serial);              // caller holds busMtx_
    void notificationLoop(uint32_t serial, bool isDS4, HANDLE cancel);

    // Submit the cached DS4_REPORT_EX for `serial`. Advances wTimestamp, and on
    // the first IOCTL_DS4_SUBMIT_REPORT_EX rejection latches `exSupported` off
    // and retries via the basic DS4_REPORT path so buttons/sticks still work
    // on a pre-1.17 ViGEmBus. Returns true if the submission succeeded by
    // either path. Caller holds busMtx_; `serial` must exist in ds4State_.
    bool submitDS4Locked(uint32_t serial);
};
