// SPDX-License-Identifier: LGPL-3.0-or-later

#include "session_service.h"

#include "ipv4_util.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

// Token generation uses std::random rather than libsodium to keep the core
// libsodium-free.
#include <random>

using satellite::formatIPv4Nbo;
using satellite::parseIPv4Nbo;

static uint32_t makeRandomToken() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist(1, UINT32_MAX);
    return dist(gen);
}

SessionService::SessionService(IGamepadPort& backend, IClientPort& client, ILogPort& log)
    : backend_(backend), client_(client), log_(log) {
    // The `this`-capturing lambdas are safe: SessionService outlives the adapter
    // (composition root tears adapters down last) and the adapter joins its
    // notification workers before the callbacks could fire.
    backend_.setRumbleCallback(
        [this](uint32_t serial, const RumbleReport& r) { handleRumbleFromBackend(serial, r); });
    backend_.setLightbarCallback([this](uint32_t serial, uint8_t r, uint8_t g, uint8_t b) {
        handleLightbarFromBackend(serial, r, g, b);
    });
}

// Internal helpers below assume the caller holds mtx_.

void SessionService::teardownConnection(Connection& conn) {
    for (auto& ctrl : conn.controllers) {
        if (ctrl.active && ctrl.serialNo != 0) {
            backend_.unplugDevice(ctrl.serialNo);
            releaseSerial(ctrl.serialNo);
            ctrl.active = false;
            ctrl.serialNo = 0;
        }
    }
    conn.activeControllerCount = 0;
    client_.removeClientAddr(conn.token);
}

uint32_t SessionService::allocateSerial() {
    for (size_t i = 0; i < MAX_BACKEND_CONTROLLERS; i++) {
        if (!serialInUse_[i]) {
            serialInUse_[i] = true;
            return static_cast<uint32_t>(i + 1); // 1-based
        }
    }
    return 0;
}

void SessionService::releaseSerial(uint32_t serial) {
    if (serial == 0 || serial > (uint32_t)MAX_BACKEND_CONTROLLERS) return;
    serialInUse_[serial - 1] = false;
}

int SessionService::countGlobalActiveControllers() const {
    int total = 0;
    for (auto& [tok, c] : connections_) { total += c.activeControllerCount; }
    return total;
}

void SessionService::closeBackendBusIfIdle() {
    if (!backend_.isBusOpen()) return;
    if (countGlobalActiveControllers() == 0) {
        backend_.closeBus();
        log_.logMsg(LogLevel::INFO, "service", "Backend bus closed (no active controllers)");
    }
}

void SessionService::broadcastStatus() {
    std::vector<std::pair<uint32_t, const Connection*>> conns;
    for (auto& [tok, c] : connections_) { conns.push_back({tok, &c}); }
    client_.broadcastServerStatus(conns, backend_.isBusOpen(),
                                  (uint8_t)countGlobalActiveControllers());
}

uint32_t SessionService::generateUniqueToken() {
    uint32_t token;
    do { token = makeRandomToken(); } while (connections_.count(token));
    return token;
}

OpenSessionResult SessionService::openSession(const std::string& deviceId,
                                              const std::string& deviceName,
                                              const std::string& clientIP,
                                              const uint8_t sharedKey[CRYPTO_KEY_SIZE],
                                              uint8_t touchpadMode) {
    std::lock_guard<std::mutex> lk(mtx_);

    for (auto it = connections_.begin(); it != connections_.end();) {
        if (it->second.deviceId == deviceId) {
            log_.logMsg(LogLevel::INFO, "service", "Replacing stale connection for " + deviceName);
            teardownConnection(it->second);
            it = connections_.erase(it);
        } else {
            ++it;
        }
    }
    closeBackendBusIfIdle();

    uint32_t token = generateUniqueToken();

    Connection conn;
    conn.token = token;
    conn.deviceId = deviceId;
    conn.deviceName = deviceName;
    conn.clientIP = clientIP;
    // Seed the numeric IPv4 cache so the first packet's "address changed?" check
    // has something to compare. Failure (0) is non-fatal — the next V4 update fills it.
    conn.clientIPv4 = parseIPv4Nbo(clientIP);
    std::memcpy(conn.sharedKey, sharedKey, CRYPTO_KEY_SIZE);
    conn.lastCounter = 0;
    conn.lastPacketTime = std::chrono::steady_clock::now();
    conn.connectedAt = std::chrono::steady_clock::now();
    conn.activeControllerCount = 0;
    conn.touchpadMode = (touchpadMode < TOUCHPAD_MODE_COUNT) ? touchpadMode : TOUCHPAD_MODE_OFF;

    connections_[token] = conn;

    int slots =
        static_cast<int>(std::count(std::begin(serialInUse_), std::end(serialInUse_), false));

    log_.logMsg(LogLevel::INFO, "service",
                "Connection opened for " + deviceName + " (token " + std::to_string(token) + ")");

    return {true, token, slots, ""};
}

int SessionService::closeSession(uint32_t token) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = connections_.find(token);
    if (it == connections_.end()) return -1;

    int removed = it->second.activeControllerCount;
    std::string devName = it->second.deviceName;
    teardownConnection(it->second);
    connections_.erase(it);
    closeBackendBusIfIdle();

    log_.logMsg(LogLevel::INFO, "service",
                "Connection closed for " + devName + " (" + std::to_string(removed) +
                    " controllers removed)");
    return removed;
}

void SessionService::closeAllSessions() {
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& [tok, conn] : connections_) { teardownConnection(conn); }
    connections_.clear();
    std::memset(serialInUse_, 0, sizeof(serialInUse_));
}

bool SessionService::handleGamepadData(uint32_t token, uint8_t ctrlIdx,
                                       const GamepadReport& report) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = connections_.find(token);
    if (it == connections_.end()) return false;

    if (ctrlIdx >= MAX_CONTROLLERS_PER_CONN) return false;
    Controller& ctrl = it->second.controllers[ctrlIdx];
    if (!ctrl.active) return false;

    ctrl.lastReport = report;
    if (controllerTypeUsesDS4(ctrl.controllerType)) {
        return backend_.submitDS4Report(ctrl.serialNo, report);
    }
    return backend_.submitReport(ctrl.serialNo, report);
}

void SessionService::handleHeartbeat(uint32_t token) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = connections_.find(token);
    if (it == connections_.end()) return;

    const Connection& conn = it->second;
    client_.sendHeartbeatAck(conn);
    client_.sendServerStatus(conn, backend_.isBusOpen(), (uint8_t)countGlobalActiveControllers());
}

void SessionService::handleControllerAdd(uint32_t token, uint8_t ctrlIdx, uint16_t caps,
                                         uint8_t controllerType) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = connections_.find(token);
    if (it == connections_.end()) return;

    Connection& conn = it->second;
    if (ctrlIdx >= MAX_CONTROLLERS_PER_CONN) return;

    Controller& ctrl = conn.controllers[ctrlIdx];
    if (ctrl.active) {
        client_.sendControllerAck(conn, MSG_CONTROLLER_ADD, ctrlIdx, ACK_ERR_ALREADY_EXISTS);
        return;
    }

    if (!backend_.isBusOpen()) {
        if (backend_.ensureBusOpen()) {
            log_.logMsg(LogLevel::INFO, "service", "Backend bus opened on demand");
            broadcastStatus();
        }
    }
    if (!backend_.isBusOpen()) {
        log_.logMsg(LogLevel::ERR, "service", "Controller add failed: backend bus unavailable");
        client_.sendControllerAck(conn, MSG_CONTROLLER_ADD, ctrlIdx, ACK_ERR_BACKEND_UNAVAIL);
        return;
    }

    uint32_t serial = allocateSerial();
    if (serial == 0) {
        client_.sendControllerAck(conn, MSG_CONTROLLER_ADD, ctrlIdx, ACK_ERR_NO_SLOTS);
        return;
    }

    // A pre-extension dish omits the type byte (UNSPECIFIED): retain the slot's
    // existing type. A current dish supplies it so the first plug is the correct
    // device (no follow-up MSG_CONTROLLER_TYPE / replug).
    if (controllerType != CONTROLLER_TYPE_UNSPECIFIED) {
        ctrl.controllerType =
            (controllerType < CONTROLLER_TYPE_COUNT) ? controllerType : CONTROLLER_TYPE_XBOX;
    }
    bool plugOk = controllerTypeUsesDS4(ctrl.controllerType) ? backend_.pluginDeviceDS4(serial)
                                                             : backend_.pluginDevice(serial);
    if (!plugOk) {
        releaseSerial(serial);
        client_.sendControllerAck(conn, MSG_CONTROLLER_ADD, ctrlIdx, ACK_ERR_PLUGIN_FAIL);
        return;
    }

    ctrl.index = ctrlIdx;
    ctrl.serialNo = serial;
    ctrl.active = true;
    ctrl.caps = caps;
    ctrl.usesDS4 = controllerTypeUsesDS4(ctrl.controllerType); // hot-path cache mirror
    ctrl.lastReport = GamepadReport{};
    // Clear rumble/lightbar coalesce state: a re-added controller is a fresh
    // actuator, so the next rumble/colour must not be suppressed as "same as last".
    ctrl.lastRumble = RumbleReport{};
    ctrl.lastRumbleValid = false;
    ctrl.lightbarR = 0;
    ctrl.lightbarG = 0;
    ctrl.lightbarB = 0;
    ctrl.lastLightbarValid = false;
    // Clear cached sender→satellite streams too: the Controller struct persists
    // across a remove/re-add of the same slot, so stale samples would show
    // phantom "active" state and — in TOUCHPAD_MODE_MOUSE — make the first
    // sample compute a delta against a pre-readd finger position, jumping the cursor.
    ctrl.lastMotion = MotionReport{};
    ctrl.lastMotionValid = false;
    ctrl.motionSinkActive = false;
    ctrl.lastBattery = BatteryReport{};
    ctrl.lastBatteryValid = false;
    ctrl.lastTouchpad = TouchpadReport{};
    ctrl.lastTouchpadValid = false;
    ctrl.touchpadMouseRemX = 0.0f;
    ctrl.touchpadMouseRemY = 0.0f;
    conn.activeControllerCount++;

    log_.logMsg(LogLevel::INFO, "service",
                "Controller #" + std::to_string(ctrlIdx) + " added (serial " +
                    std::to_string(serial) + ") for " + conn.deviceName);

    // Motion-status byte on the ACK so the dish can show "motion toggled on but
    // the kernel rejected the uinput node" instead of a false STREAMING. Only on
    // the success path; error paths above pass the default 0.
    uint8_t motionFlags = 0;
    if (backend_.supportsMotionForType(ctrl.controllerType)) {
        motionFlags |= ACK_MOTION_FLAG_SINK_SUPPORTED_FOR_TYPE;
    }
    if (backend_.motionBackendOk(serial)) { motionFlags |= ACK_MOTION_FLAG_BACKEND_OK; }
    client_.sendControllerAck(conn, MSG_CONTROLLER_ADD, ctrlIdx, ACK_OK, motionFlags);
    broadcastStatus();
}

void SessionService::handleControllerRemove(uint32_t token, uint8_t ctrlIdx) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = connections_.find(token);
    if (it == connections_.end()) return;

    Connection& conn = it->second;
    if (ctrlIdx >= MAX_CONTROLLERS_PER_CONN) return;

    Controller& ctrl = conn.controllers[ctrlIdx];
    if (!ctrl.active) {
        client_.sendControllerAck(conn, MSG_CONTROLLER_REMOVE, ctrlIdx, ACK_ERR_NOT_FOUND);
        return;
    }

    log_.logMsg(LogLevel::INFO, "service",
                "Controller #" + std::to_string(ctrlIdx) + " removed from " + conn.deviceName);

    backend_.unplugDevice(ctrl.serialNo);
    releaseSerial(ctrl.serialNo);
    ctrl.active = false;
    ctrl.serialNo = 0;
    conn.activeControllerCount--;

    closeBackendBusIfIdle();
    client_.sendControllerAck(conn, MSG_CONTROLLER_REMOVE, ctrlIdx, ACK_OK);
    broadcastStatus();
}

void SessionService::handleControllerCapsUpdate(uint32_t token, uint8_t ctrlIdx, uint16_t caps) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = connections_.find(token);
    if (it == connections_.end()) return;

    Connection& conn = it->second;
    if (ctrlIdx >= MAX_CONTROLLERS_PER_CONN) return;

    Controller& ctrl = conn.controllers[ctrlIdx];
    if (!ctrl.active) return;

    uint16_t oldCaps = ctrl.caps;
    if (oldCaps == caps) return; // no-op — don't churn the log on duplicate ticks
    ctrl.caps = caps;

    log_.logMsg(LogLevel::INFO, "service",
                "Controller #" + std::to_string(ctrlIdx) + " caps updated 0x" +
                    std::to_string(oldCaps) + " → 0x" + std::to_string(caps) + " (" +
                    conn.deviceName + ")");

    broadcastStatus();
}

void SessionService::handleControllerType(uint32_t token, uint8_t ctrlIdx, uint8_t controllerType) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = connections_.find(token);
    if (it == connections_.end()) return;

    Connection& conn = it->second;
    if (ctrlIdx >= MAX_CONTROLLERS_PER_CONN) return;

    Controller& ctrl = conn.controllers[ctrlIdx];
    if (!ctrl.active) return;

    uint8_t safeType =
        (controllerType < CONTROLLER_TYPE_COUNT) ? controllerType : CONTROLLER_TYPE_XBOX;
    uint8_t oldType = ctrl.controllerType;
    ctrl.controllerType = safeType;
    ctrl.usesDS4 = controllerTypeUsesDS4(safeType); // hot-path cache mirror

    // Switching between DS4 and non-DS4 requires replugging the virtual device.
    bool wasDS4 = controllerTypeUsesDS4(oldType);
    bool isDS4 = controllerTypeUsesDS4(safeType);
    if (wasDS4 != isDS4 && ctrl.serialNo != 0) {
        uint32_t serial = ctrl.serialNo;
        backend_.unplugDevice(serial);

        bool ok = isDS4 ? backend_.pluginDeviceDS4(serial) : backend_.pluginDevice(serial);
        if (!ok) {
            log_.logMsg(LogLevel::ERR, "service",
                        "Failed to replug controller #" + std::to_string(ctrlIdx) + " as " +
                            controllerTypeLabel(safeType));
        } else {
            log_.logMsg(LogLevel::INFO, "service",
                        "Replugged controller #" + std::to_string(ctrlIdx) + " as " +
                            controllerTypeLabel(safeType) + " (serial " + std::to_string(serial) +
                            ")");
            // The replug rebuilt the virtual device, so the motion flags the dish
            // latched at ADD time are stale. Re-send a fresh ACK.
            uint8_t motionFlags = 0;
            if (backend_.supportsMotionForType(ctrl.controllerType)) {
                motionFlags |= ACK_MOTION_FLAG_SINK_SUPPORTED_FOR_TYPE;
            }
            if (backend_.motionBackendOk(serial)) { motionFlags |= ACK_MOTION_FLAG_BACKEND_OK; }
            client_.sendControllerAck(conn, MSG_CONTROLLER_TYPE, ctrlIdx, ACK_OK, motionFlags);
        }
    } else {
        log_.logMsg(LogLevel::INFO, "service",
                    "Controller #" + std::to_string(ctrlIdx) + " type set to " +
                        controllerTypeLabel(safeType) + " (" + conn.deviceName + ")");
    }

    broadcastStatus();
}

void SessionService::handleRumbleFromBackend(uint32_t serial, const RumbleReport& report,
                                             uint16_t wireDurationMs) {
    // Linear scan for the controller owning `serial`: bounded by the global
    // 16-controller cap, and a reverse index would have to stay consistent with
    // allocateSerial/releaseSerial — not worth it for this callback.
    std::lock_guard<std::mutex> lk(mtx_);

    Connection* foundConn = nullptr;
    Controller* foundCtrl = nullptr;
    for (auto& [tok, conn] : connections_) {
        for (auto& ctrl : conn.controllers) {
            if (ctrl.active && ctrl.serialNo == serial) {
                foundConn = &conn;
                foundCtrl = &ctrl;
                break;
            }
        }
        if (foundConn) break;
    }
    if (!foundConn || !foundCtrl) {
        // Stray notification: the controller was just unplugged but a worker
        // fired one last queued event. Drop silently.
        return;
    }

    // Coalesce identical back-to-back updates (games hold both motors steady
    // across many frames). wireDurationMs is excluded so the caller can bump
    // the refresh deadline without forcing a packet.
    auto sameAs = [](const RumbleReport& a, const RumbleReport& b) {
        return a.strongMagnitude == b.strongMagnitude && a.weakMagnitude == b.weakMagnitude;
    };
    if (foundCtrl->lastRumbleValid && sameAs(foundCtrl->lastRumble, report)) { return; }

    RumbleReport stamped = report;
    stamped.durationMs = wireDurationMs;

    foundCtrl->lastRumble = stamped;
    foundCtrl->lastRumbleValid = true;

    client_.sendRumble(*foundConn, foundCtrl->index, stamped);
}

bool SessionService::handleMotionData(uint32_t token, uint8_t ctrlIdx, const MotionReport& report) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = connections_.find(token);
    if (it == connections_.end()) return false;
    if (ctrlIdx >= MAX_CONTROLLERS_PER_CONN) return false;

    Controller& ctrl = it->second.controllers[ctrlIdx];
    if (!ctrl.active) return false;

    // Cache for the web-UI debug pane regardless of backend acceptance.
    ctrl.lastMotion = report;
    ctrl.lastMotionValid = true;

    // Deliberately do NOT gate on ctrl.motionCapable() (CAP_MOTION): the
    // protocol says motion is best-effort and the receiver MUST accept
    // MSG_MOTION even from a dish that never advertised the cap. A backend
    // without an IMU surface returning false is not an error. The return value
    // (surfaced as `motionSink`) records whether the sample reached the device.
    const bool delivered = backend_.submitMotion(ctrl.serialNo, report);
    ctrl.motionSinkActive = delivered;
    return delivered;
}

bool SessionService::handleTouchpadData(uint32_t token, uint8_t ctrlIdx,
                                        const TouchpadReport& report) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = connections_.find(token);
    if (it == connections_.end()) return false;
    if (ctrlIdx >= MAX_CONTROLLERS_PER_CONN) return false;

    Connection& conn = it->second;
    Controller& ctrl = conn.controllers[ctrlIdx];
    if (!ctrl.active) return false;

    // Capture the previous sample BEFORE overwriting — relative-mouse mode needs
    // the finger-0 delta between consecutive frames.
    const TouchpadReport prev = ctrl.lastTouchpad;
    const bool prevValid = ctrl.lastTouchpadValid;

    // Cache for the web-UI debug pane regardless of routing mode / backend.
    ctrl.lastTouchpad = report;
    ctrl.lastTouchpadValid = true;

    switch (conn.touchpadMode) {
    case TOUCHPAD_MODE_OFF:
        return false; // cached above, nothing else

    case TOUCHPAD_MODE_MOUSE: {
        // Finger 0 drives the cursor; a delta is emitted only while finger 0 is
        // continuously down across both frames AND the trackingId matches —
        // when one finger lifts the hardware compacts the survivor into slot 0
        // with a new trackingId, so without the id check that teleports the
        // cursor. The per-sample delta is scaled by REFERENCE_MS/dt so cursor
        // velocity tracks finger velocity even when the dish runs slow (the
        // first MOVE after touch-down arrives up to ~16 ms late on Android,
        // causing a visible "first-touch jump" without scaling).
        int dx = 0;
        int dy = 0;
        const bool continuous = prevValid && prev.finger0.active && report.finger0.active &&
                                prev.finger0.trackingId == report.finger0.trackingId;
        if (continuous) {
            // Unsigned subtract through int32_t handles the u32 uptime wrap and
            // cheaply flags out-of-order packets (dt <= 0) in one branch.
            const int32_t dt_ms = static_cast<int32_t>(report.eventTimeMs - prev.eventTimeMs);
            if (dt_ms <= 0) {
                // Duplicate/out-of-order: no motion to integrate. Preserve the
                // remainder so a slow drag still accumulates across resends.
            } else if (dt_ms > TOUCHPAD_MOUSE_MAX_GAP_MS) {
                // Big gap (dish paused/backgrounded/stalled). Re-anchor: no
                // motion, drop the remainder so a stale one doesn't catch up.
                ctrl.touchpadMouseRemX = 0.0f;
                ctrl.touchpadMouseRemY = 0.0f;
            } else {
                const int32_t dt_clamped =
                    dt_ms < TOUCHPAD_MOUSE_MIN_DT_MS ? TOUCHPAD_MOUSE_MIN_DT_MS : dt_ms;
                const float scale = static_cast<float>(TOUCHPAD_MOUSE_REFERENCE_MS) /
                                    static_cast<float>(dt_clamped);
                const float fx = static_cast<float>(report.finger0.x - prev.finger0.x) *
                                     TOUCHPAD_MOUSE_SENSITIVITY * scale +
                                 ctrl.touchpadMouseRemX;
                const float fy = static_cast<float>(report.finger0.y - prev.finger0.y) *
                                     TOUCHPAD_MOUSE_SENSITIVITY * scale +
                                 ctrl.touchpadMouseRemY;
                dx = static_cast<int>(fx);
                dy = static_cast<int>(fy);
                // Keep the sub-pixel remainder so a slow drag still moves.
                ctrl.touchpadMouseRemX = fx - static_cast<float>(dx);
                ctrl.touchpadMouseRemY = fy - static_cast<float>(dy);
            }
        } else {
            ctrl.touchpadMouseRemX = 0.0f;
            ctrl.touchpadMouseRemY = 0.0f;
        }
        return backend_.submitRelativeMouse(dx, dy, report.buttonPressed);
    }

    case TOUCHPAD_MODE_DS4:
    default:
        // Pass through to the virtual DS4 touchpad. A false from the default
        // impl (Xbox pad / inert backend) is not an error — cached above.
        return backend_.submitTouchpad(ctrl.serialNo, report);
    }
}

bool SessionService::setTouchpadMode(const std::string& deviceId, uint8_t mode) {
    if (mode >= TOUCHPAD_MODE_COUNT) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    bool updated = false;
    for (auto& [tok, conn] : connections_) {
        if (conn.deviceId != deviceId) continue;
        conn.touchpadMode = mode;
        // Reset the relative-mouse accumulators so a stale remainder doesn't
        // carry into the new mode.
        for (auto& ctrl : conn.controllers) {
            ctrl.touchpadMouseRemX = 0.0f;
            ctrl.touchpadMouseRemY = 0.0f;
        }
        updated = true;
    }
    return updated;
}

void SessionService::handleLightbarFromBackend(uint32_t serial, uint8_t r, uint8_t g, uint8_t b) {
    std::lock_guard<std::mutex> lk(mtx_);

    // Same serial → controller scan as the rumble callback; bounded by the
    // global 16-controller cap.
    Connection* foundConn = nullptr;
    Controller* foundCtrl = nullptr;
    for (auto& [tok, conn] : connections_) {
        for (auto& ctrl : conn.controllers) {
            if (ctrl.active && ctrl.serialNo == serial) {
                foundConn = &conn;
                foundCtrl = &ctrl;
                break;
            }
        }
        if (foundCtrl != nullptr) break;
    }
    if (foundCtrl == nullptr || foundConn == nullptr) return;

    // Coalesce unchanged colours (same shape as the rumble coalesce).
    if (foundCtrl->lastLightbarValid && foundCtrl->lightbarR == r && foundCtrl->lightbarG == g &&
        foundCtrl->lightbarB == b) {
        return;
    }
    foundCtrl->lightbarR = r;
    foundCtrl->lightbarG = g;
    foundCtrl->lightbarB = b;
    foundCtrl->lastLightbarValid = true;

    // Emit only to senders that advertised CAP_LIGHTBAR (an addressable RGB
    // LED); others would just drop it. Cached above regardless for the web UI.
    if (foundCtrl->lightbarCapable()) {
        client_.sendLightbar(*foundConn, foundCtrl->index, r, g, b);
    }
}

bool SessionService::handleBatteryUpdate(uint32_t token, uint8_t ctrlIdx,
                                         const BatteryReport& report) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = connections_.find(token);
    if (it == connections_.end()) return false;
    if (ctrlIdx >= MAX_CONTROLLERS_PER_CONN) return false;

    Controller& ctrl = it->second.controllers[ctrlIdx];
    if (!ctrl.active) return false;

    // 0xFF is the documented "unknown" sentinel; 101..254 is malformed.
    if (report.level != BATTERY_LEVEL_UNKNOWN && report.level > 100) return false;
    if (report.status >= BATTERY_STATUS_COUNT) return false;

    ctrl.lastBattery = report;
    ctrl.lastBatteryValid = true;

    backend_.submitBattery(ctrl.serialNo, report); // no-op on backends without a battery surface
    return true;
}

bool SessionService::getDecryptInfo(uint32_t token, uint8_t outKey[CRYPTO_KEY_SIZE],
                                    uint32_t& outLastCounter) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = connections_.find(token);
    if (it == connections_.end()) return false;
    std::memcpy(outKey, it->second.sharedKey, CRYPTO_KEY_SIZE);
    outLastCounter = it->second.lastCounter;
    return true;
}

void SessionService::updatePostDecrypt(uint32_t token, uint32_t counter,
                                       const std::string& clientIP, uint16_t clientPort) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = connections_.find(token);
    if (it == connections_.end()) return;
    it->second.lastCounter = counter;
    it->second.lastPacketTime = std::chrono::steady_clock::now();
    it->second.clientIP = clientIP;
    it->second.clientIPv4 = parseIPv4Nbo(clientIP);
    client_.updateClientAddr(token, clientIP, clientPort);
}

// Refresh Connection::clientIP only when the numeric IPv4 changes (common case
// is one integer compare, no heap touch). Caller must hold mtx_.
static void refreshClientIPCacheLocked(Connection& conn, uint32_t newIPv4) {
    if (conn.clientIPv4 == newIPv4) return;
    conn.clientIPv4 = newIPv4;
    conn.clientIP = formatIPv4Nbo(newIPv4);
}

void SessionService::updatePostDecryptV4(uint32_t token, uint32_t counter,
                                         uint32_t ipv4NetworkOrder, uint16_t clientPort) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = connections_.find(token);
    if (it == connections_.end()) return;
    Connection& conn = it->second;
    conn.lastCounter = counter;
    conn.lastPacketTime = std::chrono::steady_clock::now();
    refreshClientIPCacheLocked(conn, ipv4NetworkOrder);
    client_.updateClientAddrV4(token, ipv4NetworkOrder, clientPort);
}

bool SessionService::handleGamepadDataAndUpdate(uint32_t token, uint32_t counter,
                                                uint32_t ipv4NetworkOrder, uint16_t clientPort,
                                                uint8_t ctrlIdx, const GamepadReport& report) {
    // ONE lock acquisition for the whole packet: the unfused path took mtx_
    // three times (getDecryptInfo + updatePostDecrypt + handleGamepadData),
    // giving SSE/heartbeat threads three chances to add tail latency.
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = connections_.find(token);
    if (it == connections_.end()) return false;
    Connection& conn = it->second;

    conn.lastCounter = counter;
    conn.lastPacketTime = std::chrono::steady_clock::now();
    refreshClientIPCacheLocked(conn, ipv4NetworkOrder);
    client_.updateClientAddrV4(token, ipv4NetworkOrder, clientPort);

    if (ctrlIdx >= MAX_CONTROLLERS_PER_CONN) return false;
    Controller& ctrl = conn.controllers[ctrlIdx];
    if (!ctrl.active) return false;

    ctrl.lastReport = report;
    if (ctrl.usesDS4) { return backend_.submitDS4Report(ctrl.serialNo, report); }
    return backend_.submitReport(ctrl.serialNo, report);
}

SessionService::ConnectionsSnapshot SessionService::getConnectionsSnapshot() const {
    std::lock_guard<std::mutex> lk(mtx_);
    ConnectionsSnapshot snap;
    snap.totalControllers = 0;
    snap.maxControllers = MAX_BACKEND_CONTROLLERS;
    snap.backendAvailable = backend_.isBusOpen();

    const auto now = std::chrono::steady_clock::now();
    const auto stallThreshold =
        std::chrono::seconds(HEARTBEAT_INTERVAL_SEC * HEARTBEAT_STALL_FACTOR);

    for (auto& [tok, conn] : connections_) {
        ConnectionSnapshot cs;
        cs.token = tok;
        cs.deviceId = conn.deviceId;
        cs.deviceName = conn.deviceName;
        cs.clientIP = conn.clientIP;
        cs.connectedAtEpoch =
            std::chrono::duration_cast<std::chrono::seconds>(conn.connectedAt.time_since_epoch())
                .count();
        cs.activeControllerCount = conn.activeControllerCount;
        cs.touchpadMode = conn.touchpadMode;
        // An existing connection is at least Active; escalates to NotResponding
        // once the stalling window lapses without packets.
        cs.linkState = (now - conn.lastPacketTime > stallThreshold) ? DeviceLinkState::NotResponding
                                                                    : DeviceLinkState::Active;

        for (auto& ctrl : conn.controllers) {
            if (ctrl.active) {
                ConnectionSnapshot::CtrlInfo info{};
                info.index = ctrl.index;
                info.serial = ctrl.serialNo;
                info.active = true;
                info.controllerType = ctrl.controllerType;
                info.batteryKnown = ctrl.lastBatteryValid;
                info.batteryLevel = ctrl.lastBattery.level;
                info.batteryStatus = ctrl.lastBattery.status;
                info.motionCapable = ctrl.motionCapable();
                info.motionActive = ctrl.lastMotionValid;
                info.motionSink = ctrl.motionSinkActive;
                info.motionSinkSupportedForType =
                    backend_.supportsMotionForType(ctrl.controllerType);
                info.motionBackendOk = backend_.motionBackendOk(ctrl.serialNo);
                info.touchpadActive = ctrl.lastTouchpadValid;
                info.lightbarCapable = ctrl.lightbarCapable();
                info.lightbarKnown = ctrl.lastLightbarValid;
                info.lightbarR = ctrl.lightbarR;
                info.lightbarG = ctrl.lightbarG;
                info.lightbarB = ctrl.lightbarB;
                cs.controllers.push_back(info);
                snap.totalControllers++;
            }
        }
        snap.connections.push_back(std::move(cs));
    }
    return snap;
}

bool SessionService::isDeviceConnected(const std::string& deviceId) const {
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& [tok, c] : connections_) {
        if (c.deviceId == deviceId) return true;
    }
    return false;
}

DeviceLinkState SessionService::linkStateForDevice(const std::string& deviceId) const {
    std::lock_guard<std::mutex> lk(mtx_);
    const auto now = std::chrono::steady_clock::now();
    const auto stallThreshold =
        std::chrono::seconds(HEARTBEAT_INTERVAL_SEC * HEARTBEAT_STALL_FACTOR);
    for (auto& [tok, c] : connections_) {
        if (c.deviceId != deviceId) continue;
        return (now - c.lastPacketTime > stallThreshold) ? DeviceLinkState::NotResponding
                                                         : DeviceLinkState::Active;
    }
    // No live connection; caller already knows the device is paired, so Paired.
    return DeviceLinkState::Paired;
}

int SessionService::reapTimedOut() {
    std::lock_guard<std::mutex> lk(mtx_);
    auto now = std::chrono::steady_clock::now();
    auto timeout = std::chrono::seconds(HEARTBEAT_INTERVAL_SEC * HEARTBEAT_MISS_MAX);

    int reaped = 0;
    for (auto it = connections_.begin(); it != connections_.end();) {
        if (now - it->second.lastPacketTime > timeout) {
            log_.logMsg(LogLevel::INFO, "service",
                        "Reaper: timed out connection for " + it->second.deviceName);
            teardownConnection(it->second);
            it = connections_.erase(it);
            reaped++;
        } else {
            ++it;
        }
    }
    if (reaped > 0) closeBackendBusIfIdle();
    return reaped;
}

bool SessionService::isBackendAvailable() const { return backend_.isBusOpen(); }

int SessionService::totalActiveControllers() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return countGlobalActiveControllers();
}

int SessionService::availableSlots() const {
    std::lock_guard<std::mutex> lk(mtx_);
    int slots =
        static_cast<int>(std::count(std::begin(serialInUse_), std::end(serialInUse_), false));
    return slots;
}
