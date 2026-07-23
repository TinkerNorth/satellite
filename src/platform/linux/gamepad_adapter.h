// SPDX-License-Identifier: LGPL-3.0-or-later
// Button codes/axis ranges follow the in-tree xpad and hid-sony evdev
// conventions so SDL2 and Steam Input recognize them as first-class controllers.
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
    bool pluginDevice(uint32_t serial, GamepadIdentity identity) override;
    bool supportsIdentity(GamepadIdentity identity) const override;
    bool unplugDevice(uint32_t serial) override;
    bool isDevicePlugged(uint32_t serial) const override;
    bool submitReport(uint32_t serial, const GamepadReport& report) override;
    void setRumbleCallback(RumbleCallback cb) override;

    // DS4 only; Xbox pads and devices whose motion node failed to open return
    // false. SessionService still caches the sample for the web UI.
    bool submitMotion(uint32_t serial, const MotionReport& report) override;

    // Backend-shape query (not per-serial): only the DS4 profile has a motion node.
    bool supportsMotionForType(uint8_t controllerType) const override;

    // True iff this serial's motion node was created at plug-in time, to
    // distinguish a kernel-rejected backend from "no game subscribed".
    bool motionBackendOk(uint32_t serial) const override;

    // DS4 only. Xbox pads and DS4s whose touch node failed to open return false.
    bool submitTouchpad(uint32_t serial, const TouchpadReport& report) override;

    // Button level is tracked so a held click fires one press/one release.
    bool submitRelativeMouse(int dx, int dy, bool leftButton) override;
    bool supportsRelativeMouse() const override { return true; }

    // uinput has no battery ioctl, so mirror each sample to
    // /tmp/satellite/controller<serial>/battery for userspace consumers.
    bool submitBattery(uint32_t serial, const BatteryReport& report) override;

    // uinput has no RGB readback (EV_LED is single-bit), so this rarely fires.
    // The wrapper mirrors any fired colour to the sysfs-proxy lightbar file;
    // wiring matches the Windows ViGEm path for a future bridge.
    void setLightbarCallback(LightbarCallback cb) override;

#ifdef SATELLITE_BUILD_TESTS
    // Drives the lightbar callback synthetically since uinput has no RGB readback
    // to fire it from a real game. Thread-safe.
    void invokeLightbarForTest(uint32_t serial, uint8_t r, uint8_t g, uint8_t b);
#endif

    // Tests override the base dir via SATELLITE_SYSFS_PROXY_DIR to avoid racing a
    // running daemon's /tmp tree.
    static std::string sysfsProxyDir();
    static bool writeSysfsProxyFile(uint32_t serial, const char* leaf, const std::string& contents);

  private:
    struct Device {
        int fd = -1;
        int motionFd = -1; // motion-capable types only; -1 for Xbox or on open failure
        GamepadIdentity identity = GamepadIdentity::Xbox;
        std::thread readerThread;
        std::atomic<bool> readerRunning{false};
        // One-byte write wakes the reader's poll() at unplug.
        int wakePipeRead = -1;
        int wakePipeWrite = -1;

        // kernel effect-id to magnitudes from the latest UI_FF_UPLOAD. EV_FF
        // value>0 plays at these; value==0 stops (re-emit zeros to the dish).
        struct EffectMags {
            uint16_t strong = 0;
            uint16_t weak = 0;
        };
        std::unordered_map<int, EffectMags> effects;

        // Fresh touchTrackingId per touch-down so consumers see discrete
        // contacts; touchSlotId holds the live id per slot (-1 = lifted).
        int touchFd = -1;
        int32_t touchTrackingId = 0;
        int32_t touchSlotId[2] = {-1, -1};
    };

    int openUinputDevice(uint32_t serial, GamepadIdentity identity);
    int openMotionUinputDevice(uint32_t serial);
    int openTouchpadUinputDevice(uint32_t serial);
    int openRelMouseUinputDevice();

    void startReader(uint32_t serial, Device& dev); // caller holds mtx_
    void stopReader(uint32_t serial);               // caller holds mtx_
    void readerLoop(uint32_t serial, int fd, int wakeFd, bool isDS4);

    // Switch Pro has a distinct evdev layout (Nintendo A/B + X/Y swap, ZL/ZR as
    // buttons); caller holds mtx_.
    bool submitSwitchLocked(int fd, const GamepadReport& report);

    mutable std::mutex mtx_;
    bool busOpen_ = false;
    std::unordered_map<uint32_t, Device> devices_;

    // Copied under mtx_ before each call so the reader doesn't hold the lock
    // across the user-supplied callback.
    RumbleCallback rumbleCb_;

    LightbarCallback lightbarCb_;

    // Lazily created on first submitRelativeMouse, destroyed in closeBus.
    int relMouseFd_ = -1;
    bool relMouseBtnDown_ = false;
};
