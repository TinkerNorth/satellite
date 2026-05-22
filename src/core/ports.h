// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * core/ports.h — Outbound port interfaces (Hexagonal Architecture).
 *
 * The SessionService depends ONLY on these interfaces.
 * Concrete adapters implement them, injected via constructor.
 */
#pragma once

#include "types.h"
#include "update_types.h"

#include <atomic>
#include <functional>

// ── Outbound: Virtual gamepad synthesis ─────────────────────────────────────
// Windows: backed by ViGEmAdapter (ViGEmBus driver).
// Linux:   backed by GamepadAdapter (/dev/uinput).
// macOS:   no backend — IGamepadPort impl reports isBusOpen() == false always.
//
// For backend-availability checks (e.g. for the web UI's diagnostic panel),
// callers use ::probeBackend() from core/gamepad_backend.h directly.
//
// Rumble flow: the *game* on the receiver host writes to the virtual device's
// rumble channel (XInputSetState on Windows, evdev EVIOCSFF on Linux). The
// platform adapter delivers those events to a single sink registered via
// `setRumbleCallback`, which the SessionService installs at construction so
// they can be encrypted and forwarded back to the dish. macOS has no backend,
// so its impl ignores the registration silently.
class IGamepadPort {
  public:
    virtual ~IGamepadPort() = default;

    // Open the bus (lazy). Returns true if bus is usable.
    virtual bool ensureBusOpen() = 0;

    // Close the bus (called when no controllers remain).
    virtual void closeBus() = 0;

    // Is the bus currently open?
    virtual bool isBusOpen() const = 0;

    // Plug in a virtual Xbox 360 controller. Returns true on success.
    virtual bool pluginDevice(uint32_t serial) = 0;

    // Plug in a virtual DualShock 4 controller. Returns true on success.
    virtual bool pluginDeviceDS4(uint32_t serial) = 0;

    // Unplug a virtual controller (any type).
    virtual void unplugDevice(uint32_t serial) = 0;

    // Submit an Xbox 360 gamepad report. Returns true on success.
    virtual bool submitReport(uint32_t serial, const GamepadReport& report) = 0;

    // Submit a DS4 gamepad report (converted from GamepadReport). Returns true on success.
    virtual bool submitDS4Report(uint32_t serial, const GamepadReport& report) = 0;

    // Install a rumble sink. Backends invoke `cb(serial, report)` from a
    // platform-specific worker thread whenever a game updates the virtual
    // device's vibration channel. The callback is owned by the adapter for
    // the rest of its lifetime; it must remain callable until the adapter is
    // destroyed (the SessionService outlives both adapter and callback).
    using RumbleCallback = std::function<void(uint32_t serial, const RumbleReport& report)>;
    virtual void setRumbleCallback(RumbleCallback cb) = 0;

    // Submit an IMU sample (gyro + accel) to the virtual device. Defaults to a
    // no-op so existing adapters compile unchanged: ViGEm DS4 (Windows) and
    // uhid DualSense (Linux, future) override this to forward the sample to
    // the backend's motion fields. The Xbox 360 emulation has no IMU surface
    // and silently drops; senders shouldn't notice the difference because
    // motion is only useful when the virtual device advertises a motion cap.
    virtual bool submitMotion(uint32_t /*serial*/, const MotionReport& /*report*/) { return false; }

    // Does this backend have an IMU surface for [controllerType]? Used by
    // SessionService::handleControllerAdd to populate
    // ConnectionSnapshot::CtrlInfo::motionSinkSupportedForType, which the
    // web UI surfaces so the operator can see "Xbox-typed virtual pad
    // can't sink motion on this backend" without having to inspect a
    // silently-dropped sample stream.
    //
    // The CAP_MOTION bit on the dish describes what the *sender* will
    // stream. This method describes what the *receiver* can land. The
    // intersection is what actually moves a game's camera.
    //
    // Default: false for every type. Windows ViGEm overrides to return
    // true only for CONTROLLER_TYPE_PLAYSTATION (DS4 has IMU; Xbox 360
    // does not). Linux uinput likewise overrides only for PLAYSTATION
    // (DualShock4 motion node), false for Xbox. macOS keeps the default.
    virtual bool supportsMotionForType(uint8_t /*controllerType*/) const { return false; }

    // True iff backend-side motion delivery succeeded for [serial] on
    // initial plug-in. On Linux this is `motionFd >= 0` after
    // openMotionUinputDevice — false means the kernel rejected the
    // INPUT_PROP_ACCELEROMETER device (rare: kernel too old, no
    // /dev/uinput permission). The web UI surfaces this so operators can
    // tell "no game has subscribed" apart from "I couldn't even create
    // the IMU node." Defaults to true; backends that can fail to create
    // a motion sink override.
    virtual bool motionBackendOk(uint32_t /*serial*/) const { return true; }

    // Submit a battery update. Defaults to no-op. Windows DS4 backend wires
    // this to the battery byte in DS4_REPORT_EX so games (and Steam Big
    // Picture) can show charge level. Backends without a battery surface
    // (Xbox 360, uinput) drop silently; the SessionService still caches
    // the latest value for the web UI.
    virtual bool submitBattery(uint32_t /*serial*/, const BatteryReport& /*report*/) {
        return false;
    }

    // Submit a touchpad sample to the virtual device (TOUCHPAD_MODE_DS4).
    // ViGEm DS4 (Windows) writes the finger coordinates + clicky button into
    // DS4_REPORT_EX's touchpad fields; the Linux adapter emits them on a
    // dedicated multitouch uinput node. Xbox-typed virtual pads have no
    // touchpad surface and the default no-op drops the sample. The
    // SessionService still caches the latest sample on the Controller for the
    // web UI debug pane regardless of backend support.
    virtual bool submitTouchpad(uint32_t /*serial*/, const TouchpadReport& /*report*/) {
        return false;
    }

    // Inject a relative mouse movement onto the receiver host's desktop — the
    // TOUCHPAD_MODE_MOUSE routing path. The SessionService converts finger-0
    // motion into a signed host-pixel delta and maps the clicky-pad button to
    // mouse button 1. `dx`/`dy` are pixels; `leftButton` is the current button
    // *level* (not an edge) — the adapter tracks transitions and emits
    // press/release only on change.
    //
    // Deliberately not keyed on a virtual-device serial: this is a host-global
    // desktop action, distinct from the per-controller virtual-pad ports
    // above. Windows backs it with SendInput, Linux with a lazily-created
    // uinput pointer device; the inert macOS backend keeps the default no-op.
    virtual bool submitRelativeMouse(int /*dx*/, int /*dy*/, bool /*leftButton*/) { return false; }

    // Install a lightbar sink. The Windows ViGEm DS4 callback fires for every
    // lightbar colour change written by the host game (independent of rumble,
    // which we coalesce on its own callback). The SessionService installs
    // this at construction so colour changes can be queued onto MSG_LIGHTBAR
    // without piggy-backing on rumble.
    //
    // Backends without an independent lightbar channel (Xbox 360 ViGEm,
    // uinput) install a no-op stub.
    using LightbarCallback = std::function<void(uint32_t serial, uint8_t r, uint8_t g, uint8_t b)>;
    virtual void setLightbarCallback(LightbarCallback /*cb*/) {}
};

// ── Outbound: Send encrypted UDP packets to clients ─────────────────────────
class IClientPort {
  public:
    virtual ~IClientPort() = default;

    // Update the network address for a token (called on every packet recv).
    virtual void updateClientAddr(uint32_t token, const std::string& ip, uint16_t port) = 0;

    // Remove address mapping for a token.
    virtual void removeClientAddr(uint32_t token) = 0;

    // Send heartbeat ACK (0x0003) to a specific client.
    virtual void sendHeartbeatAck(const Connection& conn) = 0;

    // Send controller ACK (0x0006) to a specific client.
    virtual void sendControllerAck(const Connection& conn, uint16_t requestType, uint8_t ctrlIdx,
                                   uint8_t result) = 0;

    // Send server status (0x0007) to a specific client. The backendAvailable
    // bool maps to the wire byte at offset 4 of the encrypted payload; the
    // value is unchanged from the previous protocol revision.
    virtual void sendServerStatus(const Connection& conn, bool backendAvailable,
                                  uint8_t totalActiveControllers) = 0;

    // Broadcast server status to all provided connections.
    virtual void
    broadcastServerStatus(const std::vector<std::pair<uint32_t, const Connection*>>& connections,
                          bool backendAvailable, uint8_t totalActiveControllers) = 0;

    // Send a rumble (0x0009) update to the dish that owns `ctrlIdx` on `conn`.
    // Wire format (encrypted inner payload, 7 bytes):
    //
    //   ctrlIdx   : u8
    //   strongMag : u16 BE
    //   weakMag   : u16 BE
    //   durMs     : u16 BE
    //
    // Rumble carries motor vibration only; lightbar colour has its own
    // message (MSG_LIGHTBAR / 0x000D — see sendLightbar).
    virtual void sendRumble(const Connection& conn, uint8_t ctrlIdx,
                            const RumbleReport& report) = 0;

    // Send a lightbar (0x000D) update to the dish. Decouples colour changes
    // from rumble — games that only set lightbar (no vibration) now drive
    // the LED through this dedicated stream. Wire payload:
    //
    //   ctrlIdx : u8
    //   r, g, b : u8 × 3
    virtual void sendLightbar(const Connection& conn, uint8_t ctrlIdx, uint8_t r, uint8_t g,
                              uint8_t b) = 0;
};

// ── Outbound: Configuration persistence ─────────────────────────────────────
class IConfigPort {
  public:
    virtual ~IConfigPort() = default;

    virtual Config loadConfig() = 0;
    virtual void saveConfig(const Config& cfg) = 0;
    virtual void setAutoStart(bool enable) = 0;
    virtual bool getAutoStart() = 0;
};

// ── Outbound: Logging ───────────────────────────────────────────────────────
class ILogPort {
  public:
    virtual ~ILogPort() = default;

    virtual void logMsg(LogLevel level, const std::string& source, const std::string& message) = 0;
};

// ── Outbound: OTA update IO ─────────────────────────────────────────────────
// Windows: WinHTTP → api.github.com → SatelliteSetup-vX.Y.Z.exe → Inno
//          /VERYSILENT, then ShellExecute relaunch.
// macOS:   NSURLSession → satellite-macos-stub-vX.Y.Z.zip → unzip into a
//          staging dir → swap satellite.app atomically → relaunch.
// Linux:   libcurl → satellite-X.Y.Z-x86_64.AppImage (AppImage path), or
//          surface `apt upgrade satellite` (Manual path for .deb installs).
//
// All long-running methods are synchronous; UpdateService runs them on a
// dedicated worker thread so the tray + http threads stay responsive.
class IUpdaterPort {
  public:
    virtual ~IUpdaterPort() = default;

    // Resolve the latest release for the given channel ("stable" or
    // "prerelease"). On success fills `out` and returns true. Sets
    // out.available=true iff strictly newer than currentVersion.
    // outError carries a user-displayable message on failure.
    virtual bool fetchLatestRelease(const std::string& channel, const std::string& currentVersion,
                                    UpdateInfo& out, std::string& outError) = 0;

    // Download the per-platform artifact described by `info` to a
    // platform-appropriate temp location. Reports progress through the
    // callback (bytesSoFar, totalBytes). Returns the local file path
    // through outLocalPath on success.
    //
    // The cancel pointer (when non-null) is polled periodically by the
    // adapter; if it flips to true mid-stream the download is aborted
    // and the method returns false with outError="cancelled".
    virtual bool downloadArtifact(
        const UpdateInfo& info,
        const std::function<void(uint64_t bytesSoFar, uint64_t totalBytes)>& onProgress,
        const std::atomic<bool>* cancel, std::string& outLocalPath, std::string& outError) = 0;

    // Verify SHA-256 of localPath matches info.assetSha256. If the
    // release didn't ship a digest (assetSha256 empty), the adapter
    // may still perform basic sanity checks (file size, magic bytes)
    // and return true — but should never return true on a *mismatch*.
    virtual bool verifyArtifact(const std::string& localPath, const UpdateInfo& info,
                                std::string& outError) = 0;

    // Apply the downloaded artifact and (typically) initiate process
    // exit so the installer can replace satellite.exe / satellite.app.
    // The caller is responsible for the graceful-shutdown sequence
    // *after* this returns true; on Windows the Inno installer's
    // /CLOSEAPPLICATIONS handles waiting on us.
    //
    // For info.installMethod == Manual this is a no-op returning true.
    virtual bool applyUpdate(const std::string& localPath, const UpdateInfo& info,
                             std::string& outError) = 0;

    // Stable identifier for the binary's install lineage. One of:
    //   "windows"          — Inno installer build
    //   "macos"            — .app bundle (stub)
    //   "linux-appimage"   — AppImage self-update
    //   "linux-deb"        — Debian package, manual `apt upgrade`
    //   "linux-portable"   — `make install` / portable, manual upgrade
    virtual std::string platformId() const = 0;
};
