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
    // false; the SessionService still caches the sample for the web UI.
    bool submitMotion(uint32_t serial, const MotionReport& report) override;

    // Forward a touchpad sample to the DualShock 4's dedicated multitouch
    // uinput node (created alongside the gamepad node). Emits the MT-B
    // protocol — ABS_MT_SLOT / ABS_MT_TRACKING_ID / ABS_MT_POSITION_X/Y plus
    // BTN_TOUCH / BTN_TOOL_* / BTN_LEFT. Xbox pads (no touchpad node) and any
    // DS4 whose touch node failed to open return false.
    bool submitTouchpad(uint32_t serial, const TouchpadReport& report) override;

    // Inject a relative mouse movement (TOUCHPAD_MODE_MOUSE) via a host-global
    // uinput pointer device, created lazily on first use. EV_REL REL_X/REL_Y
    // plus BTN_LEFT; the button level is tracked so a held click fires once.
    bool submitRelativeMouse(int dx, int dy, bool leftButton) override;

    // Forward a battery sample to a per-controller text file under
    // /tmp/satellite/controller<serial>/battery. uinput has no native battery
    // ioctl (no UI_SET_BATTERY) — the kernel's gamepad battery surface is in
    // /sys/class/power_supply/ and only available to in-tree HID drivers like
    // hid-sony/hid-playstation. The sysfs-proxy file is the simplest reliable
    // hook for userspace consumers (OBS overlays, LED strips, status bars,
    // shell scripts) and is also logged so an operator can see arrivals.
    bool submitBattery(uint32_t serial, const BatteryReport& report) override;

    // Install the SessionService's lightbar sink. uinput has no host-game →
    // adapter readback channel for RGB (EV_LED is single-bit only and the DS4
    // / DualSense lightbar is a hid-playstation LED-class device that uinput
    // doesn't replicate), so the wrapped callback is unlikely to fire from
    // kernel events alone. We wrap the user-supplied sink so that *whenever*
    // the callback does fire — directly via test code, or via a future
    // sysfs-watcher bridge — the RGB triple is also written to
    // /tmp/satellite/controller<serial>/lightbar in "RRGGBB\n" form. The
    // operator's userspace tooling can poll that file to mirror the colour
    // (LED strip, OBS overlay, etc.).
    void setLightbarCallback(LightbarCallback cb) override;

    // Drive the installed lightbar callback synthetically — exposed for test
    // code and for any future userspace bridge that wants to push RGB into
    // the satellite return-path without going through uinput. A no-op when
    // no callback has been installed yet. Safe to call from any thread.
    void invokeLightbarForTest(uint32_t serial, uint8_t r, uint8_t g, uint8_t b);

    // sysfs-proxy base directory (default "/tmp/satellite"). Per-controller
    // files land in <base>/controller<serial>/{battery,lightbar}. Tests
    // override this via the SATELLITE_SYSFS_PROXY_DIR env var so they don't
    // race a real running daemon's /tmp tree. Exposed for direct exercise
    // by unit tests.
    static std::string sysfsProxyDir();
    static bool writeSysfsProxyFile(uint32_t serial, const char* leaf,
                                    const std::string& contents);

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

        // DualShock 4 touchpad multitouch node (Task 1.3) — created next to
        // the gamepad node. -1 for Xbox pads or on open failure (best-effort;
        // a failure here is non-fatal — the sample is still cached). The MT
        // tracking ids are drawn from `touchTrackingId` on each finger
        // touch-down so evdev / libinput consumers see discrete contacts;
        // `touchSlotId` holds the live id per MT slot (-1 = lifted).
        int touchFd = -1;
        int32_t touchTrackingId = 0;
        int32_t touchSlotId[2] = {-1, -1};
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

    // Create the DualShock 4 touchpad multitouch uinput node — a pointer-class
    // clickpad exposing the MT-B protocol axes + BTN_LEFT. Returns fd, or -1.
    int openTouchpadUinputDevice(uint32_t serial);

    // Create (once) the host-global relative-mouse uinput pointer device used
    // by TOUCHPAD_MODE_MOUSE. Returns the fd, or -1 on failure.
    int openRelMouseUinputDevice();

    void startReader(uint32_t serial, Device& dev); // caller holds mtx_
    void stopReader(uint32_t serial);               // caller holds mtx_
    void readerLoop(uint32_t serial, int fd, int wakeFd, bool isDS4);

    mutable std::mutex mtx_;
    bool busOpen_ = false;
    std::unordered_map<uint32_t, Device> devices_;

    // Installed by the SessionService; copied under mtx_ before each call so
    // the reader doesn't hold the lock across the user-supplied callback.
    RumbleCallback rumbleCb_;

    // Lightbar sink — wraps the SessionService-supplied callback so every
    // RGB change also lands in <sysfsProxyDir>/controller<serial>/lightbar.
    // The wrapped cb is rarely exercised on uinput (no kernel readback for
    // RGB) but the wiring matches the Windows ViGEm path and keeps the door
    // open for a future userspace bridge.
    LightbarCallback lightbarCb_;

    // TOUCHPAD_MODE_MOUSE host-global pointer device — lazily created on the
    // first submitRelativeMouse, destroyed in closeBus. `relMouseBtnDown_`
    // caches the button level so a held click emits one press / one release.
    int relMouseFd_ = -1;
    bool relMouseBtnDown_ = false;
};
