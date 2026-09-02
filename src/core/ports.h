// SPDX-License-Identifier: LGPL-3.0-or-later

// Outbound port interfaces (hexagonal). SessionService depends only on these;
// concrete adapters implement them, injected via constructor.
#pragma once

#include "types.h"
#include "update_types.h"

#include <atomic>
#include <cstddef>
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

    virtual bool pluginDevicePreferring(uint32_t serial, GamepadIdentity identity,
                                        const std::string& preferredBackend) {
        (void)preferredBackend;
        return pluginDevice(serial, identity);
    }

    virtual const char* backendId() const { return ""; }

    virtual const char* backendIdForSerial(uint32_t serial) const {
        return isDevicePlugged(serial) ? backendId() : "";
    }

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

    // Inject a relative mouse frame (TOUCHPAD_MODE_MOUSE). `dx`/`dy` are pixels;
    // `buttons` are levels (not edges), so the adapter emits press/release only on
    // change. `wheelV` is a signed wheel event, 120 per notch, already de-duplicated
    // by the caller. Host-global, not keyed on a serial.
    virtual bool submitRelativeMouse(int /*dx*/, int /*dy*/, const MouseButtons& /*buttons*/,
                                     int /*wheelV*/) {
        return false;
    }

    // Whether submitRelativeMouse can reach the host. Drives the mouseControl
    // host-feature grant. Default false so inert backends deny rather than
    // silently swallow the stream.
    virtual bool supportsRelativeMouse() const { return false; }

    // Lightbar sink, independent of rumble, so a game that only sets colour
    // still drives the LED. Backends without one install a no-op stub.
    using LightbarCallback = std::function<void(uint32_t serial, uint8_t r, uint8_t g, uint8_t b)>;
    virtual void setLightbarCallback(LightbarCallback /*cb*/) {}

    // DualSense adaptive-trigger sink: fires with the raw left/right effect
    // blocks whenever a game rewrites them on the virtual pad. Only backends
    // that surface raw DS5 output reports (HIDMaestro) ever call it.
    using TriggerEffectsCallback =
        std::function<void(uint32_t serial, const TriggerEffectsReport& report)>;
    virtual void setTriggerEffectsCallback(TriggerEffectsCallback /*cb*/) {}

    // Player-indicator LED sink (DualSense 5-LED bar, Switch Pro 4 LEDs).
    using PlayerLedsCallback = std::function<void(uint32_t serial, uint8_t ledMask)>;
    virtual void setPlayerLedsCallback(PlayerLedsCallback /*cb*/) {}

    // Controller audio: the emulated pad's OWN endpoints, never host game
    // audio. Only a backend that can materialize an audio-carrying persona
    // (HIDMaestro's composite profiles) wires any of the three; the defaults
    // keep every other backend inert rather than silently swallowing PCM.

    // Push one AUDIO_FRAME_MS window into the pad's microphone endpoint:
    // `samples` mono frames at AUDIO_SAMPLE_RATE_HZ. False = no mic endpoint on
    // this serial, which is not an error (senders keep streaming).
    virtual bool submitMicAudioPcm(uint32_t /*serial*/, const int16_t* /*mono48k*/,
                                   size_t /*samples*/) {
        return false;
    }

    // Speaker sink: fires with the PCM a game wrote to the pad's speaker /
    // headset endpoint. `frames` counts per-channel frames; the buffer is
    // interleaved stereo at AUDIO_SAMPLE_RATE_HZ and is valid ONLY for the
    // duration of the call (it aliases the backend's ring).
    using SpeakerAudioCallback =
        std::function<void(uint32_t serial, const int16_t* stereo48k, size_t frames)>;
    virtual void setSpeakerAudioCallback(SpeakerAudioCallback /*cb*/) {}

    // Mic-mute LED sink: the MIC_LED_STATE_* the game asked the pad's mute lamp
    // for. Separate from the player LEDs; different lamp, different report bit.
    using MicLedCallback = std::function<void(uint32_t serial, uint8_t state)>;
    virtual void setMicLedCallback(MicLedCallback /*cb*/) {}
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

    // Trigger effects (0x0010), 23-byte payload: ctrlIdx u8, left block (11),
    // right block (11), raw DS5 bytes.
    virtual void sendTriggerEffects(const Connection& conn, uint8_t ctrlIdx,
                                    const TriggerEffectsReport& report) = 0;

    // Player LEDs (0x0011), payload: ctrlIdx u8, ledMask u8.
    virtual void sendPlayerLeds(const Connection& conn, uint8_t ctrlIdx, uint8_t ledMask) = 0;

    // Speaker audio (0x0013): ctrlIdx u8, seq u16 BE, then one 20 ms Opus
    // packet. Lossy by contract: no ack, no retransmit, no reordering on this
    // side; the receiver conceals gaps with Opus FEC/PLC.
    virtual void sendSpeakerAudio(const Connection& conn, uint8_t ctrlIdx, uint16_t seq,
                                  const uint8_t* opus, size_t opusLen) = 0;

    // Mic-mute LED (0x0014), payload: ctrlIdx u8, state u8 (MIC_LED_STATE_*).
    virtual void sendMicLed(const Connection& conn, uint8_t ctrlIdx, uint8_t state) = 0;
};

class ILogPort {
  public:
    virtual ~ILogPort() = default;

    virtual void logMsg(LogLevel level, const std::string& source, const std::string& message) = 0;
};

// OTA update IO. All long-running methods are synchronous; UpdateService runs
// them on a dedicated worker thread so the tray + http threads stay responsive.
//   Windows: WinHTTP, SatelliteSetup-X.Y.Z.exe, Inno /VERYSILENT, relaunch.
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
