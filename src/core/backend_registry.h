// SPDX-License-Identifier: LGPL-3.0-or-later

// Library-agnostic virtual-pad backend registry: identity, vendor, and
// per-controller-type capability/latency data the API advertises so a client
// can compare "controller type X via backend A vs B" without hardcoding any
// library name. Data-only — a new backend or controller type is a table entry,
// not a code change. Pure (no OS deps) so it unit-tests and links into every
// target.
#pragma once

#include "core/catalog.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace satellite {

// Relative submit latency class. Ordinal: a smaller rank means lower latency.
// Clients order/compare by rank; the name is the display token. Kernel-mode
// submit paths rank below user-mode shared-memory ones.
enum class LatencyTier : uint8_t {
    Lowest = 0, // kernel-mode IOCTL submit (e.g. ViGEm, uinput)
    Low = 1,    // user-mode shared-memory submit (e.g. HIDMaestro, machid)
    Medium = 2,
    High = 3,
};

const char* latencyTierName(LatencyTier tier);
uint8_t latencyTierRank(LatencyTier tier);

// One backend's support for a single controller type (CONTROLLER_TYPE_*),
// including the feature surface it can deliver for that type. motionRequires
// is the structured requires code ("" = none) surfaced by the catalog when
// this backend is the type's preferred materializer.
struct BackendControllerSupport {
    uint8_t controllerType;
    LatencyTier latency;
    bool motion;
    bool touchpad;
    bool lightbar;
    const char* motionRequires;
};

// Static identity of a backend, independent of host availability. `support`
// points at a static table of length `supportCount`.
struct BackendDescriptor {
    const char* id;          // stable wire id (matches BACKEND_ID_*)
    const char* vendor;      // who maintains the driver
    const char* displayName; // UI label
    bool kernelMode;
    bool mouseControl; // host pointer injection available alongside this backend
    bool rumble;
    const BackendControllerSupport* support;
    size_t supportCount;
};

// Lookup by wire id; nullptr if unknown.
const BackendDescriptor* backendDescriptorById(const std::string& id);

// Per-host availability of one backend, joined with its descriptor for the API.
struct BackendRuntimeStatus {
    std::string id;
    bool available = false;
    std::string errorCode; // empty when available
};

// JSON array advertised at /api/server/capabilities. Pure: the caller supplies
// probed availability; identity/latency/features come from the registry. Ids
// with no descriptor are skipped. Each element:
//   {"id","vendor","displayName","kernelMode","available","errorCode",
//    "controllers":[{"type","name","latency","latencyRank",
//                    "motion","touchpad","lightbar"}, ...]}
std::string buildBackendsJson(const std::vector<BackendRuntimeStatus>& statuses);

// Catalog traits folded over the host's enumerated backends, preference by
// list order (the platform's enumerateBackends() ordering). A type is offered
// if any listed backend supports it; its feature surface and requires code
// come from the FIRST listed backend supporting it, which is also the backend
// the plug router prefers, so catalog and routing stay in lockstep. Static
// identity only — availability never shapes the catalog (its ETag is keyed on
// server version + locale, so install-state variance would serve stale caches).
CatalogBackendTraits deriveCatalogTraits(const std::vector<BackendRuntimeStatus>& statuses);

} // namespace satellite
