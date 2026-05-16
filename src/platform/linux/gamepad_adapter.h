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

#include <atomic>
#include <mutex>
#include <thread>
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
    void setRumbleCallback(RumbleCallback cb) override;

    // Forward an IMU sample to the DualShock 4's motion uinput node (the
    // INPUT_PROP_ACCELEROMETER device created alongside the gamepad node).
    // Xbox 360 pads and any device whose motion node failed to open return
    // false; the SessionService still caches the sample for the DSU server.
    bool submitMotion(uint32_t serial, const MotionReport& report) override;

  private:
    // Per-virtual-device record. Owns the uinput fd, the FF effect table the
    // kernel hands us via UI_FF_UPLOAD, and a reader thread that drains the
    // device's input_event stream looking for FF events.
    struct Device {
        int fd = -1;
        // Second uinput node for a DS4's gyro/accel (INPUT_PROP_ACCELEROMETER).
        // -1 when the device is an Xbox pad or the motion node failed to open;
        // motion is best-effort so a failure here is non-fatal.
        int motionFd = -1;
        bool ds4 = false;
        std::thread readerThread;
        std::atomic<bool> readerRunning{false};
        // pipe used to wake the reader's poll() at unplug time. Writes a
        // single byte; the reader treats any traffic on this fd as "exit now".
        int wakePipeRead = -1;
        int wakePipeWrite = -1;

        // Map kernel effect-id → cached strong/weak magnitudes from the most
        // recent UI_FF_UPLOAD for that effect. EV_FF events with `value > 0`
        // turn the corresponding effect on at these magnitudes; `value == 0`
        // turns it off (we then re-emit zeros to the dish).
        struct EffectMags {
            uint16_t strong = 0;
            uint16_t weak = 0;
        };
        std::unordered_map<int, EffectMags> effects;
    };

    // Create a uinput device configured as either an Xbox 360 pad (ds4=false)
    // or a DualShock 4 (ds4=true). Returns the fd, or -1 on failure.
    // FF_RUMBLE is enabled on the device unconditionally so games see the
    // virtual pad as having force-feedback support.
    int openUinputDevice(uint32_t serial, bool ds4);

    // Create the DualShock 4 motion sensor uinput device (a separate evdev
    // node flagged INPUT_PROP_ACCELEROMETER, exposing ABS_X/Y/Z for accel and
    // ABS_RX/RY/RZ for gyro). Returns the fd, or -1 on failure.
    int openMotionUinputDevice(uint32_t serial);

    void startReader(uint32_t serial, Device& dev); // caller holds mtx_
    void stopReader(uint32_t serial);               // caller holds mtx_
    void readerLoop(uint32_t serial, int fd, int wakeFd, bool isDS4);

    mutable std::mutex mtx_;
    bool busOpen_ = false;
    std::unordered_map<uint32_t, Device> devices_;

    // Installed by the SessionService; copied under mtx_ before each call so
    // the reader doesn't hold the lock across the user-supplied callback.
    RumbleCallback rumbleCb_;
};
