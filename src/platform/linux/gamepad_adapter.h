// SPDX-License-Identifier: LGPL-3.0-or-later
// IGamepadPort over /dev/uinput. Each controller owns its own uinput fd,
// exposed as one of two profiles (Xbox 360 wired or DualShock 4 v1). Button
// codes / axis ranges follow the in-tree xpad and hid-sony evdev conventions
// so SDL2 and Steam Input recognize them as first-class controllers.
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
    bool unplugDevice(uint32_t serial) override;
    bool isDevicePlugged(uint32_t serial) const override;
    bool submitReport(uint32_t serial, const GamepadReport& report) override;
    bool submitDS4Report(uint32_t serial, const GamepadReport& report) override;
    void setRumbleCallback(RumbleCallback cb) override;

    // DS4 only (the INPUT_PROP_ACCELEROMETER node); Xbox pads and any device
    // whose motion node failed to open return false. SessionService still
    // caches the sample for the web UI.
    bool submitMotion(uint32_t serial, const MotionReport& report) override;

    // Backend-shape query (not per-serial): only the DS4 profile has a motion
    // node. Surfaced so the web UI explains why an Xbox slot won't deliver gyro.
    bool supportsMotionForType(uint8_t controllerType) const override;

    // True iff this serial's motion node was created at plug-in time. Lets the
    // web UI distinguish a kernel-rejected backend (rare) from "no game subscribed".
    bool motionBackendOk(uint32_t serial) const override;

    // DS4 only: emits MT-B (ABS_MT_SLOT/TRACKING_ID/POSITION_X/Y + BTN_TOUCH/
    // TOOL_*/LEFT). Xbox pads and DS4s whose touch node failed to open return false.
    bool submitTouchpad(uint32_t serial, const TouchpadReport& report) override;

    // TOUCHPAD_MODE_MOUSE via a host-global pointer node, created lazily on first
    // use. Button level is tracked so a held click fires one press / one release.
    bool submitRelativeMouse(int dx, int dy, bool leftButton) override;
    bool supportsRelativeMouse() const override { return true; }

    // uinput has no battery ioctl (UI_SET_BATTERY doesn't exist; the kernel
    // surface in /sys/class/power_supply is in-tree-driver-only), so we mirror
    // each sample to /tmp/satellite/controller<serial>/battery for userspace
    // consumers (OBS overlays, LED strips, status bars). Also logged on arrival.
    bool submitBattery(uint32_t serial, const BatteryReport& report) override;

    // uinput has no RGB readback channel (EV_LED is single-bit), so this rarely
    // fires from kernel events. The wrapper mirrors any fired colour to
    // /tmp/satellite/controller<serial>/lightbar ("RRGGBB\n") for userspace
    // tooling; wiring matches the Windows ViGEm path for a future bridge.
    void setLightbarCallback(LightbarCallback cb) override;

#ifdef SATELLITE_BUILD_TESTS
    // Test-only (SATELLITE_BUILD_TESTS, test target only): drives the lightbar
    // callback synthetically since uinput has no RGB readback to fire it from a
    // real game. No-op if no callback installed. Thread-safe.
    void invokeLightbarForTest(uint32_t serial, uint8_t r, uint8_t g, uint8_t b);
#endif

    // sysfs-proxy base dir (default "/tmp/satellite"). Tests override via
    // SATELLITE_SYSFS_PROXY_DIR to avoid racing a running daemon's /tmp tree.
    static std::string sysfsProxyDir();
    static bool writeSysfsProxyFile(uint32_t serial, const char* leaf, const std::string& contents);

  private:
    // Per-virtual-device record. Owns the uinput fd, the FF effect table the
    // kernel hands us via UI_FF_UPLOAD, and a reader thread draining FF events.
    struct Device {
        int fd = -1;
        // DS4 gyro/accel node (INPUT_PROP_ACCELEROMETER); -1 for Xbox pads or
        // on open failure (best-effort, non-fatal).
        int motionFd = -1;
        bool ds4 = false;
        std::thread readerThread;
        std::atomic<bool> readerRunning{false};
        // One-byte write wakes the reader's poll() at unplug ("exit now").
        int wakePipeRead = -1;
        int wakePipeWrite = -1;

        // kernel effect-id → magnitudes from the latest UI_FF_UPLOAD. EV_FF
        // value>0 plays at these; value==0 stops (we re-emit zeros to the dish).
        struct EffectMags {
            uint16_t strong = 0;
            uint16_t weak = 0;
        };
        std::unordered_map<int, EffectMags> effects;

        // DS4 touchpad MT node; -1 for Xbox pads or on open failure (non-fatal).
        // A fresh touchTrackingId is allocated per touch-down so consumers see
        // discrete contacts; touchSlotId holds the live id per slot (-1 = lifted).
        int touchFd = -1;
        int32_t touchTrackingId = 0;
        int32_t touchSlotId[2] = {-1, -1};
    };

    // Xbox 360 pad (ds4=false) or DualShock 4 (ds4=true). FF_RUMBLE always
    // enabled so games see force-feedback. Returns fd, or -1 on failure.
    int openUinputDevice(uint32_t serial, bool ds4);

    // DS4 motion node (INPUT_PROP_ACCELEROMETER: ABS_X/Y/Z accel, RX/RY/RZ gyro).
    int openMotionUinputDevice(uint32_t serial);

    // DS4 touchpad MT-B clickpad node (+ BTN_LEFT). Returns fd, or -1.
    int openTouchpadUinputDevice(uint32_t serial);

    // Host-global TOUCHPAD_MODE_MOUSE pointer node. Returns fd, or -1.
    int openRelMouseUinputDevice();

    void startReader(uint32_t serial, Device& dev); // caller holds mtx_
    void stopReader(uint32_t serial);               // caller holds mtx_
    void readerLoop(uint32_t serial, int fd, int wakeFd, bool isDS4);

    mutable std::mutex mtx_;
    bool busOpen_ = false;
    std::unordered_map<uint32_t, Device> devices_;

    // Copied under mtx_ before each call so the reader doesn't hold the lock
    // across the user-supplied callback.
    RumbleCallback rumbleCb_;

    // Wraps the SessionService sink so each RGB change also lands in
    // <sysfsProxyDir>/controller<serial>/lightbar.
    LightbarCallback lightbarCb_;

    // Host-global pointer node, lazily created on first submitRelativeMouse,
    // destroyed in closeBus. relMouseBtnDown_ caches the button level.
    int relMouseFd_ = -1;
    bool relMouseBtnDown_ = false;
};
