/*
 * core/types.h — Pure domain types. No Windows, no Winsock, no ViGEm headers.
 */
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <array>
#include <vector>
#include <chrono>

// ── Constants ───────────────────────────────────────────────────────────────
inline const char* APP_NAME = "satellite";
inline const char* APP_TITLE = "Satellite";

inline const int DEFAULT_UDP_PORT = 9876;
inline const int DEFAULT_WEB_PORT = 9877;
inline const int DEFAULT_PAIR_PORT = 9878;
inline const int DEFAULT_DISC_PORT = 9879;

// ── Protocol constants ──────────────────────────────────────────────────────
inline const uint16_t MSG_GAMEPAD_DATA = 0x0001;
inline const uint16_t MSG_HEARTBEAT_PING = 0x0002;
inline const uint16_t MSG_HEARTBEAT_ACK = 0x0003;
inline const uint16_t MSG_CONTROLLER_ADD = 0x0004;
inline const uint16_t MSG_CONTROLLER_REMOVE = 0x0005;
inline const uint16_t MSG_CONTROLLER_ACK = 0x0006;
inline const uint16_t MSG_SERVER_STATUS = 0x0007;
inline const uint16_t MSG_CONTROLLER_TYPE = 0x0008;

// Controller ACK result codes
inline const uint8_t ACK_OK = 0x00;
inline const uint8_t ACK_ERR_VIGEM_UNAVAIL = 0x01;
inline const uint8_t ACK_ERR_NO_SLOTS = 0x02;
inline const uint8_t ACK_ERR_ALREADY_EXISTS = 0x03;
inline const uint8_t ACK_ERR_NOT_FOUND = 0x04;
inline const uint8_t ACK_ERR_PLUGIN_FAIL = 0x05;

// Wire format sizes
inline const int HEADER_SIZE = 8;       // token(4) + counter(4)
inline const int INNER_HEADER_SIZE = 4; // type(2) + length(2)
inline const int AUTH_TAG_SIZE = 16;    // Poly1305
inline const int CRYPTO_KEY_SIZE = 32;
inline const int CRYPTO_NONCE_SIZE = 12;

// Timeouts & limits
inline const int HEARTBEAT_INTERVAL_SEC = 2;
inline const int HEARTBEAT_MISS_MAX = 3;
inline const int MAX_CONTROLLERS_PER_CONN = 16;
inline const int MAX_VIGEM_CONTROLLERS = 16;

// ── Controller types ─────────────────────────────────────────────────────────
inline const uint8_t CONTROLLER_TYPE_XBOX = 0;
inline const uint8_t CONTROLLER_TYPE_PLAYSTATION = 1;
inline const uint8_t CONTROLLER_TYPE_COUNT = 2;

inline const char* controllerTypeName(uint8_t type) {
    switch (type) {
    case CONTROLLER_TYPE_XBOX:
        return "xbox";
    case CONTROLLER_TYPE_PLAYSTATION:
        return "playstation";
    default:
        return "xbox";
    }
}

inline const char* controllerTypeLabel(uint8_t type) {
    switch (type) {
    case CONTROLLER_TYPE_XBOX:
        return "Xbox";
    case CONTROLLER_TYPE_PLAYSTATION:
        return "PlayStation";
    default:
        return "Xbox";
    }
}

// Returns true if this controller type should use a DualShock 4 virtual device.
inline bool controllerTypeUsesDS4(uint8_t type) { return type == CONTROLLER_TYPE_PLAYSTATION; }

// ── Gamepad report (binary-compatible with XUSB_REPORT / XINPUT_GAMEPAD) ───
struct GamepadReport {
    uint16_t wButtons = 0;
    uint8_t bLeftTrigger = 0;
    uint8_t bRightTrigger = 0;
    int16_t sThumbLX = 0;
    int16_t sThumbLY = 0;
    int16_t sThumbRX = 0;
    int16_t sThumbRY = 0;
};
static_assert(sizeof(GamepadReport) == 12, "GamepadReport must be 12 bytes");

// ── Controller (per virtual gamepad) ────────────────────────────────────────
struct Controller {
    uint8_t index = 0;     // 0-based index within the connection
    uint32_t serialNo = 0; // ViGEm serial (1–16), 0 = not plugged
    bool active = false;
    uint8_t controllerType = CONTROLLER_TYPE_XBOX; // visual type (cosmetic)
    GamepadReport lastReport{};
};

// ── Connection (per paired client session) ──────────────────────────────────
struct Connection {
    uint32_t token = 0;
    std::string deviceId;
    std::string deviceName;
    std::string clientIP;
    uint8_t sharedKey[CRYPTO_KEY_SIZE] = {};
    uint32_t lastCounter = 0; // replay protection
    std::chrono::steady_clock::time_point lastPacketTime;
    std::chrono::steady_clock::time_point connectedAt;
    std::array<Controller, MAX_CONTROLLERS_PER_CONN> controllers;
    int activeControllerCount = 0;
};

// ── Paired device info (persisted) ──────────────────────────────────────────
struct PairedDevice {
    std::string id;
    std::string name;
    std::string lastIP;
    std::string pairedAt;
    std::string sharedKeyHex; // 64-char hex (32 bytes)
};

// ── Config ──────────────────────────────────────────────────────────────────
struct Config {
    int udpPort = DEFAULT_UDP_PORT;
    int webPort = DEFAULT_WEB_PORT;
    int pairPort = DEFAULT_PAIR_PORT;
    int discPort = DEFAULT_DISC_PORT;
    bool autoStart = false;
    std::string credentials; // DPAPI-encrypted "user:salt:hash"
    std::vector<PairedDevice> pairedDevices;
};

// ── Logging ─────────────────────────────────────────────────────────────────
enum class LogLevel { INFO, WARN, ERR };
inline const int LOG_RING_SIZE = 500;

struct LogEntry {
    std::chrono::system_clock::time_point timestamp;
    LogLevel level;
    std::string source;
    std::string message;
};

// ── Result types ────────────────────────────────────────────────────────────
struct OpenSessionResult {
    bool ok = false;
    uint32_t token = 0;
    int availableSlots = 0;
    std::string error;
};

struct AddControllerResult {
    uint8_t resultCode = ACK_OK;
};
