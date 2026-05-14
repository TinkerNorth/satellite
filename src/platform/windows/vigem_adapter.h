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

    // Installed by the SessionService; called from the notification thread.
    // Stored under busMtx_ but read on every notification firing — kept simple
    // because the lifetime invariant ("adapter outlives callback") makes a
    // copy under lock + invoke unlocked safe.
    RumbleCallback rumbleCb_;

    void startNotificationWorker(uint32_t serial, bool isDS4); // caller holds busMtx_
    void stopNotificationWorker(uint32_t serial);              // caller holds busMtx_
    void notificationLoop(uint32_t serial, bool isDS4, HANDLE cancel);
};
