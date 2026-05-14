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
    // Wire format (encrypted inner payload):
    //
    //   ctrlIdx   : u8
    //   strongMag : u16 BE
    //   weakMag   : u16 BE
    //   durMs     : u16 BE
    //   flags     : u8   (bit 0 = lightbar present)
    //   [lightbarR, lightbarG, lightbarB : u8 × 3 — only when flag bit 0 set]
    //
    // Senders that don't recognise the trailing lightbar bytes simply ignore
    // them; the leading 8 bytes are the mandatory subset every dish parses.
    virtual void sendRumble(const Connection& conn, uint8_t ctrlIdx,
                            const RumbleReport& report) = 0;
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
