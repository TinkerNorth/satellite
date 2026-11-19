// SPDX-License-Identifier: LGPL-3.0-or-later

// GET /api/diagnostics: Satellite's dish-diagnostics.v1 file, the shape every
// Dish client writes (docs/architecture.md "Diagnostics file"). Pure: the route
// gathers the inputs, this redacts them and builds the document.
//
// The file never holds a pairing key, a pairing secret, an IP address, a
// network or interface name, or the name or id of a paired device: clients
// appear as "Host 1", "Host 2".
#pragma once

#include "controller_db_service.h"
#include "json.h"
#include "types.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace satellite {

inline constexpr int DIAGNOSTICS_BUNDLE_VERSION = 1;
inline constexpr const char* DIAGNOSTICS_APP_NAME = "satellite";
inline constexpr size_t DIAGNOSTICS_LOG_LINES = 500;
inline constexpr const char* DIAGNOSTICS_MASKED_IP = "x.x.x.x";

// Keys removed wherever they appear, at any depth, before anything is written.
inline constexpr const char* DIAGNOSTICS_FORBIDDEN_KEYS[] = {
    "pairedDevices", "sharedKey", "pairingKey", "publicKey",  "hmacProof",   "sessionKey",
    "sessionSalt",   "token",     "pin",        "currentPin", "previousPin", "clientPin",
    "lastIP",        "clientIP",  "senderIP",   "deviceId",   "deviceName",  "networkInterface",
};

// The settings with every forbidden key removed. What the removed keys said
// that support can use without them stays as a count or a flag:
// `pairedDeviceCount`, and `networkInterfaceChosen` (false is "automatic").
Json redactConfigForDiagnostics(const Json& config);

// Any other document (the /api/debug counters, the backend status) with every
// forbidden key removed.
Json redactForDiagnostics(const Json& doc);

// What names a client: its id, its name and its address. Each is replaced in
// log text by the client's alias.
struct HostIdentity {
    std::string deviceId;
    std::string deviceName;
    std::string address;
};

struct HostAlias {
    std::string text;
    std::string alias;
};

// "Host 1", "Host 2" for the connected clients in order, then the paired ones
// that are not connected, numbered on; one entry per non-empty identity text.
std::vector<HostAlias> aliasHosts(const std::vector<HostIdentity>& connected,
                                  const std::vector<HostIdentity>& pairedOnly);

std::string hostAlias(size_t index);

// Log text with every alias text replaced (longest first, so a name that
// contains another is replaced whole) and every dotted-quad IPv4 address
// masked.
std::string scrubDiagnosticsText(const std::string& text, const std::vector<HostAlias>& aliases);

// Unix seconds as 2026-11-19T21:04:11Z.
std::string isoUtc(int64_t epochSeconds);

struct DiagnosticsHost {
    std::string alias;
    int protocol = 0;
    std::string link;
};

struct DiagnosticsController {
    std::string host;
    int ctrlIdx = 0;
    std::string type;
    std::string backend;
    bool pluggedIn = false;
    uint16_t caps = 0;
    GameOutputKind lastGameOutput = GameOutputKind::None;
    int64_t lastGameOutputAtMs = 0;
};

struct DiagnosticsLogLine {
    int64_t atMs = 0;
    std::string level;
    std::string tag;
    std::string message;
};

struct DiagnosticsInput {
    int64_t nowEpoch = 0;
    std::string version;
    std::string build;
    std::string flavor;
    std::string deviceModel;
    std::string os;
    std::string arch;
    ControllerDbStatus controllerDb;
    std::vector<DiagnosticsHost> hosts;
    std::vector<DiagnosticsController> controllers;
    Json debug;
    Json backends;
    Json config;
    // Oldest first; only the last DIAGNOSTICS_LOG_LINES are written.
    std::vector<DiagnosticsLogLine> log;
    std::vector<HostAlias> aliases;
};

JsonOut buildDiagnosticsBundle(const DiagnosticsInput& in);

// The controllerDb block, as the bundle and GET /api/controller-db/status
// both write it.
JsonOut controllerDbStatusJson(const ControllerDbStatus& status);

// {kind, atMs}, or null when the game has written nothing; the bundle and
// /api/connections both write it.
JsonOut gameOutputJson(GameOutputKind kind, int64_t atMs);

} // namespace satellite
