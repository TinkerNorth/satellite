// SPDX-License-Identifier: LGPL-3.0-or-later

#include "session_service.h"

#include "ipv4_util.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

// std::random not libsodium, to keep the core libsodium-free. These values need
// uniqueness, not secrecy: session security rests on the pairing key feeding the
// injected KeyDeriver.
#include <random>

using satellite::formatIPv4Nbo;
using satellite::parseIPv4Nbo;

static uint32_t makeRandomToken() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist(1, UINT32_MAX);
    return dist(gen);
}

SessionService::SessionService(IGamepadPort& backend, IClientPort& client, ILogPort& log,
                               KeyDeriver keyDeriver)
    : backend_(backend), client_(client), log_(log), keyDeriver_(std::move(keyDeriver)) {
    // The `this`-capturing lambdas are safe: SessionService outlives the adapter,
    // and the adapter joins its notification workers before the callbacks fire.
    backend_.setRumbleCallback(
        [this](uint32_t serial, const RumbleReport& r) { handleRumbleFromBackend(serial, r); });
    backend_.setLightbarCallback([this](uint32_t serial, uint8_t r, uint8_t g, uint8_t b) {
        handleLightbarFromBackend(serial, r, g, b);
    });
}

// Internal helpers below assume the caller holds mtx_.

Connection* SessionService::findByDeviceId(const std::string& deviceId) {
    for (auto& [tok, conn] : connections_) {
        if (conn.deviceId == deviceId) return &conn;
    }
    return nullptr;
}

Connection* SessionService::findByConnectionId(const std::string& connectionId) {
    for (auto& [tok, conn] : connections_) {
        if (conn.connectionId == connectionId) return &conn;
    }
    return nullptr;
}

const Connection* SessionService::findByConnectionId(const std::string& connectionId) const {
    for (auto& [tok, conn] : connections_) {
        if (conn.connectionId == connectionId) return &conn;
    }
    return nullptr;
}

bool SessionService::unplugAndRelease(uint32_t serial) {
    if (backend_.unplugDevice(serial)) {
        releaseSerial(serial);
        return true;
    }
    quarantineSerial(serial);
    log_.logMsg(LogLevel::WARN, "service",
                "Unplug of serial " + std::to_string(serial) +
                    " unconfirmed; serial quarantined until the bus closes");
    return false;
}

void SessionService::teardownConnection(Connection& conn) {
    for (auto& ctrl : conn.controllers) {
        if (ctrl.active && ctrl.serialNo != 0) {
            unplugAndRelease(ctrl.serialNo);
            ctrl.active = false;
            ctrl.serialNo = 0;
        }
    }
    conn.activeControllerCount = 0;
    client_.removeClientAddr(conn.token);
}

uint32_t SessionService::allocateSerial() {
    // Round-robin scan start so a just-freed serial isn't re-plugged while its
    // async PnP removal may still be in flight.
    for (size_t step = 0; step < MAX_BACKEND_CONTROLLERS; step++) {
        size_t i = (serialScanStart_ + step) % MAX_BACKEND_CONTROLLERS;
        if (!serialInUse_[i] && !serialQuarantined_[i]) {
            serialInUse_[i] = true;
            serialScanStart_ = (i + 1) % MAX_BACKEND_CONTROLLERS;
            return static_cast<uint32_t>(i + 1); // 1-based
        }
    }
    return 0;
}

void SessionService::releaseSerial(uint32_t serial) {
    if (serial == 0 || serial > (uint32_t)MAX_BACKEND_CONTROLLERS) return;
    serialInUse_[serial - 1] = false;
}

void SessionService::quarantineSerial(uint32_t serial) {
    if (serial == 0 || serial > (uint32_t)MAX_BACKEND_CONTROLLERS) return;
    serialInUse_[serial - 1] = false;
    serialQuarantined_[serial - 1] = true;
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
        // A closed bus has no live targets, so quarantined serials are clean again.
        std::memset(serialQuarantined_, 0, sizeof(serialQuarantined_));
        log_.logMsg(LogLevel::INFO, "service", "Backend bus closed (no active controllers)");
    }
}

uint32_t SessionService::generateUniqueToken() {
    uint32_t token;
    do { token = makeRandomToken(); } while (connections_.count(token));
    return token;
}

void SessionService::deriveSessionKeyLocked(Connection& conn,
                                            const uint8_t pairingKey[CRYPTO_KEY_SIZE]) {
    if (keyDeriver_) {
        keyDeriver_(pairingKey, conn.sessionSalt, conn.token, conn.sessionKey);
    } else {
        std::memcpy(conn.sessionKey, pairingKey, CRYPTO_KEY_SIZE);
    }
}

void SessionService::resetControllerStreamState(Controller& ctrl) {
    ctrl.lastReport = GamepadReport{};
    // A (re)plugged controller is a fresh actuator, so the next rumble/colour
    // must not be suppressed as "same as last".
    ctrl.lastRumble = RumbleReport{};
    ctrl.lastRumbleValid = false;
    ctrl.lightbarR = 0;
    ctrl.lightbarG = 0;
    ctrl.lightbarB = 0;
    ctrl.lastLightbarValid = false;
    // Controller structs persist across remove/re-add, so stale samples would
    // show phantom "active" state and a MOUSE first sample would delta against a
    // pre-readd finger (cursor jump).
    ctrl.lastMotion = MotionReport{};
    ctrl.lastMotionValid = false;
    ctrl.motionSinkActive = false;
    ctrl.lastBattery = BatteryReport{};
    ctrl.lastBatteryValid = false;
    ctrl.lastTouchpad = TouchpadReport{};
    ctrl.lastTouchpadValid = false;
    ctrl.touchpadMouseRemX = 0.0f;
    ctrl.touchpadMouseRemY = 0.0f;
}

void SessionService::removeControllerLocked(Connection& conn, Controller& ctrl) {
    if (!ctrl.active) return;
    log_.logMsg(LogLevel::INFO, "service",
                "Controller #" + std::to_string(ctrl.index) + " removed from " + conn.deviceName);
    if (ctrl.serialNo != 0) unplugAndRelease(ctrl.serialNo);
    ctrl.active = false;
    ctrl.serialNo = 0;
    conn.activeControllerCount--;
    conn.epoch++;
}

uint16_t SessionService::activeBitmapLocked(const Connection& conn) const {
    uint16_t bitmap = 0;
    for (size_t i = 0; i < MAX_CONTROLLERS_PER_CONN; i++) {
        if (conn.controllers[i].active) bitmap |= static_cast<uint16_t>(1u << i);
    }
    return bitmap;
}

static void fillMotionFlags(IGamepadPort& backend, const Controller& ctrl,
                            ControllerApplyResult& out) {
    if (!ctrl.active) {
        out.motionSinkSupportedForType = false;
        out.motionBackendOk = false;
        return;
    }
    out.motionSinkSupportedForType = backend.supportsMotionForType(ctrl.controllerType);
    out.motionBackendOk = backend.motionBackendOk(ctrl.serialNo);
}

void SessionService::applyDescriptorLocked(Connection& conn, const ControllerDescriptor& desc,
                                           ControllerApplyResult& out) {
    out = ControllerApplyResult{};
    out.ctrlIdx = desc.ctrlIdx;

    if (desc.ctrlIdx >= MAX_CONTROLLERS_PER_CONN) {
        out.result = APPLY_ERR_INVALID_INDEX;
        return;
    }
    Controller& ctrl = conn.controllers[desc.ctrlIdx];
    out.appliedType = ctrl.active ? ctrl.controllerType : desc.type;

    if (desc.type >= CONTROLLER_TYPE_COUNT ||
        !backend_.supportsIdentity(controllerIdentity(desc.type))) {
        // Unknown id, or a valid type this backend can't materialize (e.g. a
        // DualSense on ViGEm): reject per-controller, leave any live pad intact.
        out.result = APPLY_ERR_INVALID_TYPE;
        out.appliedType = ctrl.active ? ctrl.controllerType : CONTROLLER_TYPE_XBOX;
        fillMotionFlags(backend_, ctrl, out);
        return;
    }

    const uint8_t safeMode =
        (desc.touchpadMode < TOUCHPAD_MODE_COUNT) ? desc.touchpadMode : TOUCHPAD_MODE_OFF;
    const GamepadIdentity wantId = controllerIdentity(desc.type);

    if (!ctrl.active) {
        if (!backend_.isBusOpen()) {
            if (backend_.ensureBusOpen()) {
                // Reopening from idle means every old target is gone, so
                // quarantined serials are clean again.
                std::memset(serialQuarantined_, 0, sizeof(serialQuarantined_));
                log_.logMsg(LogLevel::INFO, "service", "Backend bus opened on demand");
            }
        }
        if (!backend_.isBusOpen()) {
            log_.logMsg(LogLevel::ERR, "service",
                        "Controller apply failed: backend bus unavailable");
            out.result = APPLY_ERR_BACKEND_UNAVAIL;
            return;
        }

        uint32_t serial = allocateSerial();
        if (serial == 0) {
            out.result = APPLY_ERR_NO_SLOTS;
            return;
        }
        bool plugOk = backend_.pluginDevice(serial, wantId);
        if (!plugOk) {
            // The plug never created a target, so the serial is clean to reuse.
            releaseSerial(serial);
            out.result = APPLY_ERR_PLUGIN_FAIL;
            return;
        }

        ctrl.index = desc.ctrlIdx;
        ctrl.serialNo = serial;
        ctrl.active = true;
        ctrl.controllerType = desc.type;
        ctrl.identity = wantId;
        ctrl.caps = desc.caps;
        ctrl.touchpadMode = safeMode;
        resetControllerStreamState(ctrl);
        conn.activeControllerCount++;
        conn.epoch++;

        log_.logMsg(LogLevel::INFO, "service",
                    "Controller #" + std::to_string(desc.ctrlIdx) + " plugged as " +
                        controllerTypeLabel(desc.type) + " (serial " + std::to_string(serial) +
                        ") for " + conn.deviceName);
        out.appliedType = desc.type;
        fillMotionFlags(backend_, ctrl, out);
        return;
    }

    // Existing active slot.
    if (wantId != ctrl.identity) {
        if (!backend_.isBusOpen()) {
            // Bus died under a live slot; nothing to converge onto.
            conn.epoch++;
            out.result = APPLY_ERR_REPLUG_FAIL;
            out.appliedType = ctrl.controllerType;
            fillMotionFlags(backend_, ctrl, out);
            return;
        }

        uint32_t fresh = allocateSerial();
        if (fresh != 0) {
            // Transactional replug: plug the NEW target on a FRESH serial, only
            // then retire the old one. A plug failure leaves the old pad untouched.
            bool plugOk = backend_.pluginDevice(fresh, wantId);
            if (!plugOk) {
                releaseSerial(fresh);
                // Bump the epoch anyway so a client whose PUT response was lost
                // still reconciles instead of believing the switch landed.
                conn.epoch++;
                out.result = APPLY_ERR_REPLUG_FAIL;
                out.appliedType = ctrl.controllerType;
                fillMotionFlags(backend_, ctrl, out);
                log_.logMsg(LogLevel::ERR, "service",
                            "Failed to replug controller #" + std::to_string(desc.ctrlIdx) +
                                " as " + controllerTypeLabel(desc.type) + "; keeping existing " +
                                controllerTypeLabel(ctrl.controllerType));
                return;
            }
            unplugAndRelease(ctrl.serialNo);
            ctrl.serialNo = fresh;
        } else {
            // 16/16 serials in use: fall back to unplug-first on the same serial.
            uint32_t serial = ctrl.serialNo;
            if (!unplugAndRelease(serial)) {
                // Old target unconfirmed-dead and quarantined; the slot is lost.
                ctrl.active = false;
                ctrl.serialNo = 0;
                conn.activeControllerCount--;
                conn.epoch++;
                out.result = APPLY_ERR_PLUGIN_FAIL;
                out.appliedType = desc.type;
                log_.logMsg(LogLevel::ERR, "service",
                            "Replug fallback failed to unplug controller #" +
                                std::to_string(desc.ctrlIdx) + "; slot lost");
                return;
            }
            serialInUse_[serial - 1] = true; // reclaim the slot we just released
            bool plugOk = backend_.pluginDevice(serial, wantId);
            if (!plugOk) {
                releaseSerial(serial);
                ctrl.active = false;
                ctrl.serialNo = 0;
                conn.activeControllerCount--;
                conn.epoch++;
                out.result = APPLY_ERR_PLUGIN_FAIL;
                out.appliedType = desc.type;
                log_.logMsg(LogLevel::ERR, "service",
                            "Replug fallback plug failed for controller #" +
                                std::to_string(desc.ctrlIdx) + "; slot lost");
                return;
            }
        }

        ctrl.controllerType = desc.type;
        ctrl.identity = wantId;
        ctrl.caps = desc.caps;
        ctrl.touchpadMode = safeMode;
        resetControllerStreamState(ctrl);
        conn.epoch++;
        out.appliedType = desc.type;
        fillMotionFlags(backend_, ctrl, out);
        log_.logMsg(LogLevel::INFO, "service",
                    "Replugged controller #" + std::to_string(desc.ctrlIdx) + " as " +
                        controllerTypeLabel(desc.type) + " (serial " +
                        std::to_string(ctrl.serialNo) + ")");
        return;
    }

    // Same identity: converge caps/mode/type in place. No replug, no epoch bump
    // (the applied topology is unchanged).
    ctrl.controllerType = desc.type;
    ctrl.caps = desc.caps;
    ctrl.touchpadMode = safeMode;
    out.appliedType = desc.type;
    fillMotionFlags(backend_, ctrl, out);
}

SessionUpsertResult SessionService::upsertSession(
    const std::string& deviceId, const std::string& deviceName, const std::string& clientIP,
    const uint8_t pairingKey[CRYPTO_KEY_SIZE], const std::vector<ControllerDescriptor>& descriptors,
    bool requestMouseControl) {
    std::lock_guard<std::mutex> lk(mtx_);
    SessionUpsertResult res;

    // Dedup on ctrlIdx, last write wins, preserving first-seen order so the
    // converge below is deterministic for a malformed duplicate-idx body.
    std::vector<ControllerDescriptor> deduped;
    for (const auto& d : descriptors) {
        bool replaced = false;
        for (auto& existing : deduped) {
            if (existing.ctrlIdx == d.ctrlIdx) {
                existing = d;
                replaced = true;
                break;
            }
        }
        if (!replaced) deduped.push_back(d);
    }

    const auto now = std::chrono::steady_clock::now();
    Connection* conn = findByDeviceId(deviceId);
    if (conn == nullptr) {
        Connection c;
        char idHex[9];
        uint32_t idRand;
        do {
            idRand = makeRandomToken();
            snprintf(idHex, sizeof(idHex), "%08x", idRand);
        } while (findByConnectionId(std::string("conn_") + idHex) != nullptr);
        c.connectionId = std::string("conn_") + idHex;
        c.deviceId = deviceId;
        c.connectedAt = now;
        c.token = generateUniqueToken();
        uint32_t token = c.token;
        connections_[token] = std::move(c);
        conn = &connections_[token];
        log_.logMsg(LogLevel::INFO, "service",
                    "Session created for " + deviceName + " (" + conn->connectionId + ")");
    } else {
        // Same device re-PUTting: tell the old token best-effort, then rotate
        // token/key in place. The row and its pads survive, no unplug/replug churn.
        client_.sendSessionClose(*conn, CLOSE_REASON_REPLACED);
        client_.removeClientAddr(conn->token);
        uint32_t newToken = generateUniqueToken();
        auto node = connections_.extract(conn->token);
        node.key() = newToken;
        node.mapped().token = newToken;
        auto ins = connections_.insert(std::move(node));
        conn = &ins.position->second;
        log_.logMsg(LogLevel::INFO, "service",
                    "Session rotated for " + deviceName + " (" + conn->connectionId + ")");
    }

    conn->deviceName = deviceName;
    conn->clientIP = clientIP;
    // Seed the numeric IPv4 cache for the first packet's "address changed?"
    // check. Failure (0) is non-fatal; the next V4 update fills it.
    conn->clientIPv4 = parseIPv4Nbo(clientIP);
    for (size_t i = 0; i < SESSION_SALT_SIZE; i += 4) {
        uint32_t r = makeRandomToken();
        std::memcpy(conn->sessionSalt + i, &r, 4);
    }
    deriveSessionKeyLocked(*conn, pairingKey);
    conn->lastCounter = 0;
    conn->lastPacketTime = now;
    conn->graceUntil = now + std::chrono::seconds(REST_LIVENESS_GRACE_SEC);

    const bool mouseSupported = backend_.supportsRelativeMouse();
    conn->mouseControlGranted = requestMouseControl && mouseSupported;
    res.mouseControlGranted = conn->mouseControlGranted;
    if (requestMouseControl && !mouseSupported) {
        res.mouseControlDenyReason = HOST_FEATURE_DENY_NOT_SUPPORTED;
    }

    // Converge to the desired set: retire absent slots first (freeing serials
    // for newcomers), then apply each descriptor.
    bool desired[MAX_CONTROLLERS_PER_CONN] = {};
    for (const auto& d : deduped) {
        if (d.ctrlIdx < MAX_CONTROLLERS_PER_CONN) desired[d.ctrlIdx] = true;
    }
    for (size_t i = 0; i < MAX_CONTROLLERS_PER_CONN; i++) {
        if (conn->controllers[i].active && !desired[i]) {
            removeControllerLocked(*conn, conn->controllers[i]);
        }
    }
    for (const auto& d : deduped) {
        ControllerApplyResult ar;
        applyDescriptorLocked(*conn, d, ar);
        res.controllers.push_back(ar);
    }
    closeBackendBusIfIdle();

    res.ok = true;
    res.connectionId = conn->connectionId;
    res.token = conn->token;
    std::memcpy(res.sessionSalt, conn->sessionSalt, SESSION_SALT_SIZE);
    res.epoch = conn->epoch;
    res.maxControllers = MAX_BACKEND_CONTROLLERS;
    return res;
}

bool SessionService::applyController(const std::string& connectionId, const std::string& deviceId,
                                     const ControllerDescriptor& desc,
                                     ControllerApplyResult& outResult, uint16_t& outEpoch) {
    std::lock_guard<std::mutex> lk(mtx_);
    Connection* conn = findByConnectionId(connectionId);
    if (conn == nullptr || conn->deviceId != deviceId) return false;
    applyDescriptorLocked(*conn, desc, outResult);
    closeBackendBusIfIdle();
    outEpoch = conn->epoch;
    return true;
}

bool SessionService::removeController(const std::string& connectionId, const std::string& deviceId,
                                      uint8_t ctrlIdx, uint16_t& outEpoch) {
    std::lock_guard<std::mutex> lk(mtx_);
    Connection* conn = findByConnectionId(connectionId);
    if (conn == nullptr || conn->deviceId != deviceId) return false;
    if (ctrlIdx < MAX_CONTROLLERS_PER_CONN) {
        removeControllerLocked(*conn, conn->controllers[ctrlIdx]);
        closeBackendBusIfIdle();
    }
    outEpoch = conn->epoch;
    return true;
}

SessionService::SessionView SessionService::getSessionView(const std::string& connectionId,
                                                           const std::string& deviceId) const {
    std::lock_guard<std::mutex> lk(mtx_);
    SessionView view;
    const Connection* conn = findByConnectionId(connectionId);
    if (conn == nullptr) return view;
    if (!deviceId.empty() && conn->deviceId != deviceId) return view;
    view.found = true;
    view.connectionId = conn->connectionId;
    view.deviceId = conn->deviceId;
    view.epoch = conn->epoch;
    view.mouseControlGranted = conn->mouseControlGranted;
    for (const auto& ctrl : conn->controllers) {
        if (!ctrl.active) continue;
        SessionView::CtrlView cv;
        cv.ctrlIdx = ctrl.index;
        cv.appliedType = ctrl.controllerType;
        cv.caps = ctrl.caps;
        cv.touchpadMode = ctrl.touchpadMode;
        cv.motionSinkSupportedForType = backend_.supportsMotionForType(ctrl.controllerType);
        cv.motionBackendOk = backend_.motionBackendOk(ctrl.serialNo);
        view.controllers.push_back(cv);
    }
    return view;
}

int SessionService::closeSessionById(const std::string& connectionId, const std::string& deviceId,
                                     uint8_t reason, bool notify) {
    std::lock_guard<std::mutex> lk(mtx_);
    Connection* conn = findByConnectionId(connectionId);
    if (conn == nullptr) return -1;
    if (!deviceId.empty() && conn->deviceId != deviceId) return -1;

    if (notify) client_.sendSessionClose(*conn, reason);
    int removed = conn->activeControllerCount;
    std::string devName = conn->deviceName;
    uint32_t token = conn->token;
    teardownConnection(*conn);
    connections_.erase(token);
    closeBackendBusIfIdle();

    log_.logMsg(LogLevel::INFO, "service",
                "Connection closed for " + devName + " (" + std::to_string(removed) +
                    " controllers removed, " + closeReasonName(reason) + ")");
    return removed;
}

int SessionService::closeSessionsForDevice(const std::string& deviceId, uint8_t reason) {
    std::lock_guard<std::mutex> lk(mtx_);
    int closed = 0;
    for (auto it = connections_.begin(); it != connections_.end();) {
        if (it->second.deviceId == deviceId) {
            client_.sendSessionClose(it->second, reason);
            log_.logMsg(LogLevel::INFO, "service",
                        "Connection closed for " + it->second.deviceName + " (" +
                            closeReasonName(reason) + ")");
            teardownConnection(it->second);
            it = connections_.erase(it);
            closed++;
        } else {
            ++it;
        }
    }
    if (closed > 0) closeBackendBusIfIdle();
    return closed;
}

void SessionService::closeAllSessions(uint8_t reason) {
    std::lock_guard<std::mutex> lk(mtx_);
    // Notify everyone BEFORE teardown so the notify rides the still-valid
    // session keys and addresses.
    for (auto& [tok, conn] : connections_) { client_.sendSessionClose(conn, reason); }
    for (auto& [tok, conn] : connections_) { teardownConnection(conn); }
    connections_.clear();
    std::memset(serialInUse_, 0, sizeof(serialInUse_));
    std::memset(serialQuarantined_, 0, sizeof(serialQuarantined_));
    closeBackendBusIfIdle();
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
    return backend_.submitReport(ctrl.serialNo, report);
}

void SessionService::handleHeartbeat(uint32_t token) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = connections_.find(token);
    if (it == connections_.end()) return;

    const Connection& conn = it->second;
    client_.sendHeartbeatAck(conn, backend_.isBusOpen(), (uint8_t)countGlobalActiveControllers(),
                             conn.epoch, activeBitmapLocked(conn));
}

void SessionService::handleRumbleFromBackend(uint32_t serial, const RumbleReport& report,
                                             uint16_t wireDurationMs) {
    // try_to_lock, NEVER block: unplug paths join this notification worker while
    // holding mtx_, so a blocking acquire here deadlocks. A dropped frame
    // self-heals (rumble is coalesced and re-notified).
    std::unique_lock<std::mutex> lk(mtx_, std::try_to_lock);
    if (!lk.owns_lock()) return;

    // Linear scan, bounded by the 16-controller cap; not worth a reverse index
    // that must track allocateSerial/releaseSerial.
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
    // Stray event from a just-unplugged controller's worker.
    if (!foundConn || !foundCtrl) return;

    // Coalesce identical back-to-back updates (games hold the motors steady
    // across many frames). wireDurationMs is excluded so the caller can bump the
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

    // Cache for the web-UI debug pane regardless of backend acceptance.
    ctrl.lastMotion = report;
    ctrl.lastMotionValid = true;

    // Deliberately do NOT gate on ctrl.motionCapable() (CAP_MOTION): the
    // protocol says motion is best-effort and the receiver MUST accept
    // MSG_MOTION even from a dish that never advertised the cap. A backend
    // without an IMU surface returning false is not an error.
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

    // Capture the previous sample BEFORE overwriting: relative-mouse mode needs
    // the finger-0 delta between consecutive frames.
    const TouchpadReport prev = ctrl.lastTouchpad;
    const bool prevValid = ctrl.lastTouchpadValid;

    // Cache for the web-UI debug pane regardless of routing mode / backend.
    ctrl.lastTouchpad = report;
    ctrl.lastTouchpadValid = true;

    switch (ctrl.touchpadMode) {
    case TOUCHPAD_MODE_OFF:
        return false; // cached above, nothing else

    case TOUCHPAD_MODE_MOUSE: {
        // Host-input streams are only valid for session-granted features;
        // ungranted streams are dropped (cached above for the debug pane).
        if (!conn.mouseControlGranted) return false;

        // Delta only while finger 0 is down across both frames AND the
        // trackingId matches: a lifted finger compacts the survivor into slot 0
        // with a new id, and without the check the cursor teleports. Scale by
        // REFERENCE_MS/dt so velocity is dt-independent (Android's first MOVE
        // lands ~16 ms late, otherwise a visible first-touch jump).
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
        // A false from the default impl (Xbox pad / inert backend) is not an
        // error; cached above.
        return backend_.submitTouchpad(ctrl.serialNo, report);
    }
}

void SessionService::handleLightbarFromBackend(uint32_t serial, uint8_t r, uint8_t g, uint8_t b) {
    // try_to_lock for the same reason as handleRumbleFromBackend: unplug joins
    // this worker while holding mtx_; blocking here deadlocks.
    std::unique_lock<std::mutex> lk(mtx_, std::try_to_lock);
    if (!lk.owns_lock()) return;

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

    // Emit only to senders that advertised CAP_LIGHTBAR; others would drop it.
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
    std::memcpy(outKey, it->second.sessionKey, CRYPTO_KEY_SIZE);
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
    // ONE mtx_ acquisition for the whole packet (see header: tail latency).
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
        cs.connectionId = conn.connectionId;
        cs.token = tok;
        cs.deviceId = conn.deviceId;
        cs.deviceName = conn.deviceName;
        cs.clientIP = conn.clientIP;
        cs.connectedAtEpoch =
            std::chrono::duration_cast<std::chrono::seconds>(conn.connectedAt.time_since_epoch())
                .count();
        cs.epoch = conn.epoch;
        cs.activeControllerCount = conn.activeControllerCount;
        cs.mouseControlGranted = conn.mouseControlGranted;
        // The REST-open grace window counts as liveness so a fresh PUT doesn't
        // flash "not responding".
        const bool inGrace = now < conn.graceUntil;
        cs.linkState = (!inGrace && now - conn.lastPacketTime > stallThreshold)
                           ? DeviceLinkState::NotResponding
                           : DeviceLinkState::Active;

        for (auto& ctrl : conn.controllers) {
            if (ctrl.active) {
                ConnectionSnapshot::CtrlInfo info{};
                info.index = ctrl.index;
                info.serial = ctrl.serialNo;
                info.active = true;
                info.pluggedIn = backend_.isDevicePlugged(ctrl.serialNo);
                info.controllerType = ctrl.controllerType;
                info.touchpadMode = ctrl.touchpadMode;
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
        const bool inGrace = now < c.graceUntil;
        return (!inGrace && now - c.lastPacketTime > stallThreshold)
                   ? DeviceLinkState::NotResponding
                   : DeviceLinkState::Active;
    }
    return DeviceLinkState::Paired;
}

int SessionService::reapTimedOut() {
    std::lock_guard<std::mutex> lk(mtx_);
    auto now = std::chrono::steady_clock::now();
    auto timeout = std::chrono::seconds(HEARTBEAT_INTERVAL_SEC * HEARTBEAT_MISS_MAX);

    int reaped = 0;
    for (auto it = connections_.begin(); it != connections_.end();) {
        // The REST upsert counts as provisional liveness until graceUntil, so a
        // half-open session (UDP blocked) surfaces client-side instead of
        // flapping through here.
        if (now - it->second.lastPacketTime > timeout && now > it->second.graceUntil) {
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

#ifdef SATELLITE_BUILD_TESTS
void SessionService::backdateForTest(uint32_t token, int lastPacketSecondsAgo,
                                     int graceSecondsAgo) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = connections_.find(token);
    if (it == connections_.end()) return;
    const auto now = std::chrono::steady_clock::now();
    it->second.lastPacketTime = now - std::chrono::seconds(lastPacketSecondsAgo);
    it->second.graceUntil = now - std::chrono::seconds(graceSecondsAgo);
}
#endif

int SessionService::totalActiveControllers() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return countGlobalActiveControllers();
}

int SessionService::availableSlots() const {
    std::lock_guard<std::mutex> lk(mtx_);
    int slots = 0;
    for (size_t i = 0; i < MAX_BACKEND_CONTROLLERS; i++) {
        if (!serialInUse_[i] && !serialQuarantined_[i]) slots++;
    }
    return slots;
}
