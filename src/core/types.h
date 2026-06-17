// SPDX-License-Identifier: LGPL-3.0-or-later

// Pure domain types: no Windows, Winsock, or platform gamepad headers.
#pragma once

#include "core/version.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <array>
#include <vector>
#include <chrono>

inline const char* APP_NAME = "satellite";
inline const char* APP_TITLE = "Satellite";

#ifdef SATELLITE_VERSION_RESOLVED
inline const char* SATELLITE_VERSION = SATELLITE_VERSION_RESOLVED;
#else
inline const char* SATELLITE_VERSION = SATELLITE_VERSION_STRING;
#endif

inline const int DEFAULT_UDP_PORT = 9876;
inline const int DEFAULT_WEB_PORT = 9877;
inline const int DEFAULT_PAIR_PORT = 9878;
inline const int DEFAULT_DISC_PORT = 9879;
// Sender-facing API (pairing + connections), HTTPS on 0.0.0.0 — the only TCP
// port a LAN sender connects to. The admin UI on DEFAULT_WEB_PORT is 127.0.0.1.
inline const int DEFAULT_CLIENT_PORT = 9443;

// Connection-state enums driving the `state` strings on /api/connections,
// /api/devices, /api/pin/status.
enum class DeviceLinkState {
    Paired,        // in pairedDevices, no live connection (offline)
    Linking,       // PUT /api/connections handshake in flight (transient)
    Active,        // live connection, packets arriving recently
    NotResponding, // was Active, lastPacketTime > stalling threshold,
                   // not yet reaped (>=2 missed heartbeats, < HEARTBEAT_MISS_MAX)
};

// Pairing PIN lifecycle on the server. PINs rotate on a fixed period; the
// outgoing PIN stays accepted as "previous" for one more period.
enum class PinState {
    PinActive, // rotating current/previous PIN pair is live
    PinPaired, // a device just consumed a PIN — momentary success state
};

// Per-controller pipeline state (physical-on-client → bridge → virtual). The
// APPLY_* codes enumerate failures; this adds the success/transient side.
enum class ControllerState {
    Source,      // physical pad detected on client, not yet forwarded
    Registering, // descriptor PUT in flight
    Allocating,  // descriptor accepted, backend creating the virtual device
    Live,        // virtual device exists, reports flowing
    Quiet,       // Live, but no input recently (optional UI distinction)
    Detached,    // slot deleted, virtual being torn down
    Failed,      // descriptor apply returned an APPLY_ERR_* code
};

// A connection enters NotResponding after ~2 heartbeat intervals without a
// decrypted packet (HEARTBEAT_INTERVAL_SEC * 2 = 4s) but before the reaper
// evicts it (* HEARTBEAT_MISS_MAX = 10s). That window surfaces as "Not responding".
inline const int HEARTBEAT_STALL_FACTOR = 2;

// Lowercase wire strings; the dashboard maps them onto chip text.
inline const char* deviceLinkStateName(DeviceLinkState s) {
    switch (s) {
    case DeviceLinkState::Paired:
        return "paired";
    case DeviceLinkState::Linking:
        return "linking";
    case DeviceLinkState::Active:
        return "active";
    case DeviceLinkState::NotResponding:
        return "notResponding";
    }
    return "paired";
}

inline const char* pinStateName(PinState s) {
    switch (s) {
    case PinState::PinActive:
        return "active";
    case PinState::PinPaired:
        return "paired";
    }
    return "active";
}

inline const char* controllerStateName(ControllerState s) {
    switch (s) {
    case ControllerState::Source:
        return "source";
    case ControllerState::Registering:
        return "registering";
    case ControllerState::Allocating:
        return "allocating";
    case ControllerState::Live:
        return "live";
    case ControllerState::Quiet:
        return "quiet";
    case ControllerState::Detached:
        return "detached";
    case ControllerState::Failed:
        return "failed";
    }
    return "live";
}

// REST + wire protocol version (docs/contract.md). Rides in every pairing and
// session request/response so any future change is gateable.
inline const int PROTOCOL_VERSION = 1;

// Protocol message types (UDP streams + authenticated notifications only —
// topology mutation is REST-only per docs/contract.md).
inline const uint16_t MSG_GAMEPAD_DATA = 0x0001;
inline const uint16_t MSG_HEARTBEAT_PING = 0x0002;
// Enriched ack: backendAvailable(1) + totalActiveControllers(1) + epoch(u16 BE)
// + active-controller bitmap(u16 BE). The epoch/bitmap pair is the client's
// reconcile trigger for involuntary server-side topology changes.
inline const uint16_t MSG_HEARTBEAT_ACK = 0x0003;
inline const uint16_t MSG_RUMBLE = 0x0009;
inline const uint16_t MSG_MOTION = 0x000A;
inline const uint16_t MSG_BATTERY = 0x000B;
inline const uint16_t MSG_TOUCHPAD = 0x000C;
inline const uint16_t MSG_LIGHTBAR = 0x000D;
// Best-effort close notify, sent encrypted BEFORE teardown. Only valid while
// the session key exists (an unauthenticated close would be a spoofable DoS);
// after teardown the only safe channel is REST status codes.
inline const uint16_t MSG_SESSION_CLOSE = 0x000F;

// MSG_SESSION_CLOSE reason byte.
inline const uint8_t CLOSE_REASON_SHUTDOWN = 0;
inline const uint8_t CLOSE_REASON_KICKED = 1;
inline const uint8_t CLOSE_REASON_REPLACED = 2;
inline const uint8_t CLOSE_REASON_UNPAIRED = 3;

inline const char* closeReasonName(uint8_t reason) {
    switch (reason) {
    case CLOSE_REASON_SHUTDOWN:
        return "shutdown";
    case CLOSE_REASON_KICKED:
        return "kicked";
    case CLOSE_REASON_REPLACED:
        return "replaced";
    case CLOSE_REASON_UNPAIRED:
        return "unpaired";
    }
    return "shutdown";
}

// Controller capability bits, carried in ControllerDescriptor::caps (set from
// the JSON caps object of the session PUT). A descriptor with caps 0 means
// "advertises nothing"; every cap reads as best-effort.
inline const uint16_t CAP_ANALOG_TRIGGERS = 0x0001; // analog L/R triggers
inline const uint16_t CAP_RUMBLE = 0x0002;          // accepts the MSG_RUMBLE return path
inline const uint16_t CAP_MOTION = 0x0004;          // streams MSG_MOTION (0x000A) IMU data
inline const uint16_t CAP_LIGHTBAR = 0x0008;        // accepts the MSG_LIGHTBAR (0x000D) return path

// Per-controller apply outcome for the declarative session/controller PUT.
// Wire form is the lowercase string (protocol constant, never localized).
inline const uint8_t APPLY_OK = 0;
inline const uint8_t APPLY_ERR_NO_SLOTS = 1;
inline const uint8_t APPLY_ERR_PLUGIN_FAIL = 2;
inline const uint8_t APPLY_ERR_REPLUG_FAIL = 3;
inline const uint8_t APPLY_ERR_BACKEND_UNAVAIL = 4;
inline const uint8_t APPLY_ERR_INVALID_TYPE = 5;
inline const uint8_t APPLY_ERR_INVALID_INDEX = 6;

inline const char* applyResultName(uint8_t code) {
    switch (code) {
    case APPLY_OK:
        return "ok";
    case APPLY_ERR_NO_SLOTS:
        return "noSlots";
    case APPLY_ERR_PLUGIN_FAIL:
        return "pluginFailed";
    case APPLY_ERR_REPLUG_FAIL:
        return "replugFailed";
    case APPLY_ERR_BACKEND_UNAVAIL:
        return "backendUnavailable";
    case APPLY_ERR_INVALID_TYPE:
        return "invalidType";
    case APPLY_ERR_INVALID_INDEX:
        return "invalidIndex";
    }
    return "pluginFailed";
}

// Wire format sizes
inline const int HEADER_SIZE = 8;       // token(4) + counter(4)
inline const int INNER_HEADER_SIZE = 4; // type(2) + length(2)
inline const int AUTH_TAG_SIZE = 16;    // Poly1305
inline const int CRYPTO_KEY_SIZE = 32;
inline const int CRYPTO_NONCE_SIZE = 12;
inline const int SESSION_SALT_SIZE = 8; // HKDF salt minted per session PUT

// Nonce direction byte (nonce[0]) so the two directions of one session key can
// never share a nonce. See docs/contract.md §Crypto.
inline const uint8_t CRYPTO_DIR_CLIENT_TO_SERVER = 0x00;
inline const uint8_t CRYPTO_DIR_SERVER_TO_CLIENT = 0x01;

// Timeouts & limits
inline const int HEARTBEAT_INTERVAL_SEC = 2;
inline const int HEARTBEAT_MISS_MAX = 5;
inline const int MAX_CONTROLLERS_PER_CONN = 16;
inline const int MAX_BACKEND_CONTROLLERS = 16;

// A fresh/rotated session counts the REST upsert as provisional liveness for
// this long, so a half-open session (UDP blocked one way) surfaces on the
// client instead of flapping through the reaper.
inline const int REST_LIVENESS_GRACE_SEC = 15;

// Controller types — the catalog ids (GET /api/catalog) and descriptor `type`
// values. Adding a type = new id + catalog entry; clients render unknown ids
// from the catalog's server-provided strings.
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

inline bool controllerTypeUsesDS4(uint8_t type) { return type == CONTROLLER_TYPE_PLAYSTATION; }

// Binary-compatible with XUSB_REPORT / XINPUT_GAMEPAD.
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

// Rumble (game → controller, return path). Magnitudes are u16 to match XInput's
// XINPUT_VIBRATION scale (0..65535); on Linux uinput these map to FF_RUMBLE
// strong/weak verbatim. Motor vibration only — the RGB lightbar is a separate
// return path (MSG_LIGHTBAR). durationMs == 0 = hold until the next packet.
struct RumbleReport {
    uint16_t strongMagnitude = 0; // low-frequency / large motor
    uint16_t weakMagnitude = 0;   // high-frequency / small motor
    uint16_t durationMs = 0;      // 0 = continuous (until next packet)
};

// Motion report (sender → satellite, gyro + accel). Fixed full-scale wire
// convention so no downstream renormalisation:
//   gyro  ±2000 deg/s → int16 LSB = 2000/32767 deg/s
//   accel ±4 g        → int16 LSB = 4/32767 g
// Axis frame right-handed, +X right, +Y up, +Z toward player (DualSense IMU
// convention after the manufacturer rotation). Senders MUST apply that matrix;
// receivers do not rotate. timestampDeltaUs = µs since the previous MOTION
// packet for the same controller (first packet 0; u32 spans ~71 min).
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

// MSG_MOTION payload length after the 1-byte ctrlIdx: 6×int16 + uint32.
inline const int MOTION_WIRE_PAYLOAD_BYTES = 16;

// Decode the 16 little-endian wire bytes after ctrlIdx. Explicit shifts (no
// memcpy) keep decoding byte-order-independent. See docs/contract.md.
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

// Battery report (sender → satellite, periodic). Sent every
// BATTERY_REPORT_INTERVAL_SEC and on charging-state transitions. level 0..100;
// 0xFF = sender can't read the level (some BT pads report state only).
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

// Touchpad report (sender → satellite). Max 2 fingers (DS4/DualSense hardware).
// Coordinates are normalized int16 (-32768..32767), resolution-independent — the
// receiver maps to its device's space (DS4 1920×943, mouse: monitor pixels).
// Each finger's trackingId lets the receiver correlate a finger across frames
// when one lifts and another keeps contact (monotonic per controller, wraps).
// buttonPressed is the clicky-pad button, independent of any touch.
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
    // Sender-side sample timestamp (Android MotionEvent.getEventTime() truncated
    // to u32, uptime ms). Mouse-mode time-scales by (curr - prev) so the late
    // first MOVE after touch-down doesn't jump. Resends carry the SAME value;
    // the receiver treats dt == 0 as a duplicate and emits no motion.
    uint32_t eventTimeMs = 0;
};

// Wire layout for MSG_TOUCHPAD payload (after the 1-byte ctrlIdx):
//   flags(1): bit0=finger0_active, bit1=finger1_active, bit2=button_pressed
//   finger0_trackingId(1) + finger0_x(2 LE) + finger0_y(2 LE)
//   finger1_trackingId(1) + finger1_x(2 LE) + finger1_y(2 LE)
//   eventTimeMs(4 LE)
// = 1 + 5 + 5 + 4 = 15 bytes after ctrlIdx → 16 bytes total inner payload.
inline const int TOUCHPAD_WIRE_PAYLOAD_BYTES = 15;

// Decode the TOUCHPAD_WIRE_PAYLOAD_BYTES bytes after ctrlIdx. Explicit LE shifts
// (no memcpy) keep the wire byte-order-independent. See docs/contract.md.
inline TouchpadReport decodeTouchpadReport(const uint8_t* p) {
    auto le16 = [](const uint8_t* q) -> int16_t {
        return static_cast<int16_t>(static_cast<uint16_t>(q[0]) |
                                    (static_cast<uint16_t>(q[1]) << 8));
    };
    auto le32 = [](const uint8_t* q) -> uint32_t {
        return static_cast<uint32_t>(q[0]) | (static_cast<uint32_t>(q[1]) << 8) |
               (static_cast<uint32_t>(q[2]) << 16) | (static_cast<uint32_t>(q[3]) << 24);
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
    r.eventTimeMs = le32(p + 11);
    return r;
}

// Touchpad routing modes (per-controller, client-owned via the descriptor;
// "mouse" additionally requires the session's mouseControl host-feature grant).
//   DS4   — feed into the virtual DS4 touchpad surface; Xbox pads drop it.
//   MOUSE — finger 0 drives the host cursor, clicky button is mouse button 1.
//   OFF   — ignore (still cached for the web UI).
inline const uint8_t TOUCHPAD_MODE_DS4 = 0;
inline const uint8_t TOUCHPAD_MODE_MOUSE = 1;
inline const uint8_t TOUCHPAD_MODE_OFF = 2;
inline const uint8_t TOUCHPAD_MODE_COUNT = 3;

// Host pixels per wire-coordinate unit. The pad spans 65535 units/axis, so a
// full-width swipe travels ~2750 px (laptop-trackpad-like). A sub-pixel
// remainder is carried so slow drags don't truncate to zero.
inline const float TOUCHPAD_MOUSE_SENSITIVITY = 0.042f;

// Reference sample spacing the sensitivity was calibrated against (4 ms). Delta
// scales by REFERENCE_MS/dt, so cursor velocity is independent of the client's
// actual send rate — a faster client just yields more, smaller deltas.
inline const int TOUCHPAD_MOUSE_REFERENCE_MS = 4;

// Clamp the divisor: below this dt the scale factor explodes and a near-
// duplicate sample would fling the cursor.
inline const int TOUCHPAD_MOUSE_MIN_DT_MS = 1;

// Above this dt assume a pause/background/stall: re-anchor (no motion, drop
// remainder) rather than let the scale → 0 produce a huge dx next sample.
inline const int TOUCHPAD_MOUSE_MAX_GAP_MS = 100;

// Wire/UI name for a mode (protocol constant — never localized). The inverse
// mapping lives in the descriptor parser, which defaults unknowns to OFF.
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

// Declarative per-controller desired state, parsed from the session/controller
// PUT body. Always sent WHOLE by the client (a toggle = re-send with one field
// changed); the server converges. See docs/contract.md.
struct ControllerDescriptor {
    uint8_t ctrlIdx = 0;
    uint8_t type = CONTROLLER_TYPE_XBOX; // catalog id (wire enum value)
    uint16_t caps = 0;                   // CAP_* word
    uint8_t touchpadMode = TOUCHPAD_MODE_OFF;
};

// Per-controller apply outcome returned in the PUT response body.
struct ControllerApplyResult {
    uint8_t ctrlIdx = 0;
    uint8_t result = APPLY_OK; // APPLY_*
    // Type actually in force after the converge (== descriptor type on success;
    // the previous type after a failed replug left the old pad untouched).
    uint8_t appliedType = CONTROLLER_TYPE_XBOX;
    bool motionSinkSupportedForType = false;
    bool motionBackendOk = false;
};

// Controller (per virtual gamepad).
struct Controller {
    uint8_t index = 0;     // 0-based index within the connection
    uint32_t serialNo = 0; // backend serial (1–16), 0 = not plugged
    bool active = false;
    uint8_t controllerType = CONTROLLER_TYPE_XBOX; // device family actually plugged
    bool usesDS4 = false; // hot-path cache of controllerTypeUsesDS4(controllerType)
    uint16_t caps = 0;    // CAP_* word from the descriptor
    // TOUCHPAD_MODE_*; client-owned via the descriptor. MOUSE routing is gated
    // on the connection's mouseControlGranted.
    uint8_t touchpadMode = TOUCHPAD_MODE_OFF;
    bool motionCapable() const { return (caps & CAP_MOTION) != 0; }
    bool lightbarCapable() const { return (caps & CAP_LIGHTBAR) != 0; }
    GamepadReport lastReport{};
    // Last rumble forwarded to the dish; used to coalesce identical back-to-back
    // updates so a game holding the motors steady doesn't blast the wire.
    RumbleReport lastRumble{};
    bool lastRumbleValid = false;
    MotionReport lastMotion{}; // last sample, cached for the web UI debug pane
    bool lastMotionValid = false;
    // True when the last motion sample reached the backend's IMU surface. False
    // = cached but not delivered (Xbox device, old ViGEmBus, or macOS).
    bool motionSinkActive = false;
    BatteryReport lastBattery{}; // last sample, cached for the web UI
    bool lastBatteryValid = false;
    TouchpadReport lastTouchpad{}; // last sample, cached for the web UI
    bool lastTouchpadValid = false;
    // TOUCHPAD_MODE_MOUSE sub-pixel remainder. Reset on a fresh contact and on
    // (re)plug; preserved across dt<=0 resends so slow drags still accumulate.
    float touchpadMouseRemX = 0.0f;
    float touchpadMouseRemY = 0.0f;
    // Last lightbar colour the host game set; forwarded via MSG_LIGHTBAR (for
    // CAP_LIGHTBAR senders) and surfaced in the web UI.
    uint8_t lightbarR = 0;
    uint8_t lightbarG = 0;
    uint8_t lightbarB = 0;
    bool lastLightbarValid = false;
};

// Connection (per paired client session). The row is keyed on deviceId and is
// STABLE across reconnects: a re-PUT rotates token/salt/sessionKey in place,
// it never tears the row (or its pads) down.
struct Connection {
    // Stable across reconnects ("conn_" + 8 hex), minted once per row.
    std::string connectionId;
    uint32_t token = 0; // rotates on every session PUT; UDP routes by it
    std::string deviceId;
    std::string deviceName;
    // Hot-path IPv4 in NETWORK byte order (matches sockaddr_in::sin_addr).
    // Compared every packet; only on change do we re-format clientIP below,
    // saving the per-packet inet_ntop + std::string allocation.
    uint32_t clientIPv4 = 0;
    std::string clientIP; // human-readable cache of clientIPv4, refreshed lazily
    // Per-session key = HKDF(pairingKey, sessionSalt, token); never the raw
    // pairing key, so counters restart at 1 with no cross-session nonce reuse.
    uint8_t sessionKey[CRYPTO_KEY_SIZE] = {};
    uint8_t sessionSalt[SESSION_SALT_SIZE] = {};
    uint32_t lastCounter = 0; // replay protection (client → server direction)
    // Bumps on every applied-topology change regardless of initiator; echoed in
    // PUT/GET responses and every heartbeat ack so the client can reconcile.
    uint16_t epoch = 0;
    std::chrono::steady_clock::time_point lastPacketTime;
    std::chrono::steady_clock::time_point connectedAt;
    // REST-open provisional-liveness window; the reaper ignores the row until
    // it lapses (REST_LIVENESS_GRACE_SEC after each PUT).
    std::chrono::steady_clock::time_point graceUntil;
    std::array<Controller, MAX_CONTROLLERS_PER_CONN> controllers;
    int activeControllerCount = 0;
    // Host-feature grants for this session (server policy, returned in the PUT
    // response). Streams for ungranted features are dropped.
    bool mouseControlGranted = false;
};

// Paired device info (persisted).
struct PairedDevice {
    std::string id;
    std::string name;
    std::string lastIP;
    std::string pairedAt;
    std::string sharedKeyHex; // 64-char hex (32 bytes) — the pairing key
};

// Update channels: "stable" = released vMAJOR.MINOR.PATCH only; "prerelease"
// also includes -rc/-beta tags. Anything else is treated as "stable".
inline const char* UPDATE_CHANNEL_STABLE = "stable";
inline const char* UPDATE_CHANNEL_PRERELEASE = "prerelease";

inline const int UPDATE_DEFAULT_CHECK_INTERVAL_HOURS = 24;

struct Config {
    int udpPort = DEFAULT_UDP_PORT;
    int webPort = DEFAULT_WEB_PORT;
    int pairPort = DEFAULT_PAIR_PORT;
    int discPort = DEFAULT_DISC_PORT;
    // Legacy UDP broadcast beacon, kept as a fallback for senders predating the
    // mDNS responder. Slated for removal in 2027. Absent in pre-1.6 configs → on.
    bool discoveryBroadcastEnabled = true;
    bool autoStart = false;
    std::vector<PairedDevice> pairedDevices;

    // OTA update preferences (see core/update_service.h).
    //   autoDownload: fetch in the background once discovered (still needs user
    //                 confirm to install).
    //   autoInstall:  install immediately after an auto-download. Off by default.
    //   lastSeenVersion: highest version seen since the last ack, so "remind me
    //                 later" doesn't re-pop the banner.
    //   skipVersion:  suppress notifications until something newer appears.
    std::string updateChannel = UPDATE_CHANNEL_STABLE;
    bool autoCheck = true;
    bool autoDownload = false;
    bool autoInstall = false;
    int updateCheckIntervalHours = UPDATE_DEFAULT_CHECK_INTERVAL_HOURS;
    int64_t lastCheckEpoch = 0;
    std::string lastSeenVersion;
    std::string skipVersion;
    std::string networkInterface;
    bool allowPublicNetwork = false;
};

enum class LogLevel { INFO, WARN, ERR };
inline const int LOG_RING_SIZE = 500;

struct LogEntry {
    std::chrono::system_clock::time_point timestamp;
    LogLevel level;
    std::string source;
    std::string message;
};

// Host-feature deny reasons (protocol constants, never localized).
inline const char* HOST_FEATURE_DENY_NOT_SUPPORTED = "notSupported";
inline const char* HOST_FEATURE_DENY_BACKEND_UNAVAILABLE = "backendUnavailable";

// Outcome of the declarative session upsert (PUT /api/connections).
struct SessionUpsertResult {
    bool ok = false;
    std::string error; // non-empty only when !ok
    std::string connectionId;
    uint32_t token = 0;
    uint8_t sessionSalt[SESSION_SALT_SIZE] = {};
    uint16_t epoch = 0;
    int maxControllers = MAX_BACKEND_CONTROLLERS;
    std::vector<ControllerApplyResult> controllers;
    bool mouseControlGranted = false;
    std::string mouseControlDenyReason; // empty when granted
};
