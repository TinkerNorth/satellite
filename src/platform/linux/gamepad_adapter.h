// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * gamepad_adapter.h — IGamepadPort implementation backed by /dev/uinput (Linux).
 *
 * Each plugged-in controller owns its own uinput file descriptor. The adapter
 * exposes two device profiles:
 *   - Xbox 360 Wired  (VID 0x045e / PID 0x028e)
 *   - DualShock 4 v1  (VID 0x054c / PID 0x05c4)
 * Button codes and axis ranges follow the evdev conventions used by the
 * in-tree xpad and hid-sony drivers, so SDL2 and the Steam Input stack
 * recognize them as first-class game controllers.
 */
#pragma once

#include "core/ports.h"

#include <mutex>
#include <unordered_map>

class GamepadAdapter : public IGamepadPort {
  public:
    GamepadAdapter() = default;
    ~GamepadAdapter() override;

    bool ensureBusOpen() override;
    void closeBus() override;
    bool isBusOpen() const override;
    bool pluginDevice(uint32_t serial) override;
    bool pluginDeviceDS4(uint32_t serial) override;
    void unplugDevice(uint32_t serial) override;
    bool submitReport(uint32_t serial, const GamepadReport& report) override;
    bool submitDS4Report(uint32_t serial, const GamepadReport& report) override;

  private:
    // Create a uinput device configured as either an Xbox 360 pad (ds4=false)
    // or a DualShock 4 (ds4=true). Returns the fd, or -1 on failure.
    int openUinputDevice(uint32_t serial, bool ds4);

    mutable std::mutex mtx_;
    bool busOpen_ = false;
    std::unordered_map<uint32_t, int> fds_; // serial → uinput fd
};
