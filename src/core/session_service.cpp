// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * core/session_service.cpp — Domain service implementation.
 */
#include "session_service.h"
#include <algorithm>
#include <cstring>

// We need generateToken — but it uses libsodium.
// To keep the core free of libsodium, we use a simple random approach.
#include <random>

static uint32_t makeRandomToken() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist(1, UINT32_MAX);
    return dist(gen);
}

SessionService::SessionService(IGamepadPort& backend, IClientPort& client, ILogPort& log)
    : backend_(backend), client_(client), log_(log) {
    // Wire the rumble return-path the moment we own the backend. The lambda
    // captures `this` — safe because the SessionService outlives the adapter
    // (composition root tears down adapters last) and the adapter joins all
    // notification workers in its destructor before the callback could fire.
    backend_.setRumbleCallback(
        [this](uint32_t serial, const RumbleReport& r) { handleRumbleFromBackend(serial, r); });
    // Independent lightbar callback (Task 1.4) — decouples colour changes
    // from rumble so a game that only writes lightbar still drives the LED.
    backend_.setLightbarCallback([this](uint32_t serial, uint8_t r, uint8_t g, uint8_t b) {
        handleLightbarFromBackend(serial, r, g, b);
    });
}

// ── Internal helpers (caller must hold mtx_) ────────────────────────────────

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
            return static_cast<uint32_t>(i + 1); // serials are 1-based
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

// ── Connection lifecycle ────────────────────────────────────────────────────

OpenSessionResult SessionService::openSession(const std::string& deviceId,
                                              const std::string& deviceName,
                                              const std::string& clientIP,
                                              const uint8_t sharedKey[CRYPTO_KEY_SIZE],
                                              uint8_t touchpadMode) {
    std::lock_guard<std::mutex> lk(mtx_);

    // Tear down any stale connection for this device
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
    std::memcpy(conn.sharedKey, sharedKey, CRYPTO_KEY_SIZE);
    conn.lastCounter = 0;
    conn.lastPacketTime = std::chrono::steady_clock::now();
    conn.connectedAt = std::chrono::steady_clock::now();
    conn.activeControllerCount = 0;
    conn.touchpadMode = (touchpadMode < TOUCHPAD_MODE_COUNT) ? touchpadMode : TOUCHPAD_MODE_DS4;

    connections_[token] = conn;

    // Count available slots
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

// ── Packet handling ─────────────────────────────────────────────────────────

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

void SessionService::handleControllerAdd(uint32_t token, uint8_t ctrlIdx, uint16_t caps) {
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

    // Lazy-open the platform backend bus
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
    ctrl.lastReport = GamepadReport{};
    // Clear rumble + lightbar coalesce state — even though the serial may be
    // recycled, the (re)added controller is a fresh actuator from the dish's
    // point of view, so the next non-zero rumble / next colour must reach it
    // without being suppressed as "same as the last one we already sent".
    ctrl.lastRumble = RumbleReport{};
    ctrl.lastRumbleValid = false;
    ctrl.lightbarR = 0;
    ctrl.lightbarG = 0;
    ctrl.lightbarB = 0;
    ctrl.lastLightbarValid = false;
    // Clear the cached sender→satellite streams too. The Controller struct
    // persists in the connection's array across a remove/re-add of the same
    // slot, so a stale lastMotion/lastBattery/lastTouchpad would otherwise
    // show phantom "active" state in /api/connections and — for
    // TOUCHPAD_MODE_MOUSE — make the first post-readd sample compute a delta
    // against a pre-readd finger position, jumping the cursor. The touchpad
    // sub-pixel remainder is reset for the same reason.
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

    client_.sendControllerAck(conn, MSG_CONTROLLER_ADD, ctrlIdx, ACK_OK);
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

    // If switching between DS4 and non-DS4, replug the virtual device
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
        }
    } else {
        log_.logMsg(LogLevel::INFO, "service",
                    "Controller #" + std::to_string(ctrlIdx) + " type set to " +
                        controllerTypeLabel(safeType) + " (" + conn.deviceName + ")");
    }

    // Broadcast updated state so web UI refreshes
    broadcastStatus();
}

void SessionService::handleRumbleFromBackend(uint32_t serial, const RumbleReport& report,
                                             uint16_t wireDurationMs) {
    // Find the (connection, ctrlIdx) that owns `serial`. Connections is small
    // (one per paired client, currently O(1) in practice) and each connection
    // has at most MAX_CONTROLLERS_PER_CONN slots — fine to scan linearly. The
    // alternative (a serial→(token,idx) reverse index) would have to stay
    // consistent with allocateSerial / releaseSerial; not worth the bug
    // surface for this hot-but-not-critical-path callback.
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
        // Stray notification — most commonly because the controller was just
        // unplugged but the worker thread fired one last queued event before
        // the backend cancelled its pending IOCTL. Drop silently.
        return;
    }

    // Coalesce identical back-to-back updates. Games often hold both motors at
    // the same magnitude across many frames; sending the same packet 250×/s
    // wastes bandwidth and starves the dish-side actuator queue. wireDurationMs
    // is deliberately excluded from the comparison so the caller can bump the
    // refresh deadline without forcing a packet.
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

    // Cache for web-UI debug pane regardless of whether the backend takes it.
    ctrl.lastMotion = report;
    ctrl.lastMotionValid = true;

    // The backend may or may not have an IMU surface; the default IGamepadPort
    // impl returns false. We deliberately don't treat that as an error — motion
    // is best-effort, and the cache still serves the web UI.
    //
    // We intentionally do NOT consult ctrl.motionCapable() (the CAP_MOTION
    // bit) here: the protocol says motion is best-effort and the receiver
    // MUST still accept MSG_MOTION from a dish that never advertised the
    // capability. CAP_MOTION is informational only — surfaced as
    // `motionCapable` in the web UI — and is not a gate on this path.
    //
    // The return value records whether the sample reached the virtual
    // device's IMU surface, surfaced as `motionSink` in the web UI so
    // operators can see "reaching the virtual pad" vs "cached only".
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

    // Capture the previous sample BEFORE overwriting the cache — relative-mouse
    // mode needs the finger-0 delta between consecutive frames.
    const TouchpadReport prev = ctrl.lastTouchpad;
    const bool prevValid = ctrl.lastTouchpadValid;

    // Cache for the web-UI debug pane regardless of routing mode / backend.
    ctrl.lastTouchpad = report;
    ctrl.lastTouchpadValid = true;

    switch (conn.touchpadMode) {
    case TOUCHPAD_MODE_OFF:
        // Routing disabled for this device — sample is cached, nothing else.
        return false;

    case TOUCHPAD_MODE_MOUSE: {
        // Finger 0 drives the cursor. A delta is emitted only while finger 0
        // is *continuously* down across both frames: a fresh touch-down
        // re-anchors the origin (so the cursor doesn't jump) and a lift emits
        // no motion. The clicky-pad button always maps to mouse button 1.
        //
        // The trackingId must also match: when one finger lifts the hardware
        // compacts the remaining contact down into slot 0 with a *new*
        // trackingId, so "finger0 active in both frames" can still be two
        // different physical fingers. Without the id check that compaction
        // teleports the cursor.
        int dx = 0;
        int dy = 0;
        const bool continuous = prevValid && prev.finger0.active && report.finger0.active &&
                                prev.finger0.trackingId == report.finger0.trackingId;
        if (continuous) {
            const float fx =
                static_cast<float>(report.finger0.x - prev.finger0.x) * TOUCHPAD_MOUSE_SENSITIVITY +
                ctrl.touchpadMouseRemX;
            const float fy =
                static_cast<float>(report.finger0.y - prev.finger0.y) * TOUCHPAD_MOUSE_SENSITIVITY +
                ctrl.touchpadMouseRemY;
            dx = static_cast<int>(fx);
            dy = static_cast<int>(fy);
            // Keep the sub-pixel remainder so a slow drag still moves.
            ctrl.touchpadMouseRemX = fx - static_cast<float>(dx);
            ctrl.touchpadMouseRemY = fy - static_cast<float>(dy);
        } else {
            // No continuous contact — drop any accumulated sub-pixel motion.
            ctrl.touchpadMouseRemX = 0.0f;
            ctrl.touchpadMouseRemY = 0.0f;
        }
        return backend_.submitRelativeMouse(dx, dy, report.buttonPressed);
    }

    case TOUCHPAD_MODE_DS4:
    default:
        // Pass through to the virtual DualShock 4 touchpad surface. The
        // default IGamepadPort impl returns false (Xbox pad / inert backend);
        // that is not an error — the sample is still cached above.
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
        // Reset every controller's relative-mouse accumulator so a mode switch
        // doesn't carry a stale sub-pixel remainder into the new mode.
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

    // Resolve serial → (connection, ctrlIdx) via the same scan the rumble
    // callback uses. O(connections × MAX_CONTROLLERS_PER_CONN) but bounded
    // by the global 16-controller cap; the cost is dominated by the
    // notification-thread context switch, not the scan.
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

    // Coalesce: don't re-process an unchanged colour. Same shape as the rumble
    // coalesce — saves wire when a game holds a constant colour across frames.
    if (foundCtrl->lastLightbarValid && foundCtrl->lightbarR == r && foundCtrl->lightbarG == g &&
        foundCtrl->lightbarB == b) {
        return;
    }
    foundCtrl->lightbarR = r;
    foundCtrl->lightbarG = g;
    foundCtrl->lightbarB = b;
    foundCtrl->lastLightbarValid = true;

    // Emit MSG_LIGHTBAR only to senders that advertised CAP_LIGHTBAR — a sender
    // with an addressable RGB LED. A sender without one (an Xbox pad, or
    // dish-android, which has no controller-LED API) would only drop the packet.
    // The colour is cached above regardless, so the web UI swatch stays live for
    // every controller type.
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

    // Reject obviously bogus values defensively. A 0xFF level is the documented
    // "unknown" sentinel and is allowed; anything 101..254 is malformed.
    if (report.level != BATTERY_LEVEL_UNKNOWN && report.level > 100) return false;
    if (report.status >= BATTERY_STATUS_COUNT) return false;

    ctrl.lastBattery = report;
    ctrl.lastBatteryValid = true;

    // Forward to the backend (Windows DS4 wires this to DS4_REPORT_EX battery
    // byte). Other backends drop silently via the default no-op.
    backend_.submitBattery(ctrl.serialNo, report);
    return true;
}

// ── Pre-decrypt helpers ─────────────────────────────────────────────────────

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
    client_.updateClientAddr(token, clientIP, clientPort);
}

// ── Query ───────────────────────────────────────────────────────────────────

SessionService::ConnectionsSnapshot SessionService::getConnectionsSnapshot() const {
    std::lock_guard<std::mutex> lk(mtx_);
    ConnectionsSnapshot snap;
    snap.totalControllers = 0;
    snap.maxControllers = MAX_BACKEND_CONTROLLERS;
    snap.backendAvailable = backend_.isBusOpen();

    // Threshold for DeviceLinkState::NotResponding — see HEARTBEAT_STALL_FACTOR.
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
        // A connection that exists at all is at least Active; escalate to
        // NotResponding once the stalling window has lapsed without packets.
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
        // Live connection — Active unless we've crossed the stalling window.
        return (now - c.lastPacketTime > stallThreshold) ? DeviceLinkState::NotResponding
                                                         : DeviceLinkState::Active;
    }
    // No live connection for this device. The caller has already established
    // the device IS paired (it came from `pairedDevices`); fall back to Paired.
    return DeviceLinkState::Paired;
}

// ── Reaper ──────────────────────────────────────────────────────────────────

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

// ── Stats ───────────────────────────────────────────────────────────────────

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
