// SPDX-License-Identifier: LGPL-3.0-or-later

// core/backend_registry — the library-agnostic backend model the API
// advertises: identity + vendor + per-controller-type capability/latency data,
// the JSON builder joining static descriptors with probed availability, and
// the catalog-traits fold. Pure, so it tests without any platform probe.
#include "test_util.h"

#include "../src/core/backend_registry.h"
#include "../src/core/gamepad_backend.h"
#include "../src/core/types.h"

#include <string>

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
    EXPECT(!hm->kernelMode);
    EXPECT_EQ(std::string(hm->displayName), std::string("HIDMaestro"));
    EXPECT_EQ(hm->supportCount, static_cast<size_t>(4));

    EXPECT(backendDescriptorById(BACKEND_ID_UINPUT) != nullptr);
    EXPECT(backendDescriptorById(BACKEND_ID_MAC_HID) != nullptr);
    EXPECT(backendDescriptorById(BACKEND_ID_NONE) != nullptr);
    EXPECT(backendDescriptorById("does-not-exist") == nullptr);
}

// The headline requirement: a client can tell "Xbox via vigem is faster than
// Xbox via hidmaestro" generically from the rank, without naming any library.
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

static void test_hidmaestro_widens_the_windows_type_set() {
    TEST("hidmaestro supports the two types vigem cannot materialize");
    const BackendDescriptor* vigem = backendDescriptorById(BACKEND_ID_VIGEM);
    const BackendDescriptor* hm = backendDescriptorById(BACKEND_ID_HIDMAESTRO);
    EXPECT(vigem != nullptr && hm != nullptr);

    auto supports = [](const BackendDescriptor* d, uint8_t type) {
        for (size_t i = 0; i < d->supportCount; ++i)
            if (d->support[i].controllerType == type) return true;
        return false;
    };

    EXPECT(!supports(vigem, CONTROLLER_TYPE_DUALSENSE));
    EXPECT(!supports(vigem, CONTROLLER_TYPE_SWITCHPRO));
    EXPECT(supports(hm, CONTROLLER_TYPE_XBOX));
    EXPECT(supports(hm, CONTROLLER_TYPE_PLAYSTATION));
    EXPECT(supports(hm, CONTROLLER_TYPE_DUALSENSE));
    EXPECT(supports(hm, CONTROLLER_TYPE_SWITCHPRO));
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
    EXPECT(contains(json, "\"name\":\"xbox\""));
    EXPECT(contains(json, "\"latency\":\"lowest\""));
    EXPECT(contains(json, "\"latencyRank\":0"));
    EXPECT(contains(json, "\"motion\":true"));
    EXPECT(contains(json, "\"touchpad\":true"));
    EXPECT(contains(json, "\"lightbar\":true"));
}

static void test_buildBackendsJson_unavailable_backend() {
    TEST("buildBackendsJson — unavailable backend carries errorCode + tier");
    std::vector<BackendRuntimeStatus> statuses = {{BACKEND_ID_HIDMAESTRO, false, "DRIVER_MISSING"}};
    std::string json = buildBackendsJson(statuses);
    EXPECT(contains(json, "\"id\":\"hidmaestro\""));
    EXPECT(contains(json, "\"vendor\":\"hifihedgehog\""));
    EXPECT(contains(json, "\"available\":false"));
    EXPECT(contains(json, "\"errorCode\":\"DRIVER_MISSING\""));
    EXPECT(contains(json, "\"kernelMode\":false"));
    EXPECT(contains(json, "\"latency\":\"low\""));
    EXPECT(contains(json, "\"latencyRank\":1"));
    EXPECT(contains(json, "\"name\":\"dualsense\""));
    EXPECT(contains(json, "\"name\":\"switchpro\""));
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
    EXPECT(contains(json, "[{"));
    EXPECT(contains(json, "}]"));
    EXPECT_EQ(countOccur(json, "\"vendor\":"), 1);
}

static void test_buildBackendsJson_empty_and_multi() {
    TEST("buildBackendsJson — empty list and multi-backend ordering");
    EXPECT_EQ(buildBackendsJson({}), std::string("[]"));

    std::vector<BackendRuntimeStatus> statuses = {
        {BACKEND_ID_VIGEM, true, ""},
        {BACKEND_ID_HIDMAESTRO, false, "DRIVER_MISSING"},
    };
    std::string json = buildBackendsJson(statuses);
    EXPECT(json.find("vigem") < json.find("hidmaestro"));
}

static void test_deriveCatalogTraits_windows_union() {
    TEST("deriveCatalogTraits — vigem+hidmaestro union, preference by order");
    std::vector<BackendRuntimeStatus> statuses = {
        {BACKEND_ID_VIGEM, true, ""},
        {BACKEND_ID_HIDMAESTRO, false, "DRIVER_MISSING"},
    };
    CatalogBackendTraits t = deriveCatalogTraits(statuses);
    EXPECT(t.offersXbox);
    EXPECT(t.offersDS4);
    EXPECT(t.offersDualSense);
    EXPECT(t.offersSwitchPro);
    // ds4 keeps the preferred (first-listed) backend's surface + requires code.
    EXPECT(t.ds4MotionSupported);
    EXPECT_EQ(t.ds4MotionRequires, std::string("vigembus>=1.17"));
    EXPECT(t.ds4TouchpadSupported);
    EXPECT(t.ds4LightbarSupported);
    // dualsense/switchpro only exist via hidmaestro, so they carry its code.
    EXPECT(t.dualsenseMotionSupported);
    EXPECT_EQ(t.dualsenseMotionRequires, std::string("hidmaestro>=1.7"));
    EXPECT(t.dualsenseTouchpadSupported);
    EXPECT(t.dualsenseLightbarSupported);
    EXPECT(t.switchProMotionSupported);
    EXPECT_EQ(t.switchProMotionRequires, std::string("hidmaestro>=1.7"));
    EXPECT(t.mouseControlSupported);
    EXPECT(t.rumbleSupported);
    EXPECT(!t.keyboardControlSupported);
}

static void test_deriveCatalogTraits_order_decides_requires() {
    TEST("deriveCatalogTraits — first-listed backend wins the shared types");
    std::vector<BackendRuntimeStatus> statuses = {
        {BACKEND_ID_HIDMAESTRO, true, ""},
        {BACKEND_ID_VIGEM, true, ""},
    };
    CatalogBackendTraits t = deriveCatalogTraits(statuses);
    EXPECT_EQ(t.ds4MotionRequires, std::string("hidmaestro>=1.7"));
}

static void test_deriveCatalogTraits_uinput_matches_legacy() {
    TEST("deriveCatalogTraits — uinput reproduces the historical Linux traits");
    CatalogBackendTraits t = deriveCatalogTraits({{BACKEND_ID_UINPUT, true, ""}});
    EXPECT(t.offersXbox);
    EXPECT(t.offersDS4);
    EXPECT(t.offersDualSense);
    EXPECT(t.offersSwitchPro);
    EXPECT(t.ds4MotionSupported);
    EXPECT_EQ(t.ds4MotionRequires, std::string(""));
    EXPECT(t.dualsenseMotionSupported);
    EXPECT_EQ(t.dualsenseMotionRequires, std::string(""));
    EXPECT(t.switchProMotionSupported);
    EXPECT(t.mouseControlSupported);
    EXPECT(t.rumbleSupported);
}

static void test_deriveCatalogTraits_machid_matches_legacy() {
    TEST("deriveCatalogTraits — machid/none reproduce the historical macOS traits");
    for (const char* id : {BACKEND_ID_MAC_HID, BACKEND_ID_NONE}) {
        CatalogBackendTraits t = deriveCatalogTraits({{id, false, ""}});
        EXPECT(!t.offersXbox);
        EXPECT(t.offersDS4);
        EXPECT(!t.offersDualSense);
        EXPECT(!t.offersSwitchPro);
        EXPECT(t.ds4MotionSupported);
        EXPECT_EQ(t.ds4MotionRequires, std::string(""));
        EXPECT(t.ds4TouchpadSupported);
        EXPECT(t.ds4LightbarSupported);
        EXPECT(!t.mouseControlSupported);
        EXPECT(t.rumbleSupported);
    }
}

static void test_deriveCatalogTraits_ignores_availability() {
    TEST("deriveCatalogTraits — static identity only; availability never gates");
    std::vector<BackendRuntimeStatus> up = {{BACKEND_ID_VIGEM, true, ""},
                                            {BACKEND_ID_HIDMAESTRO, true, ""}};
    std::vector<BackendRuntimeStatus> down = {{BACKEND_ID_VIGEM, false, "DRIVER_MISSING"},
                                              {BACKEND_ID_HIDMAESTRO, false, "DRIVER_MISSING"}};
    CatalogBackendTraits a = deriveCatalogTraits(up);
    CatalogBackendTraits b = deriveCatalogTraits(down);
    EXPECT_EQ(a.offersDualSense, b.offersDualSense);
    EXPECT_EQ(a.ds4MotionRequires, b.ds4MotionRequires);
    EXPECT_EQ(a.mouseControlSupported, b.mouseControlSupported);
}

static void test_deriveCatalogTraits_empty_and_unknown() {
    TEST("deriveCatalogTraits — empty list and unknown ids fold to nothing");
    CatalogBackendTraits t = deriveCatalogTraits({{"phantom-backend", true, ""}});
    EXPECT(!t.offersXbox);
    EXPECT(!t.offersDS4);
    EXPECT(!t.offersDualSense);
    EXPECT(!t.offersSwitchPro);
    EXPECT(!t.mouseControlSupported);
    EXPECT(!t.rumbleSupported);
    CatalogBackendTraits e = deriveCatalogTraits({});
    EXPECT(!e.offersDS4);
}

int main() {
    test_latencyTier_names_and_ranks();
    test_descriptorById_known_and_unknown();
    test_cross_backend_latency_is_comparable();
    test_hidmaestro_widens_the_windows_type_set();
    test_buildBackendsJson_available_backend();
    test_buildBackendsJson_unavailable_backend();
    test_buildBackendsJson_skips_unknown_ids();
    test_buildBackendsJson_empty_and_multi();
    test_deriveCatalogTraits_windows_union();
    test_deriveCatalogTraits_order_decides_requires();
    test_deriveCatalogTraits_uinput_matches_legacy();
    test_deriveCatalogTraits_machid_matches_legacy();
    test_deriveCatalogTraits_ignores_availability();
    test_deriveCatalogTraits_empty_and_unknown();

    std::cout << "backend_registry: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
