// SPDX-License-Identifier: LGPL-3.0-or-later
// Hot path: per-serial state is a flat array (no hash lookups); each slot owns a
// persistent event + submit buffer so a submit is one memcpy. IO is synchronous
// (GetOverlappedResult TRUE): fire-and-forget was tried and the dish saw "no
// input reaching the game".
//
// Locking: busMtx_ guards busHandle_, the notification-worker map, and slot
// plugin/unplug. The submit path holds it only to validate + copy the handle,
// then drops it for the IOCTL. Slot events are freed only in closeBus, after all
// sync submits complete, so no in-flight IOCTL can use-after-free one.
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
    bool unplugDevice(uint32_t serial) override;
    bool isDevicePlugged(uint32_t serial) const override;
    bool submitReport(uint32_t serial, const GamepadReport& report) override;
    bool submitDS4Report(uint32_t serial, const GamepadReport& report) override;
    void setRumbleCallback(RumbleCallback cb) override;
    void setLightbarCallback(LightbarCallback cb) override;

    bool submitMotion(uint32_t serial, const MotionReport& report) override;
    bool submitBattery(uint32_t serial, const BatteryReport& report) override;
    bool submitTouchpad(uint32_t serial, const TouchpadReport& report) override;
    bool submitRelativeMouse(int dx, int dy, bool leftButton) override;
    bool supportsRelativeMouse() const override { return true; }
    bool supportsMotionForType(uint8_t controllerType) const override;
    bool motionBackendOk(uint32_t serial) const override;

  private:
    struct NotificationWorker {
        std::thread th;
        HANDLE cancel = nullptr;
        bool isDS4 = false;
    };

    // Merges the latest gamepad/IMU/touchpad samples into one DS4_REPORT_EX.
    // Only meaningful when slot.isDS4.
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

    // Slot 0 unused (serial 0 = not plugged). Buffers persist across IOCTLs so a
    // submit is one memcpy.
    struct IoSlot {
        // Persistent to avoid a CreateEvent/CloseHandle pair on the 250 Hz hot path.
        HANDLE event = nullptr;

        // All three kept: a DS4 slot may fall back EX to basic mid-session when
        // an older driver rejects the EX IOCTL.
        XUSB_SUBMIT_REPORT xsr{};
        DS4_SUBMIT_REPORT ds4Basic{};
        DS4_SUBMIT_REPORT_EX ds4Ex{};

        DS4State ds4{}; // valid only when isDS4 && plugged

        // Atomic so submitReport can read after dropping busMtx_; written under
        // busMtx_ (plugin/unplug).
        std::atomic<bool> plugged{false};
        bool isDS4 = false;
    };

    HANDLE busHandle_ = INVALID_HANDLE_VALUE;
    mutable std::mutex busMtx_;

    // Slot 0 unused so indexing matches wire-level serial numbers.
    std::array<IoSlot, MAX_BACKEND_CONTROLLERS + 1> io_;

    // Plug/unplug-time only (not hot path), so a map is fine.
    std::unordered_map<uint32_t, NotificationWorker> notifWorkers_;

    RumbleCallback rumbleCb_;
    LightbarCallback lightbarCb_;

    std::atomic<bool> relMouseBtnDown_{false};

    void startNotificationWorker(uint32_t serial, bool isDS4); // caller holds busMtx_
    void stopNotificationWorker(uint32_t serial);              // caller holds busMtx_
    void notificationLoop(uint32_t serial, bool isDS4, HANDLE cancel);

    // On the first EX rejection (pre-1.17 ViGEmBus), latch exSupported off and
    // retry via basic DS4_REPORT so buttons/sticks still work. Caller holds busMtx_.
    bool submitDS4Locked(uint32_t serial);
};
