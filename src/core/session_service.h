// SPDX-License-Identifier: LGPL-3.0-or-later

// The ONLY place that mutates connection/controller state. All adapters call
// in; all platform concerns are behind the port interfaces.
//
// Sessions are declarative: PUT /api/connections upserts the full desired
// topology for a deviceId and this service converges the backend to it,
// returning the applied state. UDP never mutates topology (docs/contract.md).
#pragma once

#include "ports.h"
#include <mutex>
#include <unordered_map>
#include <functional>

class SessionService {
  public:
    // Derives the per-session key from the pairing key + salt + token.
    // Injected so the core stays libsodium-free; production wires HKDF-SHA256
    // (net/session_crypto.h), tests may omit it (sessionKey = pairingKey).
    using KeyDeriver = std::function<void(const uint8_t pairingKey[CRYPTO_KEY_SIZE],
                                          const uint8_t salt[SESSION_SALT_SIZE], uint32_t token,
                                          uint8_t outSessionKey[CRYPTO_KEY_SIZE])>;

    // `audioCodecs` follows the same rule as `keyDeriver`: injected so the core
    // stays libopus-free (scripts/check_core_purity.sh), production wires
    // adapters/audio/opus_codec.h's factory, and it must outlive the service.
    // Null is a supported state, not a degraded one -- a build or a suite with
    // no codec still validates, gates and rate-limits audio, it just has
    // nowhere to put the samples.
    SessionService(IGamepadPort& backend, IClientPort& client, ILogPort& log,
                   KeyDeriver keyDeriver = {}, IAudioCodecFactory* audioCodecs = nullptr);

    // Declarative session upsert, keyed on deviceId. Creates the row on first
    // contact; afterwards rotates token/salt/sessionKey in place (connectionId
    // stays stable) and converges the controller set to `descriptors`. Missing
    // slots unplugged, new slots plugged, type-family changes transactionally
    // replugged. Per-controller failures ride in the result, never abort.
    SessionUpsertResult upsertSession(const std::string& deviceId, const std::string& deviceName,
                                      const std::string& clientIP,
                                      const uint8_t pairingKey[CRYPTO_KEY_SIZE],
                                      const std::vector<ControllerDescriptor>& descriptors,
                                      bool requestMouseControl,
                                      int protocolVersion = PROTOCOL_VERSION);

    // Standalone single-controller upsert (PUT .../controllers/{idx}).
    // False when connectionId doesn't exist or isn't owned by deviceId.
    bool applyController(const std::string& connectionId, const std::string& deviceId,
                         const ControllerDescriptor& desc, ControllerApplyResult& outResult,
                         uint16_t& outEpoch);

    // Removes the SLOT only; the session lives on (zero-controller sessions are
    // valid). Idempotent. False when connectionId doesn't exist or isn't owned
    // by deviceId.
    bool removeController(const std::string& connectionId, const std::string& deviceId,
                          uint8_t ctrlIdx, uint16_t& outEpoch);

    // Applied state for the reconcile endpoint (GET /api/connections/{id}),
    // scoped to the owning device. `deviceId` empty: admin scope (any owner).
    struct SessionView {
        bool found = false;
        std::string connectionId;
        std::string deviceId;
        uint16_t epoch = 0;
        bool mouseControlGranted = false;
        int protocolVersion = PROTOCOL_VERSION;
        struct CtrlView {
            uint8_t ctrlIdx;
            uint8_t appliedType;
            uint16_t caps;
            uint8_t touchpadMode;
            bool motionSinkSupportedForType;
            bool motionBackendOk;
            std::string backendId;
            std::string preferredBackend;
        };
        std::vector<CtrlView> controllers;
    };
    SessionView getSessionView(const std::string& connectionId, const std::string& deviceId) const;

    // Close by connectionId. `reason` is broadcast via close-notify 0x000F
    // BEFORE teardown when `notify`; a client closing its own session passes
    // notify=false. Returns controllers removed, -1 when not found (or owned by
    // another device when deviceId non-empty).
    int closeSessionById(const std::string& connectionId, const std::string& deviceId,
                         uint8_t reason, bool notify);

    // Close any live session for a device (unpair path). Returns sessions closed.
    int closeSessionsForDevice(const std::string& deviceId, uint8_t reason);

    // Close all connections (receiver stop / app shutdown). Broadcasts the
    // close-notify (reason, default shutdown) before teardown.
    void closeAllSessions(uint8_t reason = CLOSE_REASON_SHUTDOWN);

    // Packet handling (called by UDP adapter).

    // True if the report was submitted successfully.
    bool handleGamepadData(uint32_t token, uint8_t ctrlIdx, const GamepadReport& report);

    // Sends the enriched heartbeat ACK (status + epoch + bitmap).
    void handleHeartbeat(uint32_t token);

    // Rumble fired by the backend's notification thread. Resolves serial to
    // controller, coalesces against last forwarded state, sends only unique
    // updates. `wireDurationMs` stamps a duration the dish can't invent (Linux
    // uinput "play" events have none); the 500 ms default matches the dish-side
    // rumble heartbeat that refreshes the actuator.
    void handleRumbleFromBackend(uint32_t serial, const RumbleReport& report,
                                 uint16_t wireDurationMs = 500);

    // IMU sample (MSG_MOTION). Caches for the web UI and forwards via
    // submitMotion when active. Returns whether the backend accepted it. False
    // is NOT an error; senders keep streaming.
    bool handleMotionData(uint32_t token, uint8_t ctrlIdx, const MotionReport& report);

    // Battery update (MSG_BATTERY). Caches the value and forwards via
    // submitBattery on platforms with a battery channel. Returns true if
    // (token, ctrlIdx) resolved to an active controller.
    bool handleBatteryUpdate(uint32_t token, uint8_t ctrlIdx, const BatteryReport& report);

    // Touchpad sample (MSG_TOUCHPAD). Always caches, then routes per the
    // controller's touchpadMode: DS4 to submitTouchpad, MOUSE to finger-0 delta
    // then submitRelativeMouse (gated on the mouseControl grant), OFF to cache
    // only. Returns whether the routed sink accepted it. False is NOT an error.
    bool handleTouchpadData(uint32_t token, uint8_t ctrlIdx, const TouchpadReport& report);

    // Lightbar change from the backend's dedicated callback. Resolves serial to
    // controller, coalesces, sends only unique updates.
    void handleLightbarFromBackend(uint32_t serial, uint8_t r, uint8_t g, uint8_t b);

    // Trigger-effect blocks from the backend's raw-output callback. Coalesces,
    // then forwards only to senders that advertised CAP_TRIGGER_EFFECTS.
    void handleTriggerEffectsFromBackend(uint32_t serial, const TriggerEffectsReport& report);

    // Player-LED bitmask from the backend's raw-output callback. Coalesces,
    // then forwards only to senders that advertised CAP_PLAYER_LEDS.
    void handlePlayerLedsFromBackend(uint32_t serial, uint8_t ledMask);

    // One MSG_MIC_AUDIO frame: the sender's microphone, headed for the emulated
    // pad's mic endpoint. `opus` is one whole 20 ms packet, valid only for this
    // call. Validates session, slot and CAP_MIC, then rate-limits, then hands
    // the packet to the decode path; returns true only when it got that far.
    // Every rejection is a SILENT drop on the wire (the stream is best-effort
    // and there is no error channel for it), logged once per session per cause.
    bool handleMicAudio(uint32_t token, uint8_t ctrlIdx, uint16_t seq, const uint8_t* opus,
                        size_t opusLen);

    // Speaker PCM from the backend's audio callback: what a game wrote to the
    // pad's speaker/headset endpoint. `frames` per-channel, interleaved stereo,
    // valid only for this call. Resolves serial to controller, gates on
    // CAP_SPEAKER, then hands the frame to the encode path.
    bool handleSpeakerAudioFromBackend(uint32_t serial, const int16_t* stereo48k, size_t frames);

    // Mic-mute LED state (MIC_LED_STATE_*) from the backend's raw-output
    // callback. Coalesces, then forwards only to senders that advertised
    // CAP_MIC: a mute lamp with no microphone behind it has nothing to report.
    void handleMicLedFromBackend(uint32_t serial, uint8_t state);

    // Look up a connection's key + last counter; false if token not found.
    bool getDecryptInfo(uint32_t token, uint8_t outKey[CRYPTO_KEY_SIZE],
                        uint32_t& outLastCounter) const;

    void updatePostDecrypt(uint32_t token, uint32_t counter, const std::string& clientIP,
                           uint16_t clientPort);

    // Hot-path variant: IPv4 in network byte order, skipping the per-packet
    // string allocation; only re-formats Connection::clientIP when the IPv4
    // actually changes (essentially never within a session).
    void updatePostDecryptV4(uint32_t token, uint32_t counter, uint32_t ipv4NetworkOrder,
                             uint16_t clientPort);

    // FUSED hot path: updatePostDecryptV4 + handleGamepadData under a SINGLE
    // mtx_ acquisition (the unfused flow took it three times per packet, adding
    // tail latency). True if the report was submitted successfully.
    bool handleGamepadDataAndUpdate(uint32_t token, uint32_t counter, uint32_t ipv4NetworkOrder,
                                    uint16_t clientPort, uint8_t ctrlIdx,
                                    const GamepadReport& report);

    // Thread-safe snapshot of all connections for JSON serialization.
    struct ConnectionSnapshot {
        std::string connectionId;
        uint32_t token;
        std::string deviceId;
        std::string deviceName;
        std::string clientIP;
        int64_t connectedAtEpoch; // steady-clock seconds (boot-relative)
        uint16_t epoch;
        int activeControllerCount;
        bool mouseControlGranted;
        int protocolVersion;
        // Always Active or NotResponding here (an existing connection is at
        // least Active). The full enum is surfaced by /api/devices.
        DeviceLinkState linkState;
        struct CtrlInfo {
            uint8_t index;
            uint32_t serial;
            bool active;
            // Adapter truth, not inferred from serial > 0.
            bool pluggedIn;
            uint8_t controllerType;
            uint8_t touchpadMode;
            // level/status unspecified until batteryKnown.
            bool batteryKnown;
            uint8_t batteryLevel;  // 0..100, or BATTERY_LEVEL_UNKNOWN
            uint8_t batteryStatus; // BATTERY_STATUS_*
            // motionCapable: CAP_MOTION advertised. motionActive: a MSG_MOTION
            // decoded. motionSink: motion also reaching the IMU surface (false =
            // cached only: Xbox, old ViGEmBus, or macOS).
            bool motionCapable;
            bool motionActive;
            bool motionSink;
            // Backend has an IMU surface for the chosen type. Lets the UI warn
            // that an Xbox-typed slot can never sink motion.
            bool motionSinkSupportedForType;
            // Per-serial IMU sink created at plug-in. False distinguishes "kernel
            // rejected the motion node" from "no game subscribed yet". Meaningful
            // on Linux; true elsewhere.
            bool motionBackendOk;
            std::string backendId;
            bool touchpadActive;
            // lightbarCapable: the CAP_LIGHTBAR bit and the gate for emitting
            // MSG_LIGHTBAR. lightbarKnown once a colour is set; r/g/b hold the
            // latest (0,0,0 until known).
            bool lightbarCapable;
            bool lightbarKnown;
            uint8_t lightbarR;
            uint8_t lightbarG;
            uint8_t lightbarB;
        };
        std::vector<CtrlInfo> controllers;
    };
    struct ConnectionsSnapshot {
        std::vector<ConnectionSnapshot> connections;
        int totalControllers;
        int maxControllers;
        bool backendAvailable;
    };
    ConnectionsSnapshot getConnectionsSnapshot() const;

    bool isDeviceConnected(const std::string& deviceId) const;

    // Per-paired-device link state (server's view): Paired when no live
    // connection, NotResponding when the live connection's last packet is past
    // the stalling threshold but not yet reaped, else Active. Linking is never
    // returned: the PUT handshake is synchronous, so the connection is either
    // absent (Paired) or present (Active).
    DeviceLinkState linkStateForDevice(const std::string& deviceId) const;

    // Remove timed-out connections (the REST-open grace window is honoured).
    // Returns number reaped.
    int reapTimedOut();

    bool isBackendAvailable() const;
    int totalActiveControllers() const;
    int availableSlots() const;

#ifdef SATELLITE_BUILD_TESTS
    // Test seam: shift a connection's liveness clocks backwards so the
    // reap/grace/stall paths are testable without real sleeps.
    void backdateForTest(uint32_t token, int lastPacketSecondsAgo, int graceSecondsAgo);
    // Test seam: reopen a controller's mic allowance so a suite can stream past
    // MIC_AUDIO_MAX_PACKETS_PER_SEC frames without spending real seconds. Any
    // audio test longer than 75 frames needs this or it measures the rate
    // limiter instead of the thing under test.
    void resetMicRateWindowForTest(uint32_t token, uint8_t ctrlIdx);
    // Test seam: preset the outbound speaker sequence, so the u16 wrap is
    // reachable without encoding 65536 frames of audio to get there.
    void setSpeakerSeqForTest(uint32_t token, uint8_t ctrlIdx, uint16_t seq);
#endif

  private:
    IGamepadPort& backend_;
    IClientPort& client_;
    ILogPort& log_;
    KeyDeriver keyDeriver_;
    // Non-owning; null when this build has no codec (see the constructor).
    IAudioCodecFactory* audioCodecs_ = nullptr;

    mutable std::mutex mtx_; // protects connections_, serial state, scan cursor
    std::unordered_map<uint32_t, Connection> connections_;
    bool serialInUse_[MAX_BACKEND_CONTROLLERS] = {};
    // A serial whose unplug could not be confirmed is quarantined (never
    // reallocated) until the backend bus closes; a zombie target on it would
    // poison the next plug. Cleared whenever the bus is (re)opened from idle.
    bool serialQuarantined_[MAX_BACKEND_CONTROLLERS] = {};
    // Round-robin scan start so a just-freed serial isn't reused while its PnP
    // removal may still be in flight.
    uint32_t serialScanStart_ = 0;

    // Helpers below assume the caller holds mtx_.
    // Backend serial to its owning connection + ACTIVE controller; false when
    // nothing holds it (a stray event from a just-unplugged controller's
    // worker). Every backend-sourced callback funnels through this.
    bool findBySerialLocked(uint32_t serial, Connection*& outConn, Controller*& outCtrl);
    // Controller audio. Both halves run past every gate handleMicAudio /
    // handleSpeakerAudioFromBackend applies, so they only ever see frames that
    // belong to a bound, capable slot.
    // Inbound: reorder window, then decode (FEC on a gap that has a carrier,
    // concealment otherwise), then the pad's mic endpoint. False means the
    // window refused the packet -- late, duplicate or malformed.
    bool deliverMicAudioLocked(Controller& ctrl, uint16_t seq, const uint8_t* opus, size_t opusLen);
    // Outbound: re-window whatever the backend handed over into whole 20 ms
    // frames, encode each, send it. False means the frame could not be taken at
    // all; a partial batch that is merely still short of a frame is a true.
    bool encodeAndSendSpeakerAudioLocked(Connection& conn, Controller& ctrl,
                                         const int16_t* stereo48k, size_t frames);
    // One encoded 20 ms window onto the wire, advancing the controller's seq.
    void sendSpeakerFrameLocked(Connection& conn, Controller& ctrl, ControllerAudio& audio,
                                const int16_t* frame);
    // The pad's audio working set, allocated on first use (see ControllerAudio).
    ControllerAudio& ensureControllerAudioLocked(Controller& ctrl);
    Connection* findByDeviceId(const std::string& deviceId);
    Connection* findByConnectionId(const std::string& connectionId);
    const Connection* findByConnectionId(const std::string& connectionId) const;
    void applyDescriptorLocked(Connection& conn, const ControllerDescriptor& desc,
                               ControllerApplyResult& out);
    void removeControllerLocked(Connection& conn, Controller& ctrl);
    void resetControllerStreamState(Controller& ctrl);
    bool unplugAndRelease(uint32_t serial);
    uint16_t activeBitmapLocked(const Connection& conn) const;
    void teardownConnection(Connection& conn);
    uint32_t allocateSerial();
    void releaseSerial(uint32_t serial);
    void quarantineSerial(uint32_t serial);
    int countGlobalActiveControllers() const;
    void closeBackendBusIfIdle();
    uint32_t generateUniqueToken();
    void deriveSessionKeyLocked(Connection& conn, const uint8_t pairingKey[CRYPTO_KEY_SIZE]);
};
