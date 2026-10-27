// SPDX-License-Identifier: LGPL-3.0-or-later
#include "admin_json.h"

#include "app/app_state.h"
#include "core/json.h"
#include "core/log_ring.h"
#include "core/session_service.h"
#include "core/tracked_mutex.h"
#include "core/types.h"

#include <cstdio>
#include <mutex>
#include <string>
#include <utility>

using satellite::jsonDump;
using satellite::JsonOut;
using satellite::logEntryJson;
using satellite::logRingSlotsSince;

namespace {

JsonOut controllerBatteryObj(const SessionService::ConnectionSnapshot::CtrlInfo& ctrl) {
    JsonOut battery;
    if (ctrl.batteryLevel == BATTERY_LEVEL_UNKNOWN) {
        battery["level"] = nullptr;
    } else {
        battery["level"] = ctrl.batteryLevel;
    }
    battery["status"] = batteryStatusName(ctrl.batteryStatus);
    return battery;
}

JsonOut controllerSnapshotObj(const SessionService::ConnectionSnapshot::CtrlInfo& ctrl) {
    // Only Live/Detached are surfaced today; transient states aren't yet
    // threaded through SessionService. See ControllerState in core/types.h.
    const char* ctrlState = ctrl.active ? controllerStateName(ControllerState::Live)
                                        : controllerStateName(ControllerState::Detached);
    JsonOut o;
    o["controllerIndex"] = ctrl.index;
    o["serialNo"] = ctrl.serial;
    o["pluggedIn"] = ctrl.pluggedIn;
    o["state"] = ctrlState;
    o["controllerType"] = controllerTypeName(ctrl.controllerType);
    o["controllerTypeLabel"] = controllerTypeLabel(ctrl.controllerType);
    // Resolved here so the dashboard never rebuilds an asset path from the wire enum.
    o["catalogSlug"] = controllerTypeCatalogSlug(ctrl.controllerType);
    o["touchpadMode"] = touchpadModeName(ctrl.touchpadMode);
    if (ctrl.backendId.empty()) {
        o["backend"] = nullptr;
    } else {
        o["backend"] = ctrl.backendId;
    }
    if (ctrl.batteryKnown) {
        o["battery"] = controllerBatteryObj(ctrl);
    } else {
        o["battery"] = nullptr;
    }
    o["motionCapable"] = ctrl.motionCapable;
    o["motionActive"] = ctrl.motionActive;
    o["motionSink"] = ctrl.motionSink;
    // Backend has an IMU surface for this controller type; UI warns when
    // motionCapable but not this (motion has nowhere to land, e.g. Xbox pad).
    o["motionSinkSupportedForType"] = ctrl.motionSinkSupportedForType;
    // IMU sink was created at plug-in; false flags a kernel-level failure
    // (uinput perms, kernel too old) vs. just "no game subscribed".
    o["motionBackendOk"] = ctrl.motionBackendOk;
    o["touchpadActive"] = ctrl.touchpadActive;
    o["lightbarCapable"] = ctrl.lightbarCapable;
    if (ctrl.lightbarKnown) {
        char rgb[8];
        snprintf(rgb, sizeof(rgb), "#%02x%02x%02x", ctrl.lightbarR, ctrl.lightbarG, ctrl.lightbarB);
        o["lightbar"] = std::string(rgb);
    } else {
        o["lightbar"] = nullptr;
    }
    return o;
}

// `state` fields serialise as lowercase enum names, the canonical wire form.
// `connectedAtEpoch` is steady-clock seconds (boot-relative), not Unix epoch.
JsonOut connectionSnapshotObj(const SessionService::ConnectionSnapshot& cs) {
    JsonOut c;
    c["connectionId"] = cs.connectionId;
    c["deviceId"] = cs.deviceId;
    c["deviceName"] = cs.deviceName;
    c["senderIP"] = cs.clientIP;
    c["connectedAtEpoch"] = cs.connectedAtEpoch;
    c["epoch"] = cs.epoch;
    c["mouseControlGranted"] = cs.mouseControlGranted;
    c["protocolVersion"] = cs.protocolVersion;
    c["protocolCurrent"] = PROTOCOL_VERSION;
    // Active or NotResponding here; /api/devices covers the Paired (offline) case.
    c["state"] = deviceLinkStateName(cs.linkState);
    JsonOut controllers = JsonOut::array();
    for (const auto& ctrl : cs.controllers) controllers.push_back(controllerSnapshotObj(ctrl));
    c["controllers"] = std::move(controllers);
    c["activeControllerCount"] = cs.activeControllerCount;
    return c;
}

} // namespace

std::string buildConnectionsJson(const SessionService& svc) {
    const auto snap = svc.getConnectionsSnapshot();
    JsonOut connections = JsonOut::array();
    for (const auto& cs : snap.connections) connections.push_back(connectionSnapshotObj(cs));
    JsonOut j;
    j["connections"] = std::move(connections);
    j["totalControllers"] = snap.totalControllers;
    j["maxControllers"] = snap.maxControllers;
    j["backendAvailable"] = snap.backendAvailable;
    return jsonDump(j);
}

// Paired devices + their live link state (paired | active | notResponding).
// Shared by the admin route and the SSE devices event.
std::string buildDevicesJson(const SessionService& svc) {
    const std::shared_ptr<const Config> cfg = g_configStore->current();
    JsonOut arr = JsonOut::array();
    for (const auto& d : cfg->pairedDevices) {
        const DeviceLinkState s = svc.linkStateForDevice(d.id);
        JsonOut o;
        o["id"] = d.id;
        o["name"] = d.name;
        o["lastIP"] = d.lastIP;
        o["pairedAt"] = d.pairedAt;
        o["state"] = deviceLinkStateName(s);
        arr.push_back(std::move(o));
    }
    return jsonDump(arr);
}

std::string buildLogsJson(uint64_t since) {
    std::lock_guard<TrackedMutex> lk(g_logMtx);
    JsonOut entries = JsonOut::array();
    for (const auto& slot : logRingSlotsSince(LOG_RING_SIZE, g_logHead, g_logSeq, since)) {
        entries.push_back(logEntryJson(slot.seq, g_logRing[static_cast<size_t>(slot.index)]));
    }
    JsonOut j;
    j["seq"] = g_logSeq;
    j["entries"] = std::move(entries);
    return jsonDump(j);
}
