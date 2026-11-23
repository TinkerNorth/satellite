// SPDX-License-Identifier: LGPL-3.0-or-later

// GET /api/connections/{id}/diagnostics: the satellite-connection-diagnostics.v1
// document a paired Dish asks for about its own connection, to put beside its
// own view of the session in its diagnostics file. Pure: the route gathers the
// facts, this picks the log lines about the connection, scrubs them and builds
// the document (docs/architecture.md "Connection diagnostics").
//
// The document never names another client, the machine, an address or a
// secret: the requester is "this connection", every other client "another
// client", the machine "this PC".
#pragma once

#include "controller_db_service.h"
#include "diagnostics_bundle.h"
#include "json.h"
#include "types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace satellite {

inline constexpr const char* CONNECTION_DIAGNOSTICS_FORMAT = "satellite-connection-diagnostics.v1";
inline constexpr size_t CONNECTION_DIAGNOSTICS_LOG_LINES = 200;
inline constexpr const char* DIAGNOSTICS_THIS_CONNECTION = "this connection";
inline constexpr const char* DIAGNOSTICS_ANOTHER_CLIENT = "another client";
// Satellite keeps its wire, audio and rejection counters per process, not per
// session; the per-session counts are the controllers' input and output.
inline constexpr const char* CONNECTION_COUNTERS_SCOPE = "process";

struct ConnectionBackend {
    std::string id;
    bool available = false;
    std::string driver;
    std::string bundled;
    bool preferred = false;
    std::string lifecycle;
};

struct ConnectionController {
    int ctrlIdx = 0;
    uint8_t requestedType = CONTROLLER_TYPE_XBOX;
    uint8_t appliedType = CONTROLLER_TYPE_XBOX;
    std::string backend;
    bool composite = false;
    bool pluggedIn = false;
    uint16_t caps = 0;
    uint8_t touchpadMode = TOUCHPAD_MODE_OFF;
    bool motionSinkSupportedForType = false;
    bool motionBackendOk = false;
    uint32_t serial = 0;
    int64_t pluggedAtMs = 0;
    InputFacts input;
    GameOutputRecord output;
    GameOutputKind lastGameOutput = GameOutputKind::None;
    int64_t lastGameOutputAtMs = 0;
};

// A log line names one of this connection's pads by its serial ("Serial 3",
// "serial 3"); it is about this connection only from the pad's plug on, since
// a serial passes between clients.
struct SerialClaim {
    uint32_t serial = 0;
    int64_t sinceMs = 0;
};

struct ConnectionLogScope {
    // The tokens Satellite itself writes for the connection: its id and its
    // paired device's id, each matched whole. Never the name the client chose
    // or an address, which other clients' lines can hold too.
    std::vector<std::string> texts;
    std::vector<SerialClaim> serials;
};

struct ConnectionDiagnosticsInput {
    std::string version;
    // "windows" | "macos" | "linux"
    std::string platform;
    int protocolMin = 0;
    int protocolMax = 0;
    int64_t uptimeS = 0;
    std::vector<ConnectionBackend> backends;
    std::vector<ConnectionController> controllers;
    // The host's controllerAudio block, as /api/server/capabilities writes it.
    Json grants;
    // {rx, tx, audio, rejected}; the document adds the scope.
    Json counters;
    ControllerDbStatus controllerDb;
    std::string connectionId;
    HostIdentity self;
    // Every other connected or paired client.
    std::vector<HostIdentity> others;
    // The host name and anything spelled from it (the mDNS instance).
    std::vector<std::string> machineNames;
    // Key material that must never appear, as text (pairing keys in hex).
    std::vector<std::string> secrets;
    // The recent ring, oldest first.
    std::vector<DiagnosticsLogLine> log;
};

bool logLineConcernsConnection(const DiagnosticsLogLine& line, const ConnectionLogScope& scope);

ConnectionLogScope connectionLogScope(const ConnectionDiagnosticsInput& in);

// The requester's id, name and address become "this connection", every other
// client's "another client", each machine name "this PC", each secret
// "[redacted]".
std::vector<HostAlias> connectionAliases(const ConnectionDiagnosticsInput& in);

JsonOut buildConnectionDiagnostics(const ConnectionDiagnosticsInput& in);

} // namespace satellite
