// SPDX-License-Identifier: LGPL-3.0-or-later
// Hot path: per-serial state is a flat array (no hash lookups); each slot owns
// the duplicated section view + event handles and a persistent payload buffer,
// so a submit is pack + one seqlock write + SetEvent — no allocations, no
// syscall beyond the doorbell, no managed code. The elevated device lifecycle
// (driver install, SwDevice creation, section creation, handle duplication)
// lives behind IHidMaestroProvisioner and only runs at plug/unplug time.
//
// Locking: busMtx_ guards slot plugin/unplug, the worker map, and the
// callbacks. Submits run entirely under it — the guarded section is a few
// hundred nanoseconds of memcpy into the mapped view (unlike ViGEm's blocking
// IOCTL there is nothing worth dropping the lock for), and holding it means a
// concurrent unplug can never unmap a view mid-write.
#pragma once

#include "core/gamepad_backend.h"
#include "core/ports.h"
#include "hidmaestro_provisioner.h"
#include "hidmaestro_report.h"
#include "hidmaestro_wire.h"

#include <winsock2.h>
#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_map>

class HidMaestroAdapter : public IGamepadPort {
  public:
    explicit HidMaestroAdapter(satellite::hidmaestro::IHidMaestroProvisioner& provisioner);
    ~HidMaestroAdapter() override;

    bool ensureBusOpen() override;
    void closeBus() override;
    bool isBusOpen() const override;
    bool pluginDevice(uint32_t serial, GamepadIdentity identity) override;
    bool supportsIdentity(GamepadIdentity identity) const override;
    const char* backendId() const override { return BACKEND_ID_HIDMAESTRO; }
    bool unplugDevice(uint32_t serial) override;
    bool isDevicePlugged(uint32_t serial) const override;
    bool submitReport(uint32_t serial, const GamepadReport& report) override;
    void setRumbleCallback(RumbleCallback cb) override;
    void setLightbarCallback(LightbarCallback cb) override;

    bool submitMotion(uint32_t serial, const MotionReport& report) override;
    bool submitBattery(uint32_t serial, const BatteryReport& report) override;
    bool submitTouchpad(uint32_t serial, const TouchpadReport& report) override;
    bool submitRelativeMouse(int dx, int dy, bool leftButton) override;
    bool supportsRelativeMouse() const override { return true; }
    bool supportsMotionForType(uint8_t controllerType) const override;

  private:
    struct OutputWorker {
        std::thread th;
        HANDLE cancel = nullptr;
    };

    // Slot 0 unused so indexing matches wire-level serial numbers. Views and
    // handles are the duplicated objects the provisioner handed over; payload
    // persists across frames so a submit never allocates.
    struct IoSlot {
        GamepadIdentity identity = GamepadIdentity::Xbox;
        uint8_t* inputView = nullptr;
        const uint8_t* outputView = nullptr;
        HANDLE inputSection = nullptr;
        HANDLE inputEvent = nullptr;
        HANDLE companionEvent = nullptr;
        HANDLE outputSection = nullptr;
        HANDLE outputEvent = nullptr;

        // Merged latest-sample state per identity family, so motion/touchpad/
        // battery samples ride every subsequent frame (same model as the
        // ViGEm DS4 EX merge).
        Ds4InputState ds4{};
        satellite::hidmaestro::Ds5InputState ds5{};
        GamepadReport switchPad{};
        MotionReport switchMotion{};
        bool fingerDown0 = false;
        bool fingerDown1 = false;
        std::chrono::steady_clock::time_point lastSonySubmit{};

        uint8_t payload[satellite::hidmaestro::INPUT_DATA_CAPACITY]{};
        uint8_t gip[satellite::hidmaestro::INPUT_GIP_LENGTH]{};

        // Atomic so isDevicePlugged can read without data races; written under
        // busMtx_ (plugin/unplug).
        std::atomic<bool> plugged{false};
    };

    satellite::hidmaestro::IHidMaestroProvisioner& provisioner_;
    mutable std::mutex busMtx_;
    bool busOpen_ = false;

    std::array<IoSlot, MAX_BACKEND_CONTROLLERS + 1> io_;

    // Plug/unplug-time only (not hot path), so a map is fine.
    std::unordered_map<uint32_t, OutputWorker> outputWorkers_;

    RumbleCallback rumbleCb_;
    LightbarCallback lightbarCb_;

    std::atomic<bool> relMouseBtnDown_{false};

    void startOutputWorker(uint32_t serial); // caller holds busMtx_
    void stopOutputWorker(uint32_t serial);  // caller holds busMtx_
    void outputLoop(uint32_t serial, HANDLE cancel, uint32_t lastSeq);
    void releaseSlotLocked(IoSlot& slot);

    // Advance the Sony free-running clocks from wall time, pack the slot's
    // merged state for its identity, and publish the frame. Caller holds
    // busMtx_; the slot must be plugged.
    bool packAndWriteLocked(IoSlot& slot);
};
