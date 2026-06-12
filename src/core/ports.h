// SPDX-License-Identifier: LGPL-3.0-or-later

// Outbound port interfaces (hexagonal). SessionService depends only on these;
// concrete adapters implement them, injected via constructor.
#pragma once

#include "types.h"
#include "update_types.h"

#include <atomic>
#include <functional>

// Virtual gamepad synthesis. Windows: ViGEmAdapter (ViGEmBus). Linux:
// GamepadAdapter (/dev/uinput). macOS: no backend, isBusOpen() always false.
//
// Rumble flow: the game on the receiver host writes the virtual device's rumble
// channel (XInputSetState / evdev EVIOCSFF); the adapter delivers those to the
// single sink registered via setRumbleCallback, which SessionService installs at
// construction so they can be encrypted and forwarded back to the dish.
class IGamepadPort {
  public:
    virtual ~IGamepadPort() = default;

    virtual bool ensureBusOpen() = 0; // lazy open; true if usable
    virtual void closeBus() = 0;
    virtual bool isBusOpen() const = 0;

    virtual bool pluginDevice(uint32_t serial) = 0;    // virtual Xbox 360
    virtual bool pluginDeviceDS4(uint32_t serial) = 0; // virtual DualShock 4

    // True iff the device is gone (or was never plugged). False means the
    // backend could not confirm removal — the caller MUST quarantine the
    // serial so a zombie target can't poison the next plug on it.
    virtual bool unplugDevice(uint32_t serial) = 0;

    // Adapter truth for "a virtual device exists on this serial right now".
    // Drives the dashboard's pluggedIn instead of inferring from serialNo > 0.
    // Default mirrors the legacy inference for backends without slot state.
    virtual bool isDevicePlugged(uint32_t serial) const { return serial != 0; }

    virtual bool submitReport(uint32_t serial, const GamepadReport& report) = 0;
    virtual bool submitDS4Report(uint32_t serial, const GamepadReport& report) = 0;

    // Rumble sink, invoked from a platform worker thread. The callback must
    // stay callable until the adapter is destroyed (SessionService outlives
    // both adapter and callback).
    using RumbleCallback = std::function<void(uint32_t serial, const RumbleReport& report)>;
    virtual void setRumbleCallback(RumbleCallback cb) = 0;

    // Submit an IMU sample. Default no-op so adapters compile unchanged; only
    // DS4-capable backends override. Xbox 360 has no IMU surface and drops.
    virtual bool submitMotion(uint32_t /*serial*/, const MotionReport& /*report*/) { return false; }

    // Does this backend have an IMU surface for `controllerType`? CAP_MOTION
    // describes what the sender streams; this describes what the receiver can
    // land — the intersection is what moves a game's camera. Default false;
    // ViGEm/uinput override true only for PLAYSTATION (DS4 has IMU, Xbox 360
    // doesn't). Surfaced as CtrlInfo::motionSinkSupportedForType.
    virtual bool supportsMotionForType(uint8_t /*controllerType*/) const { return false; }

    // True iff the per-serial IMU sink was created at plug-in. On Linux this is
    // `motionFd >= 0`; false means the kernel rejected the accelerometer device
    // (kernel too old, no /dev/uinput permission). Lets the UI tell "no game
    // subscribed" from "couldn't create the IMU node." Default true.
    virtual bool motionBackendOk(uint32_t /*serial*/) const { return true; }

    // Battery update. Default no-op. Windows DS4 wires this to the DS4_REPORT_EX
    // battery byte; other backends drop silently (SessionService still caches).
    virtual bool submitBattery(uint32_t /*serial*/, const BatteryReport& /*report*/) {
        return false;
    }

    // Touchpad sample (TOUCHPAD_MODE_DS4). ViGEm DS4 / Linux multitouch node
    // take it; Xbox pads have no surface and the default no-op drops it
    // (SessionService still caches for the web UI).
    virtual bool submitTouchpad(uint32_t /*serial*/, const TouchpadReport& /*report*/) {
        return false;
    }

    // Inject a relative mouse movement (TOUCHPAD_MODE_MOUSE). `dx`/`dy` are
    // pixels; `leftButton` is the button *level* (not an edge) — the adapter
    // emits press/release only on change. Host-global, deliberately not keyed
    // on a serial. SendInput on Windows, uinput pointer on Linux, no-op macOS.
    virtual bool submitRelativeMouse(int /*dx*/, int /*dy*/, bool /*leftButton*/) { return false; }

    // Whether submitRelativeMouse can reach the host at all. Drives the
    // mouseControl host-feature grant on the session PUT. Default false so
    // inert backends (macOS) deny rather than silently swallow the stream.
    virtual bool supportsRelativeMouse() const { return false; }

    // Lightbar sink, independent of rumble (coalesced on its own callback) so a
    // game that only sets colour still drives the LED. Backends without an
    // independent lightbar channel (Xbox 360, uinput) install a no-op stub.
    using LightbarCallback = std::function<void(uint32_t serial, uint8_t r, uint8_t g, uint8_t b)>;
    virtual void setLightbarCallback(LightbarCallback /*cb*/) {}
};

// Send encrypted UDP packets to clients.
class IClientPort {
  public:
    virtual ~IClientPort() = default;

    // Cold-path / test variant with a string IP already on hand.
    virtual void updateClientAddr(uint32_t token, const std::string& ip, uint16_t port) = 0;

    // Hot-path variant: IPv4 in network byte order (matches the raw sockaddr
    // the receiver has). Default no-op so the entry point doesn't drag winsock
    // into every consumer; the production ClientAdapter overrides it.
    virtual void updateClientAddrV4(uint32_t /*token*/, uint32_t /*ipv4NetworkOrder*/,
                                    uint16_t /*port*/) {}

    virtual void removeClientAddr(uint32_t token) = 0;

    // Enriched heartbeat ack (0x0003): server status + the session epoch and
    // active-controller bitmap the client reconciles against. Payload:
    // backendAvailable(1) + totalActiveControllers(1) + epoch(u16 BE) +
    // bitmap(u16 BE).
    virtual void sendHeartbeatAck(const Connection& conn, bool backendAvailable,
                                  uint8_t totalActiveControllers, uint16_t epoch,
                                  uint16_t activeBitmap) = 0;

    // Best-effort close notify (0x000F), payload reason(1) = CLOSE_REASON_*.
    // Must be sent BEFORE teardown while the session key and address exist.
    virtual void sendSessionClose(const Connection& conn, uint8_t reason) = 0;

    // Rumble (0x0009), encrypted 7-byte inner payload:
    //   ctrlIdx u8, strongMag u16 BE, weakMag u16 BE, durMs u16 BE.
    // Motor vibration only; lightbar colour has its own message (sendLightbar).
    virtual void sendRumble(const Connection& conn, uint8_t ctrlIdx,
                            const RumbleReport& report) = 0;

    // Lightbar (0x000D), payload: ctrlIdx u8, r/g/b u8×3. Decoupled from rumble
    // so games that only set colour still drive the LED.
    virtual void sendLightbar(const Connection& conn, uint8_t ctrlIdx, uint8_t r, uint8_t g,
                              uint8_t b) = 0;
};

// Logging.
class ILogPort {
  public:
    virtual ~ILogPort() = default;

    virtual void logMsg(LogLevel level, const std::string& source, const std::string& message) = 0;
};

// OTA update IO. All long-running methods are synchronous; UpdateService runs
// them on a dedicated worker thread so the tray + http threads stay responsive.
//   Windows: WinHTTP → SatelliteSetup-vX.Y.Z.exe → Inno /VERYSILENT, relaunch.
//   macOS:   NSURLSession → .zip → atomic satellite.app swap → relaunch.
//   Linux:   libcurl → .AppImage replace-in-place, or `apt upgrade` (Manual).
class IUpdaterPort {
  public:
    virtual ~IUpdaterPort() = default;

    // Resolve the latest release for channel ("stable"/"prerelease"). Sets
    // out.available=true iff strictly newer than currentVersion. outError
    // carries a user-displayable message on failure.
    virtual bool fetchLatestRelease(const std::string& channel, const std::string& currentVersion,
                                    UpdateInfo& out, std::string& outError) = 0;

    // Download `info`'s artifact to a temp location, reporting progress. `cancel`
    // (when non-null) is polled; if it flips true mid-stream the download aborts
    // and returns false with outError="cancelled".
    virtual bool downloadArtifact(
        const UpdateInfo& info,
        const std::function<void(uint64_t bytesSoFar, uint64_t totalBytes)>& onProgress,
        const std::atomic<bool>* cancel, std::string& outLocalPath, std::string& outError) = 0;

    // Verify SHA-256 against info.assetSha256. If the release shipped no digest
    // (empty), the adapter may do basic sanity checks and return true — but must
    // never return true on a *mismatch*.
    virtual bool verifyArtifact(const std::string& localPath, const UpdateInfo& info,
                                std::string& outError) = 0;

    // Apply the artifact and typically exit so the installer can replace the
    // binary. Caller owns the graceful-shutdown sequence after this returns true
    // (Windows Inno /CLOSEAPPLICATIONS waits on us). No-op true when Manual.
    virtual bool applyUpdate(const std::string& localPath, const UpdateInfo& info,
                             std::string& outError) = 0;

    // Stable install-lineage id: "windows", "macos", "linux-appimage",
    // "linux-deb" (manual apt), or "linux-portable" (manual upgrade).
    virtual std::string platformId() const = 0;
};
