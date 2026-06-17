// SPDX-License-Identifier: LGPL-3.0-or-later

// core/backend_registry — the library-agnostic backend model the API advertises:
// identity + vendor + per-controller-type latency tiers, and the JSON builder
// that joins static descriptors with probed availability. Pure, so it tests
// without any platform probe.
#include "../src/core/backend_registry.h"
#include "../src/core/gamepad_backend.h" // BACKEND_ID_*
#include "../src/core/types.h"           // CONTROLLER_TYPE_*

#include <iostream>
#include <string>

static int g_pass = 0;
static int g_fail = 0;
static std::string g_currentTest;

#define TEST(name)                                                                                 \
    do { g_currentTest = (name); } while (0)

#define EXPECT(cond)                                                                               \
    do {                                                                                           \
        if (cond) {                                                                                \
            g_pass++;                                                                              \
        } else {                                                                                   \
            g_fail++;                                                                              \
            std::cerr << "  FAIL [" << g_currentTest << "] " << __FILE__ << ":" << __LINE__        \
                      << "  " << #cond << "\n";                                                    \
        }                                                                                          \
    } while (0)

#define EXPECT_EQ(a, b)                                                                            \
    do {                                                                                           \
        auto _a = (a);                                                                             \
        auto _b = (b);                                                                             \
        if (_a == _b) {                                                                            \
            g_pass++;                                                                              \
        } else {                                                                                   \
            g_fail++;                                                                              \
            std::cerr << "  FAIL [" << g_currentTest << "] " << __FILE__ << ":" << __LINE__        \
                      << "  " << #a << " == " << #b << "  (got " << _a << " vs " << _b << ")\n";   \
        }                                                                                          \
    } while (0)

using namespace satellite;

static bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

static int countOccur(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

static void test_latencyTier_names_and_ranks() {
    TEST("latencyTier name + rank ordinal");
    EXPECT_EQ(std::string(latencyTierName(LatencyTier::Lowest)), std::string("lowest"));
    EXPECT_EQ(std::string(latencyTierName(LatencyTier::Low)), std::string("low"));
    EXPECT_EQ(std::string(latencyTierName(LatencyTier::Medium)), std::string("medium"));
    EXPECT_EQ(std::string(latencyTierName(LatencyTier::High)), std::string("high"));
    // Rank is the comparison key: smaller == lower latency.
    EXPECT(latencyTierRank(LatencyTier::Lowest) < latencyTierRank(LatencyTier::Low));
    EXPECT(latencyTierRank(LatencyTier::Low) < latencyTierRank(LatencyTier::Medium));
}

static void test_descriptorById_known_and_unknown() {
    TEST("backendDescriptorById lookup");
    const BackendDescriptor* vigem = backendDescriptorById(BACKEND_ID_VIGEM);
    EXPECT(vigem != nullptr);
    EXPECT_EQ(std::string(vigem->vendor), std::string("Nefarius Software Solutions"));
    EXPECT(vigem->kernelMode);

    const BackendDescriptor* hm = backendDescriptorById(BACKEND_ID_HIDMAESTRO);
    EXPECT(hm != nullptr);
    EXPECT(!hm->kernelMode); // user-mode UMDF2
    EXPECT_EQ(std::string(hm->displayName), std::string("HIDMaestro"));

    EXPECT(backendDescriptorById("does-not-exist") == nullptr);
}

// The headline requirement: a client can tell "Xbox via vigem is faster than
// Xbox via hidmaestro" generically from the rank, without naming either library.
static void test_cross_backend_latency_is_comparable() {
    TEST("same controller type ranks lower-latency on the kernel backend");
    const BackendDescriptor* vigem = backendDescriptorById(BACKEND_ID_VIGEM);
    const BackendDescriptor* hm = backendDescriptorById(BACKEND_ID_HIDMAESTRO);
    EXPECT(vigem != nullptr && hm != nullptr);

    auto rankFor = [](const BackendDescriptor* d, uint8_t type) -> int {
        for (size_t i = 0; i < d->supportCount; ++i)
            if (d->support[i].controllerType == type) return latencyTierRank(d->support[i].latency);
        return -1;
    };

    EXPECT(rankFor(vigem, CONTROLLER_TYPE_XBOX) >= 0);
    EXPECT(rankFor(hm, CONTROLLER_TYPE_XBOX) >= 0);
    EXPECT(rankFor(vigem, CONTROLLER_TYPE_XBOX) < rankFor(hm, CONTROLLER_TYPE_XBOX));
    EXPECT(rankFor(vigem, CONTROLLER_TYPE_PLAYSTATION) < rankFor(hm, CONTROLLER_TYPE_PLAYSTATION));
}

static void test_buildBackendsJson_available_backend() {
    TEST("buildBackendsJson — available backend serializes vendor + null error");
    std::vector<BackendRuntimeStatus> statuses = {{BACKEND_ID_VIGEM, true, ""}};
    std::string json = buildBackendsJson(statuses);
    EXPECT(contains(json, "\"id\":\"vigem\""));
    EXPECT(contains(json, "\"vendor\":\"Nefarius Software Solutions\""));
    EXPECT(contains(json, "\"available\":true"));
    EXPECT(contains(json, "\"errorCode\":null"));
    EXPECT(contains(json, "\"kernelMode\":true"));
    // Per-controller latency carried inline so the client can compare.
    EXPECT(contains(json, "\"name\":\"xbox\""));
    EXPECT(contains(json, "\"latency\":\"lowest\""));
    EXPECT(contains(json, "\"latencyRank\":0"));
}

static void test_buildBackendsJson_unavailable_backend() {
    TEST("buildBackendsJson — unavailable backend carries errorCode + tier");
    std::vector<BackendRuntimeStatus> statuses = {{BACKEND_ID_HIDMAESTRO, false, "NOT_INSTALLED"}};
    std::string json = buildBackendsJson(statuses);
    EXPECT(contains(json, "\"id\":\"hidmaestro\""));
    EXPECT(contains(json, "\"available\":false"));
    EXPECT(contains(json, "\"errorCode\":\"NOT_INSTALLED\""));
    EXPECT(contains(json, "\"kernelMode\":false"));
    EXPECT(contains(json, "\"latency\":\"low\""));
    EXPECT(contains(json, "\"latencyRank\":1"));
}

static void test_buildBackendsJson_skips_unknown_ids() {
    TEST("buildBackendsJson — unknown ids dropped, not emitted");
    std::vector<BackendRuntimeStatus> statuses = {
        {"phantom-backend", true, ""},
        {BACKEND_ID_VIGEM, true, ""},
    };
    std::string json = buildBackendsJson(statuses);
    EXPECT(!contains(json, "phantom-backend"));
    EXPECT(contains(json, "\"id\":\"vigem\""));
    // Exactly one backend object survived (each object has one top-level "id").
    EXPECT(contains(json, "[{"));
    EXPECT(contains(json, "}]"));
    EXPECT_EQ(countOccur(json, "\"id\":\""), 1);
}

static void test_buildBackendsJson_empty_and_multi() {
    TEST("buildBackendsJson — empty list and multi-backend ordering");
    EXPECT_EQ(buildBackendsJson({}), std::string("[]"));

    std::vector<BackendRuntimeStatus> statuses = {
        {BACKEND_ID_VIGEM, true, ""},
        {BACKEND_ID_HIDMAESTRO, false, "NOT_INSTALLED"},
    };
    std::string json = buildBackendsJson(statuses);
    // Preference order preserved: vigem element precedes hidmaestro element.
    EXPECT(json.find("vigem") < json.find("hidmaestro"));
}

int main() {
    test_latencyTier_names_and_ranks();
    test_descriptorById_known_and_unknown();
    test_cross_backend_latency_is_comparable();
    test_buildBackendsJson_available_backend();
    test_buildBackendsJson_unavailable_backend();
    test_buildBackendsJson_skips_unknown_ids();
    test_buildBackendsJson_empty_and_multi();

    std::cout << "backend_registry: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
