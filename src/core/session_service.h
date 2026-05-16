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
    // stored on the Controller so the DSU server / web UI know whether to
    // expect an IMU stream.
    void handleControllerAdd(uint32_t token, uint8_t ctrlIdx, uint16_t caps = 0);

    // Handle controller remove request.
    void handleControllerRemove(uint32_t token, uint8_t ctrlIdx);

    // Handle controller type change request.
    void handleControllerType(uint32_t token, uint8_t ctrlIdx, uint8_t controllerType);

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
            // gamepad's IMU surface (vs. DSU-emulator-only — Xbox device, old
            // ViGEmBus, or macOS).
            bool motionCapable;
            bool motionActive;
            bool motionSink;
            // True once at least one MSG_TOUCHPAD sample has been decoded for
            // this controller. Where that sample is routed is the
            // connection-level touchpadMode above.
            bool touchpadActive;
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

    // Per-slot motion snapshot for the DSU server. Returns at most
    // `dsu::MAX_SLOTS` (4) entries — the first N active controllers in
    // (token, ctrlIdx) iteration order. Each entry includes whether the
    // controller currently has a cached MotionReport (false during the
    // window between ControllerAdd and the first MotionData packet).
    struct MotionSlot {
        bool occupied = false;
        MotionReport motion{};
        bool hasMotion = false;
    };
    std::array<MotionSlot, 4> getMotionSlotsForDsu() const;

    // Check if a deviceId is already connected.
    bool isDeviceConnected(const std::string& deviceId) const;

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
