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
// Clients order/compare by rank; the name is the display token.
enum class LatencyTier : uint8_t {
    Lowest = 0,
    Low = 1,
    Medium = 2,
    High = 3,
};

const char* latencyTierName(LatencyTier tier);
uint8_t latencyTierRank(LatencyTier tier);

struct SubmitPathFacts {
    uint8_t kernelCrossings = 0;
    uint8_t threadWakeups = 0;
    uint8_t brokerHops = 0;
    bool managedRuntime = false;
    uint16_t pollIntervalUs = 0;
};

struct LatencyWeight {
    uint32_t nominalUs;
    uint32_t tailUs;
};

inline constexpr LatencyWeight kWeightKernelCrossing{2, 5};
inline constexpr LatencyWeight kWeightThreadWakeup{15, 250};
inline constexpr LatencyWeight kWeightBrokerHop{40, 500};
inline constexpr LatencyWeight kWeightManagedRuntime{5, 2000};

inline constexpr uint32_t kTailWeightDivisor = 10;

inline constexpr uint32_t kScoreLowestMax = 10;
inline constexpr uint32_t kScoreLowMax = 60;
inline constexpr uint32_t kScoreMediumMax = 250;

struct LatencyCost {
    uint32_t nominalUs = 0;
    uint32_t tailUs = 0;
    uint32_t score = 0;
};

inline constexpr LatencyCost estimateCost(const SubmitPathFacts& f) {
    const uint32_t nominal = kWeightKernelCrossing.nominalUs * f.kernelCrossings +
                             kWeightThreadWakeup.nominalUs * f.threadWakeups +
                             kWeightBrokerHop.nominalUs * f.brokerHops +
                             (f.managedRuntime ? kWeightManagedRuntime.nominalUs : 0u) +
                             f.pollIntervalUs / 2u;
    const uint32_t tail = kWeightKernelCrossing.tailUs * f.kernelCrossings +
                          kWeightThreadWakeup.tailUs * f.threadWakeups +
                          kWeightBrokerHop.tailUs * f.brokerHops +
                          (f.managedRuntime ? kWeightManagedRuntime.tailUs : 0u) + f.pollIntervalUs;
    return {nominal, tail, nominal + tail / kTailWeightDivisor};
}

inline constexpr LatencyTier tierForScore(uint32_t score) {
    if (score < kScoreLowestMax) return LatencyTier::Lowest;
    if (score < kScoreLowMax) return LatencyTier::Low;
    if (score < kScoreMediumMax) return LatencyTier::Medium;
    return LatencyTier::High;
}

inline constexpr const char* submitPathName(const SubmitPathFacts& f) {
    if (f.brokerHops > 0) return "usermode-broker";
    if (f.threadWakeups > 0) return "usermode-shm";
    return "kernel-direct";
}

inline const char* BACKEND_LIFECYCLE_SUPPORTED = "supported";
inline const char* BACKEND_LIFECYCLE_MAINTENANCE = "maintenance";
inline const char* BACKEND_LIFECYCLE_EOL = "eol";

// One backend's support for a single controller type (CONTROLLER_TYPE_*),
// including the feature surface it can deliver for that type. motionRequires
// is the structured requires code ("" = none) surfaced by the catalog when
// this backend is the type's preferred materializer.
struct BackendControllerSupport {
    uint8_t controllerType;
    SubmitPathFacts facts;
    bool motion;
    bool touchpad;
    bool lightbar;
    const char* motionRequires;
    // Raw-output surfaces: only a backend that hands back the game's own
    // DS5/Switch output reports (HIDMaestro) can source these.
    bool triggerEffects = false;
    bool playerLeds = false;
    // Controller-audio endpoints: only a backend that can materialize a
    // composite (USB-audio-carrying) persona presents the pad's own mic and
    // speaker to the host. Static identity, like every other column here: the
    // runtime on/off switch is the `controllerAudio` server setting, which the
    // catalog deliberately does not see (its ETag is version + locale, so
    // install-state variance would serve stale caches).
    bool mic = false;
    bool speaker = false;
};

// Static identity of a backend, independent of host availability. `support`
// points at a static table of length `supportCount`.
struct BackendDescriptor {
    const char* id;          // stable wire id (matches BACKEND_ID_*)
    const char* vendor;      // who maintains the driver
    const char* displayName; // UI label
    // Whether the INPUT submit path crosses into the kernel. It says nothing
    // about controller audio: a HIDMaestro pad carrying audio endpoints is
    // still a user-mode submit path, but its composite persona additionally
    // rides a bundled signed kernel USB transport that installs on first use.
    // The `audio` field in the JSON is what tells a reader that is in play.
    bool kernelMode;
    bool mouseControl; // host pointer injection available alongside this backend
    bool rumble;
    const char* lifecycle;
    const char* eolDate;
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
    std::string driverVersion;

    BackendRuntimeStatus() = default;
    BackendRuntimeStatus(std::string id_, bool available_, std::string errorCode_ = std::string(),
                         std::string driverVersion_ = std::string())
        : id(std::move(id_)), available(available_), errorCode(std::move(errorCode_)),
          driverVersion(std::move(driverVersion_)) {}
};

// True when any controller type this backend serves has audio endpoints, i.e.
// when the `controllerAudio` setting can change what this backend materializes.
bool backendCanCarryAudio(const BackendDescriptor& d);

// JSON array advertised at /api/server/capabilities. Pure: the caller supplies
// probed availability and the host's `controllerAudio` setting;
// identity/latency/features come from the registry. Ids with no descriptor are
// skipped. Each element:
//   {"id","vendor","displayName","kernelMode","audio","available","errorCode",
//    "lifecycle","eolDate","driverVersion",
//    "controllers":[{"type","name","latency","latencyRank","motion","touchpad",
//                    "lightbar","motionRequires","submitLatency"}, ...]}
// `audio` is the only runtime-switched field on the element: the per-controller
// `mic`/`speaker` columns are static identity (what the backend COULD do), and
// `audio` is whether this host will actually ask it to. Both are needed: a
// client reading only the columns would offer audio a host has turned off.
std::string buildBackendsJson(const std::vector<BackendRuntimeStatus>& statuses,
                              bool controllerAudio);

// Catalog traits folded over the host's enumerated backends, preference by
// list order (the platform's enumerateBackends() ordering). A type is offered
// if any listed backend supports it; its feature surface and requires code
// come from the FIRST listed backend supporting it, which is also the backend
// the plug router prefers, so catalog and routing stay in lockstep. Static
// identity only — availability never shapes the catalog (its ETag is keyed on
// server version + locale, so install-state variance would serve stale caches).
CatalogBackendTraits deriveCatalogTraits(const std::vector<BackendRuntimeStatus>& statuses);

} // namespace satellite
