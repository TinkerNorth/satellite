// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * adapters/vigem_adapter.h -- IGamepadPort implementation wrapping the
 * ViGEmBus driver IOCTLs.
 *
 * Hot-path data layout:
 *   * Per-serial state lives in a flat std::array indexed 1..16. The
 *     previous std::unordered_map<uint32_t, HANDLE> + std::unordered_map<
 *     uint32_t, DS4State> meant two hash lookups per submitted gamepad
 *     frame; the flat array is one indexed load with no hashing.
 *   * Each slot owns one persistent OVERLAPPED + auto-reset event +
 *     submit buffer for Xbox / DS4 / DS4_EX paths. The buffers live as
 *     long as the slot does -- the kernel reads from them asynchronously
 *     during fire-and-forget IOCTLs, so they must outlive the IOCTL.
 *   * `submitReport` does not wait for kernel completion any more.
 *     `WaitForSingleObject(slot.event, INFINITE)` runs at the *next*
 *     submission and throttles us to the kernel's pace -- in steady
 *     state the event is already signalled, so the wait is a single
 *     test-and-clear with no syscall blocking.
 *
 * Locking:
 *   * `busMtx_` still guards `busHandle_`, the notification-worker map,
 *     and slot bookkeeping (plugin/unplug). The hot submit path acquires
 *     the lock for a brief validate-and-copy-handle, then drops it for
 *     the DeviceIoControl + Wait. Slot teardown happens only at
 *     `closeBus` and waits on `slot.event` before closing it, so an
 *     in-flight IOCTL cannot use-after-free the event handle.
 */
#pragma once

#include "core/ports.h"

#include <winsock2.h>
#include <windows.h>
#include <setupapi.h>
#include "ViGEm/BusShared.h"

#include <array>
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

    bool submitMotion(uint32_t serial, const MotionReport& report) override;
    bool submitBattery(uint32_t serial, const BatteryReport& report) override;
    bool submitTouchpad(uint32_t serial, const TouchpadReport& report) override;
    bool submitRelativeMouse(int dx, int dy, bool leftButton) override;
    bool supportsMotionForType(uint8_t controllerType) const override;

  private:
    struct NotificationWorker {
        std::thread th;
        HANDLE cancel = nullptr;
        bool isDS4 = false;
    };

    // Per-DS4-serial running-report state. Merges the latest gamepad
    // frame, IMU sample, and touchpad sample into one DS4_REPORT_EX so
    // the host sees a coherent view. Only meaningful when slot.isDS4.
    struct DS4State {
        DS4_REPORT_EX report{};
        bool exSupported = true;
        std::chrono::steady_clock::time_point lastSubmit{};
        uint8_t touchPacket = 0;
        uint8_t trackingId0 = 0;
        uint8_t trackingId1 = 0;
        bool fingerDown0 = false;
        bool fingerDown1 = false;
        bool touchpadButton = false;
    };

    // One slot per backend serial (1..16). Indexed by serial; slot 0 is
    // unused (serial 0 means "not plugged"). All non-trivial fields are
    // persistent across IOCTL calls because the kernel reads them
    // asynchronously after we return from DeviceIoControl.
    struct IoSlot {
        // Auto-reset event, signalled by the kernel on IOCTL completion.
        // Created in the signalled state at plugin time so the very first
        // submit's WaitForSingleObject passes through immediately.
        HANDLE event = nullptr;
        OVERLAPPED ov{};

        // Submit buffers for each report kind. We keep all three because
        // a DS4 slot may need to fall back from EX to basic mid-session
        // when an older ViGEmBus driver rejects the EX IOCTL.
        XUSB_SUBMIT_REPORT xsr{};
        DS4_SUBMIT_REPORT ds4Basic{};
        DS4_SUBMIT_REPORT_EX ds4Ex{};

        // Running DS4 merge state. Only valid when isDS4 && plugged.
        DS4State ds4{};

        // Hot-path gates. Atomic so submitReport can read them after
        // dropping busMtx_ without taking another lock; writes happen
        // under busMtx_ (plugin / unplug).
        std::atomic<bool> plugged{false};
        bool isDS4 = false;
    };

    HANDLE busHandle_ = INVALID_HANDLE_VALUE;
    mutable std::mutex busMtx_;

    // Slots 1..MAX_BACKEND_CONTROLLERS. Slot 0 is unused so direct
    // indexing matches the wire-level serial numbers.
    std::array<IoSlot, MAX_BACKEND_CONTROLLERS + 1> io_;

    // Per-serial notification worker. Plug-/unplug-time only; not on
    // the hot path. Still a map (cheap allocation only at plug time).
    std::unordered_map<uint32_t, NotificationWorker> notifWorkers_;

    RumbleCallback rumbleCb_;
    LightbarCallback lightbarCb_;

    std::atomic<bool> relMouseBtnDown_{false};

    void startNotificationWorker(uint32_t serial, bool isDS4); // caller holds busMtx_
    void stopNotificationWorker(uint32_t serial);              // caller holds busMtx_
    void notificationLoop(uint32_t serial, bool isDS4, HANDLE cancel);

    // Submit the cached DS4_REPORT_EX for `serial`. Advances wTimestamp,
    // latches `exSupported` off on the first EX rejection and retries
    // via the basic DS4_REPORT path so buttons/sticks still work on a
    // pre-1.17 ViGEmBus. Returns true if either path queued the IOCTL.
    // Caller holds busMtx_; the slot must be plugged && isDS4.
    bool submitDS4Locked(uint32_t serial);
};
