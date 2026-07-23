// SPDX-License-Identifier: LGPL-3.0-or-later

// Outbound port interfaces (hexagonal). SessionService depends only on these;
// concrete adapters implement them, injected via constructor.
#pragma once

#include "types.h"
#include "update_types.h"

#include <atomic>
#include <functional>

// Virtual gamepad synthesis. Windows: ViGEmAdapter (ViGEmBus). Linux:
// GamepadAdapter (/dev/uinput). macOS: MacHidGamepadAdapter (IOHIDUserDevice;
// inert without the HID virtual-device entitlement, isBusOpen() then false).
//
// Rumble flow: the game on the receiver host writes the virtual device's rumble
// channel; the adapter delivers those to the single sink registered via
// setRumbleCallback (installed by SessionService at construction) to be
// encrypted and forwarded back to the dish.
class IGamepadPort {
  public:
    virtual ~IGamepadPort() = default;

    virtual bool ensureBusOpen() = 0; // lazy open; true if usable
    virtual void closeBus() = 0;
    virtual bool isBusOpen() const = 0;

    // Plug a virtual device of the given materialization identity. The adapter
    // records the identity per serial so submitReport packs the right report.
    virtual bool pluginDevice(uint32_t serial, GamepadIdentity identity) = 0;

    // Can this backend materialize `identity`? Gates per-backend catalog offers
    // and the invalidType apply result. Default = the two universally-emulable
    // identities; adapters widen or narrow.
    virtual bool supportsIdentity(GamepadIdentity identity) const {
        return identity == GamepadIdentity::Xbox || identity == GamepadIdentity::DS4;
    }

    // True iff the device is gone (or was never plugged). False means removal
    // was unconfirmed: the caller MUST quarantine the serial so a zombie target
    // can't poison the next plug on it.
    virtual bool unplugDevice(uint32_t serial) = 0;

    // Adapter truth for "a virtual device exists on this serial right now".
    // Default mirrors the legacy inference for backends without slot state.
    virtual bool isDevicePlugged(uint32_t serial) const { return serial != 0; }

    // Submit input for a plugged serial; the adapter packs per the identity it
    // recorded at plug.
    virtual bool submitReport(uint32_t serial, const GamepadReport& report) = 0;

    // Rumble sink, invoked from a platform worker thread. Must stay callable
    // until the adapter is destroyed (SessionService outlives it).
    using RumbleCallback = std::function<void(uint32_t serial, const RumbleReport& report)>;
    virtual void setRumbleCallback(RumbleCallback cb) = 0;

    // Submit an IMU sample. Default no-op; only DS4-capable backends override.
    virtual bool submitMotion(uint32_t /*serial*/, const MotionReport& /*report*/) { return false; }

    // Does this backend have an IMU surface for `controllerType`? CAP_MOTION is
    // what the sender streams; this is what the receiver can land. Default false;
    // ViGEm/uinput override true for the motion-capable types
    // (controllerTypeHasMotion). Surfaced as CtrlInfo::motionSinkSupportedForType.
    virtual bool supportsMotionForType(uint8_t /*controllerType*/) const { return false; }

    // True iff the per-serial IMU sink was created at plug-in. False means the
    // kernel rejected the accelerometer device. Lets the UI tell "no game
    // subscribed" from "couldn't create the IMU node". Default true.
    virtual bool motionBackendOk(uint32_t /*serial*/) const { return true; }

    // Battery update. Default no-op. Windows DS4 wires this to the DS4_REPORT_EX
    // battery byte; other backends drop (SessionService still caches).
    virtual bool submitBattery(uint32_t /*serial*/, const BatteryReport& /*report*/) {
        return false;
    }

    // Touchpad sample (TOUCHPAD_MODE_DS4). Xbox pads have no surface and the
    // default no-op drops it (SessionService still caches for the web UI).
    virtual bool submitTouchpad(uint32_t /*serial*/, const TouchpadReport& /*report*/) {
        return false;
    }

    // Inject a relative mouse movement (TOUCHPAD_MODE_MOUSE). `dx`/`dy` are
    // pixels; `leftButton` is the button level (not an edge), so the adapter
    // emits press/release only on change. Host-global, not keyed on a serial.
    virtual bool submitRelativeMouse(int /*dx*/, int /*dy*/, bool /*leftButton*/) { return false; }

    // Whether submitRelativeMouse can reach the host. Drives the mouseControl
    // host-feature grant. Default false so inert backends deny rather than
    // silently swallow the stream.
    virtual bool supportsRelativeMouse() const { return false; }

    // Lightbar sink, independent of rumble, so a game that only sets colour
    // still drives the LED. Backends without one install a no-op stub.
    using LightbarCallback = std::function<void(uint32_t serial, uint8_t r, uint8_t g, uint8_t b)>;
    virtual void setLightbarCallback(LightbarCallback /*cb*/) {}
};

// Send encrypted UDP packets to clients.
class IClientPort {
  public:
    virtual ~IClientPort() = default;

    // Cold-path / test variant with a string IP already on hand.
    virtual void updateClientAddr(uint32_t token, const std::string& ip, uint16_t port) = 0;

    // Hot-path variant: IPv4 in network byte order. Default no-op so the entry
    // point doesn't drag winsock into every consumer; ClientAdapter overrides it.
    virtual void updateClientAddrV4(uint32_t /*token*/, uint32_t /*ipv4NetworkOrder*/,
                                    uint16_t /*port*/) {}

    virtual void removeClientAddr(uint32_t token) = 0;

    // Enriched heartbeat ack (0x0003). Payload: backendAvailable(1) +
    // totalActiveControllers(1) + epoch(u16 BE) + bitmap(u16 BE).
    virtual void sendHeartbeatAck(const Connection& conn, bool backendAvailable,
                                  uint8_t totalActiveControllers, uint16_t epoch,
                                  uint16_t activeBitmap) = 0;

    // Best-effort close notify (0x000F), payload reason(1) = CLOSE_REASON_*.
    // Must be sent BEFORE teardown while the session key and address exist.
    virtual void sendSessionClose(const Connection& conn, uint8_t reason) = 0;

    // Rumble (0x0009), 7-byte payload: ctrlIdx u8, strongMag u16 BE, weakMag
    // u16 BE, durMs u16 BE. Motor only; lightbar has its own message.
    virtual void sendRumble(const Connection& conn, uint8_t ctrlIdx,
                            const RumbleReport& report) = 0;

    // Lightbar (0x000D), payload: ctrlIdx u8, r/g/b u8x3.
    virtual void sendLightbar(const Connection& conn, uint8_t ctrlIdx, uint8_t r, uint8_t g,
                              uint8_t b) = 0;
};

class ILogPort {
  public:
    virtual ~ILogPort() = default;

    virtual void logMsg(LogLevel level, const std::string& source, const std::string& message) = 0;
};

// OTA update IO. All long-running methods are synchronous; UpdateService runs
// them on a dedicated worker thread so the tray + http threads stay responsive.
//   Windows: WinHTTP, SatelliteSetup-vX.Y.Z.exe, Inno /VERYSILENT, relaunch.
//   macOS:   NSURLSession, .zip, atomic satellite.app swap, relaunch.
//   Linux:   libcurl, .AppImage replace-in-place, or `apt upgrade` (Manual).
class IUpdaterPort {
  public:
    virtual ~IUpdaterPort() = default;

    // Resolve the latest release for channel ("stable"/"prerelease"). Sets
    // out.available=true iff strictly newer than currentVersion. outError
    // carries a user-displayable message on failure.
    virtual bool fetchLatestRelease(const std::string& channel, const std::string& currentVersion,
                                    UpdateInfo& out, std::string& outError) = 0;

    // Download `info`'s artifact to a temp location, reporting progress. `cancel`
    // (when non-null) is polled; if it flips true the download aborts and
    // returns false with outError="cancelled".
    virtual bool downloadArtifact(
        const UpdateInfo& info,
        const std::function<void(uint64_t bytesSoFar, uint64_t totalBytes)>& onProgress,
        const std::atomic<bool>* cancel, std::string& outLocalPath, std::string& outError) = 0;

    // Verify SHA-256 against info.assetSha256. If the release shipped no digest,
    // the adapter may return true, but must never return true on a mismatch.
    virtual bool verifyArtifact(const std::string& localPath, const UpdateInfo& info,
                                std::string& outError) = 0;

    // Apply the artifact and typically exit so the installer can replace the
    // binary. Caller owns the graceful-shutdown sequence after this returns true.
    // No-op true when Manual.
    virtual bool applyUpdate(const std::string& localPath, const UpdateInfo& info,
                             std::string& outError) = 0;

    // Stable install-lineage id: "windows", "macos", "linux-appimage",
    // "linux-deb" (manual apt), or "linux-portable" (manual upgrade).
    virtual std::string platformId() const = 0;
};
