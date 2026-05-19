// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * core/types.h — Pure domain types. No Windows, no Winsock, no platform-
 * specific virtual-gamepad headers.
 */
#pragma once

#include "core/version.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <array>
#include <vector>
#include <chrono>

// ── Constants ───────────────────────────────────────────────────────────────
inline const char* APP_NAME = "satellite";
inline const char* APP_TITLE = "Satellite";

// Build-time version, threaded into the binary from src/core/version.h
// and mirrored in /VERSION (which CMake + installer.iss read). Surfaced
// at runtime via /api/version and the update checker.
inline const char* SATELLITE_VERSION = SATELLITE_VERSION_STRING;

inline const int DEFAULT_UDP_PORT = 9876;
inline const int DEFAULT_WEB_PORT = 9877;
inline const int DEFAULT_PAIR_PORT = 9878;
inline const int DEFAULT_DISC_PORT = 9879;
// Sender-facing API (pairing + connections), served over HTTPS on 0.0.0.0.
// The admin web UI on DEFAULT_WEB_PORT is bound to 127.0.0.1; this is the
// only TCP port a sender on the LAN connects to.
inline const int DEFAULT_CLIENT_PORT = 9443;

// ── Protocol constants ──────────────────────────────────────────────────────
inline const uint16_t MSG_GAMEPAD_DATA = 0x0001;
inline const uint16_t MSG_HEARTBEAT_PING = 0x0002;
inline const uint16_t MSG_HEARTBEAT_ACK = 0x0003;
inline const uint16_t MSG_CONTROLLER_ADD = 0x0004;
inline const uint16_t MSG_CONTROLLER_REMOVE = 0x0005;
inline const uint16_t MSG_CONTROLLER_ACK = 0x0006;
inline const uint16_t MSG_SERVER_STATUS = 0x0007;
inline const uint16_t MSG_CONTROLLER_TYPE = 0x0008;
inline const uint16_t MSG_RUMBLE = 0x0009;
inline const uint16_t MSG_MOTION = 0x000A;
inline const uint16_t MSG_BATTERY = 0x000B;
inline const uint16_t MSG_TOUCHPAD = 0x000C;
inline const uint16_t MSG_LIGHTBAR = 0x000D;

// Controller ACK result codes (wire values are stable across platforms; only
// the C++ identifier changed from ACK_ERR_VIGEM_UNAVAIL → ACK_ERR_BACKEND_UNAVAIL).
inline const uint8_t ACK_OK = 0x00;
inline const uint8_t ACK_ERR_BACKEND_UNAVAIL = 0x01;
inline const uint8_t ACK_ERR_NO_SLOTS = 0x02;
inline const uint8_t ACK_ERR_ALREADY_EXISTS = 0x03;
inline const uint8_t ACK_ERR_NOT_FOUND = 0x04;
inline const uint8_t ACK_ERR_PLUGIN_FAIL = 0x05;

// ── Controller capability bits ──────────────────────────────────────────────
// Carried in the 2-byte big-endian capability word of the MSG_CONTROLLER_ADD
// (0x0004) payload. The dish advertises these at controller-add time so the
// receiver knows which optional streams to expect from that client. Unknown /
// unset bits are forward-compatible: a pre-cap-aware dish sends 0 and the
// receiver treats every capability as "unknown / best-effort".
inline const uint16_t CAP_ANALOG_TRIGGERS = 0x0001; // analog L/R triggers
inline const uint16_t CAP_RUMBLE = 0x0002;          // accepts the MSG_RUMBLE return path
inline const uint16_t CAP_MOTION = 0x0004;          // streams MSG_MOTION (0x000A) IMU data
inline const uint16_t CAP_LIGHTBAR = 0x0008;        // accepts the MSG_LIGHTBAR (0x000D) return path

// Wire format sizes
inline const int HEADER_SIZE = 8;       // token(4) + counter(4)
inline const int INNER_HEADER_SIZE = 4; // type(2) + length(2)
inline const int AUTH_TAG_SIZE = 16;    // Poly1305
inline const int CRYPTO_KEY_SIZE = 32;
inline const int CRYPTO_NONCE_SIZE = 12;

// Timeouts & limits
inline const int HEARTBEAT_INTERVAL_SEC = 2;
inline const int HEARTBEAT_MISS_MAX = 5;
inline const int MAX_CONTROLLERS_PER_CONN = 16;
inline const int MAX_BACKEND_CONTROLLERS = 16;

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

// ── Rumble report (game → physical controller, return path) ─────────────────
// Magnitudes are 16-bit unsigned to match XInput's `XINPUT_VIBRATION` scale
// (0..65535). Backends report low-frequency / high-frequency intensities; on
// Linux uinput these map to the FF_RUMBLE strong/weak magnitudes verbatim.
//
// Rumble carries motor vibration only. The controller's RGB lightbar is a
// separate return path — see MSG_LIGHTBAR (0x000D) / IClientPort::sendLightbar.
//
// `durationMs == 0` is the "until-overridden" mode: the actuator should keep
// the magnitudes applied until the next rumble packet for the same controller.
// Non-zero durations are clamped on the dish side to match what each platform's
// rumble API accepts (XInput / SDL: u32 ms; Android Vibrator: u32 ms).
struct RumbleReport {
    uint16_t strongMagnitude = 0; // low-frequency / large motor
    uint16_t weakMagnitude = 0;   // high-frequency / small motor
    uint16_t durationMs = 0;      // 0 = continuous (until next packet)
};

// ── Motion report (sender → satellite, gyro + accel from a real IMU) ────────
// Wire scale uses a fixed full-scale convention so no per-axis
// renormalisation is needed downstream:
//
//   gyro  axes: deg/s, ±2000 deg/s full scale → int16 LSB = 2000 / 32767 deg/s
//   accel axes: g,     ±4 g     full scale → int16 LSB = 4    / 32767 g
//
// Axis frame is right-handed, +X = right, +Y = up, +Z = toward the player
// (the same convention as Sony's DualSense IMU after applying the
// manufacturer rotation matrix). Senders MUST apply that matrix; receivers
// do not rotate.
//
// `timestampDeltaUs` is microseconds elapsed since the previous MOTION packet
// for the same controller on the same connection. The first packet uses 0.
// 32 bits is enough for ~71 minutes of gap, well past any realistic stall.
inline const float MOTION_GYRO_SCALE_DEG_S = 2000.0f / 32767.0f;
inline const float MOTION_ACCEL_SCALE_G = 4.0f / 32767.0f;

struct MotionReport {
    int16_t gyroX = 0;
    int16_t gyroY = 0;
    int16_t gyroZ = 0;
    int16_t accelX = 0;
    int16_t accelY = 0;
    int16_t accelZ = 0;
    uint32_t timestampDeltaUs = 0;
};
static_assert(sizeof(MotionReport) == 16, "MotionReport must be 16 bytes on the wire");

// Wire payload length of MSG_MOTION's MotionReport portion (after the 1-byte
// ctrlIdx) — 6×int16 + uint32. The receiver guards on ctrlIdx(1) + this.
inline const int MOTION_WIRE_PAYLOAD_BYTES = 16;

// Decode a MotionReport from `p` (16 wire bytes, little-endian, the portion
// after the 1-byte ctrlIdx). Explicit shifts — no struct memcpy — so decoding
// is byte-order-independent and immune to MotionReport layout/padding changes.
// Senders encode the matching little-endian layout; see docs/protocol.md.
inline MotionReport decodeMotionReport(const uint8_t* p) {
    auto le16 = [](const uint8_t* q) -> int16_t {
        return static_cast<int16_t>(static_cast<uint16_t>(q[0]) |
                                    (static_cast<uint16_t>(q[1]) << 8));
    };
    auto le32 = [](const uint8_t* q) -> uint32_t {
        return static_cast<uint32_t>(q[0]) | (static_cast<uint32_t>(q[1]) << 8) |
               (static_cast<uint32_t>(q[2]) << 16) | (static_cast<uint32_t>(q[3]) << 24);
    };
    MotionReport r;
    r.gyroX = le16(p + 0);
    r.gyroY = le16(p + 2);
    r.gyroZ = le16(p + 4);
    r.accelX = le16(p + 6);
    r.accelY = le16(p + 8);
    r.accelZ = le16(p + 10);
    r.timestampDeltaUs = le32(p + 12);
    return r;
}

// ── Battery report (sender → satellite, periodic) ───────────────────────────
// Sent once per BATTERY_REPORT_INTERVAL_SEC and again whenever the controller
// transitions between charging states. `level` is 0..100 inclusive; 0xFF means
// the sender can't read the level (some Bluetooth pads only report state).
inline const uint8_t BATTERY_LEVEL_UNKNOWN = 0xFF;
inline const uint8_t BATTERY_STATUS_UNKNOWN = 0;
inline const uint8_t BATTERY_STATUS_DISCHARGING = 1;
inline const uint8_t BATTERY_STATUS_CHARGING = 2;
inline const uint8_t BATTERY_STATUS_FULL = 3;
inline const uint8_t BATTERY_STATUS_WIRED = 4; // No battery, AC powered
inline const uint8_t BATTERY_STATUS_COUNT = 5;
inline const int BATTERY_REPORT_INTERVAL_SEC = 30;

struct BatteryReport {
    uint8_t level = BATTERY_LEVEL_UNKNOWN;
    uint8_t status = BATTERY_STATUS_UNKNOWN;
};

// ── Touchpad report (sender → satellite, DS4 / DualSense trackpad) ──────────
// Two simultaneous finger contacts max (matching the DS4 / DualSense hardware
// surface). Coordinates are normalized int16 (-32768..32767) on both axes so
// the wire is resolution-independent — the receiver maps to whichever virtual
// device's coordinate space it owns (DS4: 1920×943, mouse: monitor pixels).
//
// Each finger carries a "tracking ID" so the receiver can correlate a finger
// across frames even when finger 0 lifts and finger 1 keeps contact. IDs are
// monotonically increasing per controller; the sender wraps freely.
//
// `buttonPressed` is the dedicated clicky-trackpad button (DS4 + DualSense
// both have one). It is logically independent from any touch.
struct TouchpadFinger {
    bool active = false;
    uint8_t trackingId = 0;
    int16_t x = 0;
    int16_t y = 0;
};

struct TouchpadReport {
    TouchpadFinger finger0{};
    TouchpadFinger finger1{};
    bool buttonPressed = false;
};

// Wire layout for MSG_TOUCHPAD payload (after the 1-byte ctrlIdx):
//   flags(1): bit0=finger0_active, bit1=finger1_active, bit2=button_pressed
//   finger0_trackingId(1) + finger0_x(2 LE) + finger0_y(2 LE)
//   finger1_trackingId(1) + finger1_x(2 LE) + finger1_y(2 LE)
// = 1 + 5 + 5 = 11 bytes after ctrlIdx → 12 bytes total inner payload.
inline const int TOUCHPAD_WIRE_PAYLOAD_BYTES = 11;

// Decode a TouchpadReport from `p` (TOUCHPAD_WIRE_PAYLOAD_BYTES bytes — the
// portion after the 1-byte ctrlIdx). Explicit little-endian shifts, no struct
// memcpy, so the wire stays byte-order-independent. Mirrors decodeMotionReport;
// senders encode the matching layout (see docs/protocol.md).
inline TouchpadReport decodeTouchpadReport(const uint8_t* p) {
    auto le16 = [](const uint8_t* q) -> int16_t {
        return static_cast<int16_t>(static_cast<uint16_t>(q[0]) |
                                    (static_cast<uint16_t>(q[1]) << 8));
    };
    TouchpadReport r;
    const uint8_t flags = p[0];
    r.finger0.active = (flags & 0x01) != 0;
    r.finger1.active = (flags & 0x02) != 0;
    r.buttonPressed = (flags & 0x04) != 0;
    r.finger0.trackingId = p[1];
    r.finger0.x = le16(p + 2);
    r.finger0.y = le16(p + 4);
    r.finger1.trackingId = p[6];
    r.finger1.x = le16(p + 7);
    r.finger1.y = le16(p + 9);
    return r;
}

// ── Touchpad routing modes (per-paired-device, Task 1.3) ────────────────────
// How the receiver routes a paired device's MSG_TOUCHPAD samples. Persisted in
// PairedDevice, mirrored onto every live Connection, and hot-applied from the
// web UI without re-pairing.
//   DS4   — feed the finger coordinates + clicky button into the virtual
//           DualShock 4 touchpad surface (DS4_REPORT_EX on Windows, a
//           multitouch uinput node on Linux). Xbox-typed virtual pads have no
//           touchpad surface and drop it.
//   MOUSE — synthesize a relative-mouse pointer on the receiver host: finger 0
//           movement drives the cursor, the clicky button is mouse button 1.
//   OFF   — ignore touchpad entirely (still cached for the web UI debug pane).
inline const uint8_t TOUCHPAD_MODE_DS4 = 0;
inline const uint8_t TOUCHPAD_MODE_MOUSE = 1;
inline const uint8_t TOUCHPAD_MODE_OFF = 2;
inline const uint8_t TOUCHPAD_MODE_COUNT = 3;

// Relative-mouse sensitivity for TOUCHPAD_MODE_MOUSE: host pixels travelled per
// unit of wire-coordinate finger movement. The pad spans 65535 wire units per
// axis, so a full-width swipe travels ~2750 px — close to a laptop trackpad.
// The SessionService carries a sub-pixel remainder so slow drags don't
// truncate to zero motion.
inline const float TOUCHPAD_MOUSE_SENSITIVITY = 0.042f;

inline const char* touchpadModeName(uint8_t mode) {
    switch (mode) {
    case TOUCHPAD_MODE_MOUSE:
        return "mouse";
    case TOUCHPAD_MODE_OFF:
        return "off";
    case TOUCHPAD_MODE_DS4:
    default:
        return "ds4";
    }
}

// Parse a touchpad-mode name (web UI / config JSON). Unknown or empty → DS4, so
// a config written before Task 1.3 migrates to the historical pass-through.
inline uint8_t touchpadModeFromName(const std::string& s) {
    if (s == "mouse") return TOUCHPAD_MODE_MOUSE;
    if (s == "off") return TOUCHPAD_MODE_OFF;
    return TOUCHPAD_MODE_DS4;
}

inline const char* batteryStatusName(uint8_t status) {
    switch (status) {
    case BATTERY_STATUS_DISCHARGING:
        return "discharging";
    case BATTERY_STATUS_CHARGING:
        return "charging";
    case BATTERY_STATUS_FULL:
        return "full";
    case BATTERY_STATUS_WIRED:
        return "wired";
    default:
        return "unknown";
    }
}

// ── Controller (per virtual gamepad) ────────────────────────────────────────
struct Controller {
    uint8_t index = 0;     // 0-based index within the connection
    uint32_t serialNo = 0; // backend serial (1–16), 0 = not plugged
    bool active = false;
    uint8_t controllerType = CONTROLLER_TYPE_XBOX; // visual type (cosmetic)
    // Capability word from MSG_CONTROLLER_ADD (CAP_* bits). 0 when the dish is
    // pre-cap-aware. `motionCapable()` / `lightbarCapable()` are the convenience
    // accessors the web UI / return-path routing use.
    uint16_t caps = 0;
    bool motionCapable() const { return (caps & CAP_MOTION) != 0; }
    bool lightbarCapable() const { return (caps & CAP_LIGHTBAR) != 0; }
    GamepadReport lastReport{};
    // Most recent rumble state forwarded to the dish. Used to coalesce identical
    // back-to-back game updates so we don't blast the wire when a game holds
    // both motors at the same magnitude across many frames.
    RumbleReport lastRumble{};
    bool lastRumbleValid = false;
    // Most recent motion sample (sender → satellite). Stored so the web UI's
    // debug pane can show live IMU values without keeping a parallel cache.
    MotionReport lastMotion{};
    bool lastMotionValid = false;
    // True when the most recent motion sample was actually delivered to the
    // virtual-gamepad backend's IMU surface (ViGEm DS4 extended report /
    // Linux uinput motion node). False means motion is captured & cached
    // but NOT delivered to any IMU surface — e.g. an Xbox-typed virtual
    // device, a ViGEmBus too old for the extended report, or the inert
    // macOS backend. Drives the web UI's motion state.
    bool motionSinkActive = false;
    // Most recent battery sample (sender → satellite). Surfaced in the
    // web UI's connection list and (eventually) forwarded to the virtual
    // device's battery channel where the backend supports it.
    BatteryReport lastBattery{};
    bool lastBatteryValid = false;
    // Most recent touchpad sample (sender → satellite, DS4 / DualSense
    // trackpad). Backends with a touchpad surface (ViGEm DS4) forward
    // these into DS4_REPORT_EX touchpad fields; the rest cache for the
    // web UI debug pane.
    TouchpadReport lastTouchpad{};
    bool lastTouchpadValid = false;
    // TOUCHPAD_MODE_MOUSE sub-pixel remainder — the fraction of a pixel a slow
    // finger drag hasn't yet accumulated into a whole-pixel mouse delta. Reset
    // on a fresh contact and whenever the touchpad mode changes.
    float touchpadMouseRemX = 0.0f;
    float touchpadMouseRemY = 0.0f;
    // Most recent lightbar colour the host game set on this controller.
    // Written by handleLightbarFromBackend; forwarded to the dish via
    // MSG_LIGHTBAR (for CAP_LIGHTBAR senders) and surfaced in the web UI.
    uint8_t lightbarR = 0;
    uint8_t lightbarG = 0;
    uint8_t lightbarB = 0;
    bool lastLightbarValid = false;
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
    // Touchpad routing for this session (TOUCHPAD_MODE_*). Seeded from the
    // paired device's persisted setting at openSession; hot-swappable from the
    // web UI via SessionService::setTouchpadMode without re-pairing.
    uint8_t touchpadMode = TOUCHPAD_MODE_DS4;
};

// ── Paired device info (persisted) ──────────────────────────────────────────
struct PairedDevice {
    std::string id;
    std::string name;
    std::string lastIP;
    std::string pairedAt;
    std::string sharedKeyHex; // 64-char hex (32 bytes)
    // Touchpad routing mode (TOUCHPAD_MODE_*). Devices paired before Task 1.3
    // load as TOUCHPAD_MODE_DS4 — the historical pass-through behaviour.
    uint8_t touchpadMode = TOUCHPAD_MODE_DS4;
};

// ── Update channels ─────────────────────────────────────────────────────────
// "stable" — only published GitHub Releases tagged vMAJOR.MINOR.PATCH.
// "prerelease" — include pre-release tags (vX.Y.Z-rc.1, -beta.2, etc.).
// Anything else is treated as "stable".
inline const char* UPDATE_CHANNEL_STABLE = "stable";
inline const char* UPDATE_CHANNEL_PRERELEASE = "prerelease";

// Default cadence for the background "auto-check" timer.
inline const int UPDATE_DEFAULT_CHECK_INTERVAL_HOURS = 24;

// ── Config ──────────────────────────────────────────────────────────────────
struct Config {
    int udpPort = DEFAULT_UDP_PORT;
    int webPort = DEFAULT_WEB_PORT;
    int pairPort = DEFAULT_PAIR_PORT;
    int discPort = DEFAULT_DISC_PORT;
    // Legacy UDP broadcast beacon on discPort. mDNS/Bonjour (Task 1.6) is the
    // modern discovery path; the broadcast beacon stays on as a fallback for
    // senders that predate the mDNS responder. Operators on an all-mDNS LAN
    // can disable it to cut broadcast noise. Slated for removal in 2027 once
    // the install base has migrated. Absent in pre-1.6 configs → defaults on.
    bool discoveryBroadcastEnabled = true;
    bool autoStart = false;
    std::vector<PairedDevice> pairedDevices;

    // ── OTA update preferences (see core/update_service.h) ─────────────
    // updateChannel:   "stable" or "prerelease".
    // autoCheck:       if true, the background timer hits the GitHub
    //                  releases API every updateCheckIntervalHours.
    // autoDownload:    if true, an available update is fetched in the
    //                  background once discovered (still requires the
    //                  user to confirm the restart-and-install step).
    // autoInstall:     if true, after a successful auto-download the
    //                  updater triggers the platform install flow
    //                  immediately. Off by default — most users want
    //                  the "ready to install" prompt.
    // lastCheckEpoch:  unix seconds of the most recent check attempt
    //                  (0 = never). Surfaced in the settings UI.
    // lastSeenVersion: highest version we've seen since the last user
    //                  acknowledgement. Used so we don't re-pop the
    //                  banner after a user has clicked "remind me later".
    // skipVersion:     a version the user explicitly told us to skip.
    //                  We won't notify again until something newer
    //                  than this appears.
    std::string updateChannel = UPDATE_CHANNEL_STABLE;
    bool autoCheck = true;
    bool autoDownload = false;
    bool autoInstall = false;
    int updateCheckIntervalHours = UPDATE_DEFAULT_CHECK_INTERVAL_HOURS;
    int64_t lastCheckEpoch = 0;
    std::string lastSeenVersion;
    std::string skipVersion;
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
