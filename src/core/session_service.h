// SPDX-License-Identifier: LGPL-3.0-or-later

// The ONLY place that mutates connection/controller state. All adapters (UDP,
// HTTP, TCP) call in; all platform concerns are behind the port interfaces.
#pragma once

#include "ports.h"
#include <mutex>
#include <unordered_map>
#include <functional>

class SessionService {
  public:
    SessionService(IGamepadPort& backend, IClientPort& client, ILogPort& log);

    // Open a connection for a paired device, tearing down any stale connection
    // for the same deviceId first. `touchpadMode` seeds touchpad routing from
    // the persisted setting; changeable later via setTouchpadMode.
    OpenSessionResult openSession(const std::string& deviceId, const std::string& deviceName,
                                  const std::string& clientIP,
                                  const uint8_t sharedKey[CRYPTO_KEY_SIZE],
                                  uint8_t touchpadMode = TOUCHPAD_MODE_OFF);

    // Disconnect a connection by token. Returns controllers removed.
    int closeSession(uint32_t token);

    // Close all connections (receiver stop / app shutdown).
    void closeAllSessions();

    // Packet handling (called by UDP adapter).

    // True if the report was submitted successfully.
    bool handleGamepadData(uint32_t token, uint8_t ctrlIdx, const GamepadReport& report);

    // Sends heartbeat ACK + server status.
    void handleHeartbeat(uint32_t token);

    // `caps` is the CAP_* word from the payload (0 when the dish is
    // pre-cap-aware), stored so the web UI knows which streams to expect.
    // `controllerType` lets a single MSG_CONTROLLER_ADD plug the correct virtual
    // device on the first try. CONTROLLER_TYPE_UNSPECIFIED (a pre-extension dish
    // that omits the type byte) retains the slot's existing type instead, then
    // a follow-up MSG_CONTROLLER_TYPE corrects it.
    void handleControllerAdd(uint32_t token, uint8_t ctrlIdx, uint16_t caps = 0,
                             uint8_t controllerType = CONTROLLER_TYPE_UNSPECIFIED);

    void handleControllerRemove(uint32_t token, uint8_t ctrlIdx);
    void handleControllerType(uint32_t token, uint8_t ctrlIdx, uint8_t controllerType);

    // Mid-session cap update (MSG_CONTROLLER_CAPS_UPDATE / 0x000E). Overwrites
    // Controller::caps in place; no replug, no fresh ACK. No data path gates on
    // the cap bits (see handleMotionData), so this is purely an honesty update
    // for the web UI's motionCapable/lightbarCapable fields — the dish-side
    // listener gate is the load-bearing path. Silently dropped for a missing /
    // inactive controller, like every other handle* path.
    void handleControllerCapsUpdate(uint32_t token, uint8_t ctrlIdx, uint16_t caps);

    // Rumble fired by the backend's notification thread. Resolves serial →
    // controller, coalesces against last forwarded state, sends only unique
    // updates. `wireDurationMs` stamps a duration the dish can't otherwise
    // invent (Linux uinput "play" events have none); the 500 ms default matches
    // the dish-side rumble heartbeat that refreshes the actuator.
    void handleRumbleFromBackend(uint32_t serial, const RumbleReport& report,
                                 uint16_t wireDurationMs = 500);

    // IMU sample (MSG_MOTION). Caches Controller.lastMotion for the web UI and
    // forwards via submitMotion when active. Returns whether the backend
    // accepted it (always false today for Xbox 360 / uinput — no motion
    // surface). False is NOT an error; senders keep streaming.
    bool handleMotionData(uint32_t token, uint8_t ctrlIdx, const MotionReport& report);

    // Battery update (MSG_BATTERY). Caches the value and forwards via
    // submitBattery on platforms with a battery channel (Windows DS4). Returns
    // true if (token, ctrlIdx) resolved to an active controller.
    bool handleBatteryUpdate(uint32_t token, uint8_t ctrlIdx, const BatteryReport& report);

    // Touchpad sample (MSG_TOUCHPAD). Always caches, then routes per the
    // connection's touchpadMode: DS4 → submitTouchpad, MOUSE → finger-0 delta →
    // submitRelativeMouse, OFF → cache only. Returns whether the routed sink
    // accepted it. False is NOT an error; senders keep streaming.
    bool handleTouchpadData(uint32_t token, uint8_t ctrlIdx, const TouchpadReport& report);

    // Hot-apply a touchpad routing mode to every live connection for `deviceId`
    // without re-pairing. The persisted PairedDevice remains the source of truth
    // across restarts; this only updates in-memory state. True if any matched.
    bool setTouchpadMode(const std::string& deviceId, uint8_t mode);

    // Lightbar change from the backend's dedicated callback (independent of
    // rumble). Resolves serial → controller, coalesces, sends only unique
    // updates. Called from the backend's notification thread.
    void handleLightbarFromBackend(uint32_t serial, uint8_t r, uint8_t g, uint8_t b);

    // Pre-decrypt helpers (called under lock briefly).

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

    // Query.

    // Thread-safe snapshot of all connections for JSON serialization.
    struct ConnectionSnapshot {
        uint32_t token;
        std::string deviceId;
        std::string deviceName;
        std::string clientIP;
        int64_t connectedAtEpoch;
        int activeControllerCount;
        uint8_t touchpadMode; // TOUCHPAD_MODE_*
        // Always Active or NotResponding here (an existing connection is at
        // least Active). The full enum is surfaced by /api/devices.
        DeviceLinkState linkState;
        struct CtrlInfo {
            uint8_t index;
            uint32_t serial;
            bool active;
            uint8_t controllerType;
            // Most recent MSG_BATTERY. level/status unspecified until batteryKnown.
            bool batteryKnown;
            uint8_t batteryLevel;  // 0..100, or BATTERY_LEVEL_UNKNOWN
            uint8_t batteryStatus; // BATTERY_STATUS_*
            // motionCapable = the CAP_MOTION bit the dish advertised.
            // motionActive = at least one MSG_MOTION decoded. motionSink = motion
            // is also reaching the virtual device's IMU surface (false = cached
            // only: Xbox, old ViGEmBus, or macOS).
            bool motionCapable;
            bool motionActive;
            bool motionSink;
            // Backend has an IMU surface for the chosen type (DS4 yes, Xbox/macOS
            // no). Lets the UI warn that an Xbox-typed slot can never sink motion.
            bool motionSinkSupportedForType;
            // Per-serial IMU sink created at plug-in. False distinguishes "kernel
            // rejected the motion node" from "no game subscribed yet"
            // (motionSink == false). Meaningful on Linux; true elsewhere.
            bool motionBackendOk;
            // At least one MSG_TOUCHPAD decoded; routing is the connection-level
            // touchpadMode above.
            bool touchpadActive;
            // lightbarCapable = the CAP_LIGHTBAR bit and the receiver's gate for
            // emitting MSG_LIGHTBAR. lightbarKnown once a colour is set; r/g/b
            // hold the latest (0,0,0 until known).
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
    // returned — the POST /api/connections handshake is synchronous, so the
    // connection is either absent (Paired) or already present (Active).
    DeviceLinkState linkStateForDevice(const std::string& deviceId) const;

    // Remove timed-out connections. Returns number reaped.
    int reapTimedOut();

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

    // Helpers below assume the caller holds mtx_.
    void teardownConnection(Connection& conn);
    uint32_t allocateSerial();
    void releaseSerial(uint32_t serial);
    int countGlobalActiveControllers() const;
    void closeBackendBusIfIdle();
    void broadcastStatus();
    uint32_t generateUniqueToken();
};
