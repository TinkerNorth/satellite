// SPDX-License-Identifier: LGPL-3.0-or-later

// Helpers shared by BOTH route surfaces (routes_admin.cpp and
// routes_client.cpp), extracted verbatim from webserver.cpp in the D10
// decomposition. Anything used by only one surface stays file-static there.
#pragma once

#include "core/catalog.h"
#include "core/gamepad_backend.h"
#include "core/json.h"

#include <string>

// Lenient body parse: anything but a JSON object collapses to {}.
satellite::Json parseBody(const std::string& body);

std::string readFile(const std::string& path);

// Web UI keys all backend-status copy off (id, errorCode).
satellite::JsonOut backendJsonObj(const BackendStatus& s);

std::string buildBackendJson(const BackendStatus& s);
std::string buildBackendJson();

// The /api/backend/status body: the singular object extended with the
// per-host `backends` array (registry identity + live availability).
std::string buildBackendStatusJson();

// Static facts that shape the catalog, folded over the host's enumerated
// backends via the registry — not their live health (the catalog only changes
// on server upgrade; live health is /api/server/capabilities).
satellite::CatalogBackendTraits catalogBackendTraits();

// GET /api/server/capabilities: CURRENT dynamic state (the static
// what-exists layer is /api/catalog). Served by both the admin and the
// client API servers.
std::string buildCapabilitiesJson();
