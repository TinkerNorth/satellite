// SPDX-License-Identifier: LGPL-3.0-or-later

// Library-agnostic virtual-pad backend registry: identity, vendor, and
// per-controller-type latency the API advertises so a client can compare
// "controller type X via backend A vs B" without hardcoding any library name.
// Data-only — a new backend or controller type is a table entry, not a code
// change. Pure (no OS deps) so it unit-tests and links into every target.
#pragma once

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
    Low = 1,    // user-mode shared-memory submit (e.g. HIDMaestro)
    Medium = 2,
    High = 3,
};

const char* latencyTierName(LatencyTier tier);
uint8_t latencyTierRank(LatencyTier tier);

// One backend's support + latency for a single controller type (CONTROLLER_TYPE_*).
struct BackendControllerSupport {
    uint8_t controllerType;
    LatencyTier latency;
};

// Static identity of a backend, independent of host availability. `support`
// points at a static table of length `supportCount`.
struct BackendDescriptor {
    const char* id;          // stable wire id (matches BACKEND_ID_*)
    const char* vendor;      // who maintains the driver
    const char* displayName; // UI label
    bool kernelMode;
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
// probed availability; identity/latency come from the registry. Ids with no
// descriptor are skipped. Each element:
//   {"id","vendor","displayName","kernelMode","available","errorCode",
//    "controllers":[{"type","name","latency","latencyRank"}, ...]}
std::string buildBackendsJson(const std::vector<BackendRuntimeStatus>& statuses);

} // namespace satellite
