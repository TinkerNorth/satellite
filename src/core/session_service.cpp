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

SessionService::SessionService(IViGemPort& vigem, IClientPort& client, ILogPort& log)
    : vigem_(vigem), client_(client), log_(log) {}

// ── Internal helpers (caller must hold mtx_) ────────────────────────────────

void SessionService::teardownConnection(Connection& conn) {
    for (auto& ctrl : conn.controllers) {
        if (ctrl.active && ctrl.serialNo != 0) {
            vigem_.unplugDevice(ctrl.serialNo);
            releaseSerial(ctrl.serialNo);
            ctrl.active = false;
            ctrl.serialNo = 0;
        }
    }
    conn.activeControllerCount = 0;
    client_.removeClientAddr(conn.token);
}

uint32_t SessionService::allocateSerial() {
    for (size_t i = 0; i < MAX_VIGEM_CONTROLLERS; i++) {
        if (!serialInUse_[i]) {
            serialInUse_[i] = true;
            return static_cast<uint32_t>(i + 1); // serials are 1-based
        }
    }
    return 0;
}

void SessionService::releaseSerial(uint32_t serial) {
    if (serial == 0 || serial > (uint32_t)MAX_VIGEM_CONTROLLERS) return;
    serialInUse_[serial - 1] = false;
}

int SessionService::countGlobalActiveControllers() const {
    int total = 0;
    for (auto& [tok, c] : connections_) { total += c.activeControllerCount; }
    return total;
}

void SessionService::closeVigemBusIfIdle() {
    if (!vigem_.isBusOpen()) return;
    if (countGlobalActiveControllers() == 0) {
        vigem_.closeBus();
        log_.logMsg(LogLevel::INFO, "service", "ViGEm bus closed (no active controllers)");
    }
}

void SessionService::broadcastStatus() {
    std::vector<std::pair<uint32_t, const Connection*>> conns;
    for (auto& [tok, c] : connections_) { conns.push_back({tok, &c}); }
    client_.broadcastServerStatus(conns, vigem_.isBusOpen(),
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
                                              const uint8_t sharedKey[CRYPTO_KEY_SIZE]) {
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
    closeVigemBusIfIdle();

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

    connections_[token] = conn;

    // Count available slots
    int slots = static_cast<int>(
        std::count(std::begin(serialInUse_), std::end(serialInUse_), false));

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
    closeVigemBusIfIdle();

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
    return vigem_.submitReport(ctrl.serialNo, report);
}

void SessionService::handleHeartbeat(uint32_t token) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = connections_.find(token);
    if (it == connections_.end()) return;

    const Connection& conn = it->second;
    client_.sendHeartbeatAck(conn);
    client_.sendServerStatus(conn, vigem_.isBusOpen(), (uint8_t)countGlobalActiveControllers());
}

void SessionService::handleControllerAdd(uint32_t token, uint8_t ctrlIdx) {
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

    // Lazy-open ViGEm bus
    if (!vigem_.isBusOpen()) {
        if (vigem_.ensureBusOpen()) {
            log_.logMsg(LogLevel::INFO, "service", "ViGEm bus opened on demand");
            broadcastStatus();
        }
    }
    if (!vigem_.isBusOpen()) {
        log_.logMsg(LogLevel::ERR, "service", "Controller add failed: ViGEm bus unavailable");
        client_.sendControllerAck(conn, MSG_CONTROLLER_ADD, ctrlIdx, ACK_ERR_VIGEM_UNAVAIL);
        return;
    }

    uint32_t serial = allocateSerial();
    if (serial == 0) {
        client_.sendControllerAck(conn, MSG_CONTROLLER_ADD, ctrlIdx, ACK_ERR_NO_SLOTS);
        return;
    }

    if (!vigem_.pluginDevice(serial)) {
        releaseSerial(serial);
        client_.sendControllerAck(conn, MSG_CONTROLLER_ADD, ctrlIdx, ACK_ERR_PLUGIN_FAIL);
        return;
    }

    ctrl.index = ctrlIdx;
    ctrl.serialNo = serial;
    ctrl.active = true;
    ctrl.lastReport = GamepadReport{};
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

    vigem_.unplugDevice(ctrl.serialNo);
    releaseSerial(ctrl.serialNo);
    ctrl.active = false;
    ctrl.serialNo = 0;
    conn.activeControllerCount--;

    closeVigemBusIfIdle();
    client_.sendControllerAck(conn, MSG_CONTROLLER_REMOVE, ctrlIdx, ACK_OK);
    broadcastStatus();
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
    snap.maxControllers = MAX_VIGEM_CONTROLLERS;
    snap.vigemAvailable = vigem_.isBusOpen();

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

        for (auto& ctrl : conn.controllers) {
            if (ctrl.active) {
                cs.controllers.push_back({ctrl.index, ctrl.serialNo, true});
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
    if (reaped > 0) closeVigemBusIfIdle();
    return reaped;
}

// ── Stats ───────────────────────────────────────────────────────────────────

bool SessionService::isViGEmAvailable() const { return vigem_.isBusOpen(); }

int SessionService::totalActiveControllers() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return countGlobalActiveControllers();
}

int SessionService::availableSlots() const {
    std::lock_guard<std::mutex> lk(mtx_);
    int slots = static_cast<int>(
        std::count(std::begin(serialInUse_), std::end(serialInUse_), false));
    return slots;
}
