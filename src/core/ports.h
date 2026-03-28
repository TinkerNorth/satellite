/*
 * core/ports.h — Outbound port interfaces (Hexagonal Architecture).
 *
 * The SessionService depends ONLY on these interfaces.
 * Concrete adapters implement them, injected via constructor.
 */
#pragma once

#include "types.h"

// ── Outbound: ViGEm driver operations ───────────────────────────────────────
class IViGemPort {
public:
    virtual ~IViGemPort() = default;

    // Open the bus (lazy). Returns true if bus is usable.
    virtual bool ensureBusOpen() = 0;

    // Close the bus (called when no controllers remain).
    virtual void closeBus() = 0;

    // Is the bus currently open?
    virtual bool isBusOpen() const = 0;

    // Plug in a virtual Xbox controller. Returns true on success.
    virtual bool pluginDevice(uint32_t serial) = 0;

    // Unplug a virtual Xbox controller.
    virtual void unplugDevice(uint32_t serial) = 0;

    // Submit a gamepad report. Returns true on success.
    virtual bool submitReport(uint32_t serial, const GamepadReport& report) = 0;

    // Check if the ViGEm driver is installed on this system.
    virtual bool isDriverInstalled() = 0;
};

// ── Outbound: Send encrypted UDP packets to clients ─────────────────────────
class IClientPort {
public:
    virtual ~IClientPort() = default;

    // Update the network address for a token (called on every packet recv).
    virtual void updateClientAddr(uint32_t token,
                                  const std::string& ip, uint16_t port) = 0;

    // Remove address mapping for a token.
    virtual void removeClientAddr(uint32_t token) = 0;

    // Send heartbeat ACK (0x0003) to a specific client.
    virtual void sendHeartbeatAck(const Connection& conn) = 0;

    // Send controller ACK (0x0006) to a specific client.
    virtual void sendControllerAck(const Connection& conn,
                                   uint16_t requestType,
                                   uint8_t ctrlIdx, uint8_t result) = 0;

    // Send server status (0x0007) to a specific client.
    virtual void sendServerStatus(const Connection& conn,
                                  bool vigemAvailable,
                                  uint8_t totalActiveControllers) = 0;

    // Broadcast server status to all provided connections.
    virtual void broadcastServerStatus(
        const std::vector<std::pair<uint32_t, const Connection*>>& connections,
        bool vigemAvailable, uint8_t totalActiveControllers) = 0;
};

// ── Outbound: Configuration persistence ─────────────────────────────────────
class IConfigPort {
public:
    virtual ~IConfigPort() = default;

    virtual Config loadConfig() = 0;
    virtual void   saveConfig(const Config& cfg) = 0;
    virtual void   setAutoStart(bool enable) = 0;
    virtual bool   getAutoStart() = 0;
};

// ── Outbound: Logging ───────────────────────────────────────────────────────
class ILogPort {
public:
    virtual ~ILogPort() = default;

    virtual void logMsg(LogLevel level, const std::string& source,
                        const std::string& message) = 0;
};

