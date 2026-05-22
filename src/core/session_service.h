// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * core/session_service.h — Domain service: connections + controllers + serial pool.
 *
 * This is the ONLY place that mutates connection/controller state.
 * All adapters (UDP, HTTP, TCP) call into this service.
 * All platform concerns are behind port interfaces.
 */
#pragma once

#include "ports.h"
#include <mutex>
#include <unordered_map>
#include <functional>

class SessionService {
  public:
    SessionService(IGamepadPort& backend, IClientPort& client, ILogPort& log);

    // ── Connection lifecycle ────────────────────────────────────────────

    // Open a new connection for a paired device.
    // Tears down any stale connection for the same deviceId first.
    // `touchpadMode` (TOUCHPAD_MODE_*) seeds the connection's touchpad routing
    // from the paired device's persisted setting; it can be changed later
    // without re-pairing via setTouchpadMode.
    OpenSessionResult openSession(const std::string& deviceId, const std::string& deviceName,
                                  const std::string& clientIP,
                                  const uint8_t sharedKey[CRYPTO_KEY_SIZE],
                                  uint8_t touchpadMode = TOUCHPAD_MODE_DS4);

    // Close (disconnect) a connection by token.  Returns controllers removed.
    int closeSession(uint32_t token);

    // Close all connections.  Used on receiver stop / app shutdown.
    void closeAllSessions();

    // ── Packet handling (called by UDP adapter) ─────────────────────────

    // Handle a decrypted gamepad data packet.
    // Returns true if the report was submitted successfully.
    bool handleGamepadData(uint32_t token, uint8_t ctrlIdx, const GamepadReport& report);

    // Handle a heartbeat ping — sends ACK + server status.
    void handleHeartbeat(uint32_t token);

    // Handle controller add request. `caps` is the CAP_* capability word from
    // the MSG_CONTROLLER_ADD payload (0 when the dish is pre-cap-aware); it is
    // stored on the Controller so the web UI knows whether to expect an IMU
    // stream.
    void handleControllerAdd(uint32_t token, uint8_t ctrlIdx, uint16_t caps = 0);

    // Handle controller remove request.
    void handleControllerRemove(uint32_t token, uint8_t ctrlIdx);

    // Handle controller type change request.
    void handleControllerType(uint32_t token, uint8_t ctrlIdx, uint8_t controllerType);

    // Handle a mid-session capability update from the dish
    // (MSG_CONTROLLER_CAPS_UPDATE / 0x000E). Overwrites
    // `Controller::caps` in place; no replug, no fresh ACK. The receiver
    // does not gate any data path on the cap bits today
    // (`session_service.cpp::handleMotionData` is explicit about it), so
    // this is purely an honesty update for the
    // ConnectionSnapshot::CtrlInfo::motionCapable / lightbarCapable
    // fields the web UI surfaces — the dish-side listener gate is the
    // load-bearing path for "are bytes actually flowing right now."
    //
    // Silently dropped if the connection / controller doesn't exist or
    // is inactive, matching every other "handle*" path's policy of
    // not surfacing per-packet errors back to the dish (the wire is
    // not reliable, and a malicious dish couldn't be told anything
    // useful anyway).
    void handleControllerCapsUpdate(uint32_t token, uint8_t ctrlIdx, uint16_t caps);

    // Handle a rumble notification fired by the platform gamepad backend.
    // Called from the backend's notification thread (ViGEm worker on Windows,
    // uinput reader on Linux). Resolves `serial` → (connection, ctrlIdx),
    // coalesces against the controller's last forwarded rumble state, and
    // invokes IClientPort::sendRumble for unique updates only.
    //
    // The `wireDurationMs` knob lets the SessionService stamp a duration onto
    // every outgoing packet — without it the dish would have to invent one,
    // and Linux uinput "play" events have no native duration. The default
    // (500 ms) matches the rumble heartbeat the dish-side uses to refresh
    // its actuator while the backend keeps reporting non-zero magnitudes.
    void handleRumbleFromBackend(uint32_t serial, const RumbleReport& report,
                                 uint16_t wireDurationMs = 500);

    // Handle an IMU sample from the dish (MSG_MOTION). Forwards to the
    // backend's motion channel via IGamepadPort::submitMotion when the
    // controller is active; caches as Controller.lastMotion for the web UI
    // either way. Returns true if the backend accepted the sample (which
    // for Xbox 360 / uinput backends is always false today since they
    // have no motion surface). False does NOT indicate an error — senders
    // should keep streaming motion regardless.
    bool handleMotionData(uint32_t token, uint8_t ctrlIdx, const MotionReport& report);

    // Handle a battery update from the dish (MSG_BATTERY). Caches the
    // most recent value on the Controller so the web UI can surface it,
    // and forwards to the backend via IGamepadPort::submitBattery for
    // platforms that expose a battery channel (Windows DS4 today).
    // Returns true if the cache was updated (i.e. the (token, ctrlIdx)
    // resolved to an active controller).
    bool handleBatteryUpdate(uint32_t token, uint8_t ctrlIdx, const BatteryReport& report);

    // Handle a touchpad sample from the dish (MSG_TOUCHPAD). Always caches on
    // the Controller for the web UI, then routes per the owning connection's
    // touchpadMode:
    //   DS4   — forwards to IGamepadPort::submitTouchpad (virtual DS4 pad).
    //   MOUSE — converts the finger-0 delta into a relative-mouse movement and
    //           forwards to IGamepadPort::submitRelativeMouse.
    //   OFF   — caches only; nothing is forwarded.
    // Returns whether the routed sink accepted the sample. False does NOT
    // indicate an error — senders keep streaming touchpad regardless.
    bool handleTouchpadData(uint32_t token, uint8_t ctrlIdx, const TouchpadReport& report);

    // Hot-apply a touchpad routing mode (TOUCHPAD_MODE_*) to every live
    // connection for `deviceId` — the web UI's per-device toggle, applied
    // without re-pairing or reconnecting. The persisted PairedDevice remains
    // the source of truth across restarts; this only updates in-memory
    // Connection state. Returns true if at least one live connection matched.
    bool setTouchpadMode(const std::string& deviceId, uint8_t mode);

    // Handle a lightbar change fired by the platform gamepad backend's
    // dedicated lightbar callback (independent of rumble). Resolves
    // `serial` → (connection, ctrlIdx), coalesces against the controller's
    // last forwarded colour, and invokes IClientPort::sendLightbar only on
    // unique updates. Called from the backend's notification thread.
    void handleLightbarFromBackend(uint32_t serial, uint8_t r, uint8_t g, uint8_t b);

    // ── Pre-decrypt helpers (called under lock briefly) ─────────────────

    // Look up a connection's key and last counter for decryption.
    // Returns false if token not found.
    bool getDecryptInfo(uint32_t token, uint8_t outKey[CRYPTO_KEY_SIZE],
                        uint32_t& outLastCounter) const;

    // Update connection state after successful decrypt (counter, timestamp, addr).
    void updatePostDecrypt(uint32_t token, uint32_t counter, const std::string& clientIP,
                           uint16_t clientPort);

    // ── Query ───────────────────────────────────────────────────────────

    // Build a snapshot of all connections for JSON serialization (thread-safe).
    struct ConnectionSnapshot {
        uint32_t token;
        std::string deviceId;
        std::string deviceName;
        std::string clientIP;
        int64_t connectedAtEpoch;
        int activeControllerCount;
        // Touchpad routing for this connection (TOUCHPAD_MODE_*). The web UI
        // renders it as the per-device selector's current value.
        uint8_t touchpadMode;
        // Per-connection link state (DeviceLinkState). Always Active or
        // NotResponding here — a connection that exists at all is at least
        // Active; the stalling threshold escalates it to NotResponding. The
        // /api/devices endpoint surfaces the full enum (Paired vs Active vs
        // NotResponding vs Linking).
        DeviceLinkState linkState;
        struct CtrlInfo {
            uint8_t index;
            uint32_t serial;
            bool active;
            uint8_t controllerType;
            // Battery snapshot (most recent MSG_BATTERY received). When the
            // controller has not reported battery yet, `batteryKnown` is false
            // and the level/status fields are unspecified.
            bool batteryKnown;
            uint8_t batteryLevel;  // 0..100, or BATTERY_LEVEL_UNKNOWN
            uint8_t batteryStatus; // BATTERY_STATUS_*
            // Motion (IMU). `motionCapable` is the CAP_MOTION bit the dish
            // advertised at controller-add; `motionActive` is true once at
            // least one MSG_MOTION packet has actually been decoded for this
            // controller (the two differ during the add → first-sample window
            // and for a dish that streams motion without advertising the cap).
            // `motionSink` is true when motion is also reaching the virtual
            // gamepad's IMU surface; false means it is cached only (Xbox
            // device, ViGEmBus too old for the extended report, or macOS).
            bool motionCapable;
            bool motionActive;
            bool motionSink;
            // True iff this satellite's backend has an IMU surface for this
            // controller's chosen type (DS4 yes, Xbox no, macOS no). The
            // web UI uses this to warn the operator that an Xbox-typed
            // slot can never sink motion regardless of whether the dish
            // advertises it.
            bool motionSinkSupportedForType;
            // True iff the platform adapter successfully created the IMU
            // sink for this serial at plug-in time. False distinguishes
            // "kernel rejected the motion node" (a real failure to
            // diagnose) from "no game has subscribed yet" (the common
            // motionSink == false case). True on Windows/macOS today
            // since neither can fail per-serial in the same way; the
            // signal is meaningful on Linux.
            bool motionBackendOk;
            // True once at least one MSG_TOUCHPAD sample has been decoded for
            // this controller. Where that sample is routed is the
            // connection-level touchpadMode above.
            bool touchpadActive;
            // Lightbar (DS4 / DualSense LED). `lightbarCapable` is the
            // CAP_LIGHTBAR bit the dish advertised at controller-add — and the
            // gate the receiver uses to decide whether to emit MSG_LIGHTBAR.
            // `lightbarKnown` is true once the host game has set a colour;
            // r/g/b carry the most recent colour (0,0,0 until known).
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

    // Check if a deviceId is already connected.
    bool isDeviceConnected(const std::string& deviceId) const;

    // Compute the per-paired-device link state (server's view). Returns
    // DeviceLinkState::Paired when no live connection exists for `deviceId`;
    // DeviceLinkState::NotResponding when the live connection's last packet is
    // older than the stalling threshold (but not yet reaped); otherwise
    // DeviceLinkState::Active.
    //
    // DeviceLinkState::Linking is brief and currently not derivable on the
    // server side — the POST /api/connections handshake is synchronous, so
    // the connection is either absent (Paired) or already present (Active).
    // TODO: thread an in-flight "Linking" state through openSession if we
    // start surfacing per-request progress; for now Linking is enumerated
    // but never returned here.
    DeviceLinkState linkStateForDevice(const std::string& deviceId) const;

    // ── Reaper ──────────────────────────────────────────────────────────

    // Remove timed-out connections.  Returns number of connections reaped.
    int reapTimedOut();

    // ── Stats ───────────────────────────────────────────────────────────
    bool isBackendAvailable() const;
    int totalActiveControllers() const;
    int availableSlots() const;

  private:
    IGamepadPort& backend_;
    IClientPort& client_;
    ILogPort& log_;

    mutable std::mutex mtx_; // protects connections_ and serialInUse_
    std::unordered_map<uint32_t, Connection> connections_;
    bool serialInUse_[MAX_BACKEND_CONTROLLERS] = {};

    // ── Internal helpers (caller must hold mtx_) ────────────────────────
    void teardownConnection(Connection& conn);
    uint32_t allocateSerial();
    void releaseSerial(uint32_t serial);
    int countGlobalActiveControllers() const;
    void closeBackendBusIfIdle();
    void broadcastStatus();
    uint32_t generateUniqueToken();
};
