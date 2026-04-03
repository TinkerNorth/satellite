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
    SessionService(IViGemPort& vigem, IClientPort& client, ILogPort& log);

    // ── Connection lifecycle ────────────────────────────────────────────

    // Open a new connection for a paired device.
    // Tears down any stale connection for the same deviceId first.
    OpenSessionResult openSession(const std::string& deviceId, const std::string& deviceName,
                                  const std::string& clientIP,
                                  const uint8_t sharedKey[CRYPTO_KEY_SIZE]);

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

    // Handle controller add request.
    void handleControllerAdd(uint32_t token, uint8_t ctrlIdx);

    // Handle controller remove request.
    void handleControllerRemove(uint32_t token, uint8_t ctrlIdx);

    // Handle controller type change request.
    void handleControllerType(uint32_t token, uint8_t ctrlIdx, uint8_t controllerType);

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
        struct CtrlInfo {
            uint8_t index;
            uint32_t serial;
            bool active;
            uint8_t controllerType;
        };
        std::vector<CtrlInfo> controllers;
    };
    struct ConnectionsSnapshot {
        std::vector<ConnectionSnapshot> connections;
        int totalControllers;
        int maxControllers;
        bool vigemAvailable;
    };
    ConnectionsSnapshot getConnectionsSnapshot() const;

    // Check if a deviceId is already connected.
    bool isDeviceConnected(const std::string& deviceId) const;

    // ── Reaper ──────────────────────────────────────────────────────────

    // Remove timed-out connections.  Returns number of connections reaped.
    int reapTimedOut();

    // ── Stats ───────────────────────────────────────────────────────────
    bool isViGEmAvailable() const;
    int totalActiveControllers() const;
    int availableSlots() const;

  private:
    IViGemPort& vigem_;
    IClientPort& client_;
    ILogPort& log_;

    mutable std::mutex mtx_; // protects connections_ and serialInUse_
    std::unordered_map<uint32_t, Connection> connections_;
    bool serialInUse_[MAX_VIGEM_CONTROLLERS] = {};

    // ── Internal helpers (caller must hold mtx_) ────────────────────────
    void teardownConnection(Connection& conn);
    uint32_t allocateSerial();
    void releaseSerial(uint32_t serial);
    int countGlobalActiveControllers() const;
    void closeVigemBusIfIdle();
    void broadcastStatus();
    uint32_t generateUniqueToken();
};
