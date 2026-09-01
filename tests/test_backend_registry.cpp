// SPDX-License-Identifier: LGPL-3.0-or-later

// core/backend_registry — the library-agnostic backend model the API
// advertises: identity + vendor + per-controller-type capability/latency data,
// the JSON builder joining static descriptors with probed availability, and
// the catalog-traits fold. Pure, so it tests without any platform probe.
#include "test_util.h"

#include "../src/core/backend_registry.h"
#include "../src/core/gamepad_backend.h"
#include "../src/core/json.h"
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

static LatencyTier tierFor(const BackendDescriptor* d, uint8_t type) {
    for (size_t i = 0; i < d->supportCount; ++i) {
        if (d->support[i].controllerType == type)
            return tierForScore(estimateCost(d->support[i].facts).score);
    }
    return LatencyTier::High;
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
            if (d->support[i].controllerType == type) return latencyTierRank(tierFor(d, type));
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

static void test_uinput_advertises_no_lightbar_it_cannot_deliver() {
    TEST("uinput advertises no lightbar (evdev carries no RGB output channel)");
    const BackendDescriptor* uinput = backendDescriptorById(BACKEND_ID_UINPUT);
    EXPECT(uinput != nullptr);
    for (size_t i = 0; i < uinput->supportCount; ++i) { EXPECT(!uinput->support[i].lightbar); }

    CatalogBackendTraits t = deriveCatalogTraits({{BACKEND_ID_UINPUT, true, ""}});
    EXPECT(!t.ds4LightbarSupported);
    EXPECT(!t.dualsenseLightbarSupported);
    EXPECT(t.ds4TouchpadSupported);
    EXPECT(t.ds4MotionSupported);
    EXPECT(t.rumbleSupported);

    EXPECT(!contains(buildBackendsJson({{BACKEND_ID_UINPUT, true, ""}}), "\"lightbar\":true"));

    for (const char* id : {BACKEND_ID_VIGEM, BACKEND_ID_HIDMAESTRO, BACKEND_ID_MAC_HID}) {
        EXPECT(contains(buildBackendsJson({{id, true, ""}}), "\"lightbar\":true"));
    }
}

static void test_rawOutput_surfaces_hidmaestro_only() {
    TEST("triggerEffects/playerLeds ride only HIDMaestro's raw-output types");
    // HIDMaestro hands back the game's raw DS5/Switch output reports, so it is
    // the only backend that can source trigger effects and player LEDs.
    const BackendDescriptor* hm = backendDescriptorById(BACKEND_ID_HIDMAESTRO);
    EXPECT(hm != nullptr);
    for (size_t i = 0; i < hm->supportCount; ++i) {
        const BackendControllerSupport& cs = hm->support[i];
        if (cs.controllerType == CONTROLLER_TYPE_DUALSENSE) {
            EXPECT(cs.triggerEffects);
            EXPECT(cs.playerLeds);
        } else if (cs.controllerType == CONTROLLER_TYPE_SWITCHPRO) {
            EXPECT(!cs.triggerEffects);
            EXPECT(cs.playerLeds);
        } else {
            EXPECT(!cs.triggerEffects);
            EXPECT(!cs.playerLeds);
        }
    }
    for (const char* id : {BACKEND_ID_VIGEM, BACKEND_ID_UINPUT, BACKEND_ID_MAC_HID}) {
        const BackendDescriptor* d = backendDescriptorById(id);
        EXPECT(d != nullptr);
        for (size_t i = 0; i < d->supportCount; ++i) {
            EXPECT(!d->support[i].triggerEffects);
            EXPECT(!d->support[i].playerLeds);
        }
        EXPECT(!contains(buildBackendsJson({{id, true, ""}}), "\"triggerEffects\":true"));
        EXPECT(!contains(buildBackendsJson({{id, true, ""}}), "\"playerLeds\":true"));
    }
    const std::string json = buildBackendsJson({{BACKEND_ID_HIDMAESTRO, true, ""}});
    EXPECT(contains(json, "\"triggerEffects\":true"));
    EXPECT(contains(json, "\"playerLeds\":true"));

    // Traits aggregation carries them through for the catalog.
    CatalogBackendTraits t = deriveCatalogTraits({{BACKEND_ID_HIDMAESTRO, true, ""}});
    EXPECT(t.dualsenseTriggerEffectsSupported);
    EXPECT(t.dualsensePlayerLedsSupported);
    EXPECT(t.switchProPlayerLedsSupported);
    // ViGEm first: DS4 stays the preferred vigem shape, but the hidmaestro-only
    // types still contribute their raw-output surfaces.
    CatalogBackendTraits u =
        deriveCatalogTraits({{BACKEND_ID_VIGEM, true, ""}, {BACKEND_ID_HIDMAESTRO, true, ""}});
    EXPECT(u.dualsenseTriggerEffectsSupported);
    EXPECT(u.dualsensePlayerLedsSupported);
    EXPECT(u.switchProPlayerLedsSupported);
    CatalogBackendTraits v = deriveCatalogTraits({{BACKEND_ID_UINPUT, true, ""}});
    EXPECT(!v.dualsenseTriggerEffectsSupported);
    EXPECT(!v.dualsensePlayerLedsSupported);
    EXPECT(!v.switchProPlayerLedsSupported);
}

static void test_controllerAudio_surfaces_hidmaestroSonyOnly() {
    TEST("mic/speaker ride only HIDMaestro's two Sony types");
    // Controller audio needs a composite (USB-audio) persona, which only
    // HIDMaestro can materialize, and only the DualShock 4 v2 and DualSense
    // have audio endpoints worth emulating in the first place.
    const BackendDescriptor* hm = backendDescriptorById(BACKEND_ID_HIDMAESTRO);
    EXPECT(hm != nullptr);
    for (size_t i = 0; i < hm->supportCount; ++i) {
        const BackendControllerSupport& cs = hm->support[i];
        const bool sony = cs.controllerType == CONTROLLER_TYPE_PLAYSTATION ||
                          cs.controllerType == CONTROLLER_TYPE_DUALSENSE;
        EXPECT_EQ(cs.mic, sony);
        EXPECT_EQ(cs.speaker, sony);
        // Both directions ship together: a pad's audio function carries the
        // headset jack and the microphone as one interface.
        EXPECT_EQ(cs.mic, cs.speaker);
    }
    for (const char* id : {BACKEND_ID_VIGEM, BACKEND_ID_UINPUT, BACKEND_ID_MAC_HID}) {
        const BackendDescriptor* d = backendDescriptorById(id);
        EXPECT(d != nullptr);
        for (size_t i = 0; i < d->supportCount; ++i) {
            EXPECT(!d->support[i].mic);
            EXPECT(!d->support[i].speaker);
        }
        EXPECT(!contains(buildBackendsJson({{id, true, ""}}), "\"mic\":true"));
        EXPECT(!contains(buildBackendsJson({{id, true, ""}}), "\"speaker\":true"));
    }
    const std::string json = buildBackendsJson({{BACKEND_ID_HIDMAESTRO, true, ""}});
    EXPECT(contains(json, "\"mic\":true"));
    EXPECT(contains(json, "\"speaker\":true"));

    // Traits aggregation carries them to the catalog for both Sony types.
    CatalogBackendTraits t = deriveCatalogTraits({{BACKEND_ID_HIDMAESTRO, true, ""}});
    EXPECT(t.ds4MicSupported);
    EXPECT(t.ds4SpeakerSupported);
    EXPECT(t.dualsenseMicSupported);
    EXPECT(t.dualsenseSpeakerSupported);

    // ViGEm listed first takes the DS4 column, and ViGEm has no audio: the
    // catalog must then say so rather than promising a surface the preferred
    // materializer cannot build.
    CatalogBackendTraits u =
        deriveCatalogTraits({{BACKEND_ID_VIGEM, true, ""}, {BACKEND_ID_HIDMAESTRO, true, ""}});
    EXPECT(!u.ds4MicSupported);
    EXPECT(!u.ds4SpeakerSupported);
    EXPECT(u.dualsenseMicSupported); // ViGEm offers no DualSense, so hidmaestro wins it
    EXPECT(u.dualsenseSpeakerSupported);

    CatalogBackendTraits v = deriveCatalogTraits({{BACKEND_ID_UINPUT, true, ""}});
    EXPECT(!v.ds4MicSupported);
    EXPECT(!v.ds4SpeakerSupported);
    EXPECT(!v.dualsenseMicSupported);
    EXPECT(!v.dualsenseSpeakerSupported);
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

static void test_estimateCost_matches_the_documented_table() {
    TEST("estimateCost — documented rows, evaluated at compile time");
    constexpr SubmitPathFacts kernelDirect{1, 0, 0, false, 0};
    constexpr SubmitPathFacts hmSony{2, 1, 0, false, 0};
    constexpr SubmitPathFacts hmXbox{3, 2, 0, false, 0};

    static_assert(estimateCost(kernelDirect).nominalUs == 2, "kernel-direct nominal");
    static_assert(estimateCost(kernelDirect).tailUs == 5, "kernel-direct tail");
    static_assert(estimateCost(kernelDirect).score == 2, "kernel-direct score");
    static_assert(tierForScore(estimateCost(kernelDirect).score) == LatencyTier::Lowest, "");

    static_assert(estimateCost(hmSony).nominalUs == 19, "one wakeup nominal");
    static_assert(estimateCost(hmSony).tailUs == 260, "one wakeup tail");
    static_assert(estimateCost(hmSony).score == 45, "one wakeup score");
    static_assert(tierForScore(estimateCost(hmSony).score) == LatencyTier::Low, "");

    static_assert(estimateCost(hmXbox).nominalUs == 36, "two wakeups nominal");
    static_assert(estimateCost(hmXbox).tailUs == 515, "two wakeups tail");
    static_assert(estimateCost(hmXbox).score == 87, "two wakeups score");
    static_assert(tierForScore(estimateCost(hmXbox).score) == LatencyTier::Medium, "");

    constexpr SubmitPathFacts polled{1, 0, 0, false, 1000};
    static_assert(estimateCost(polled).nominalUs == 502, "poll nominal is T/2");
    static_assert(estimateCost(polled).tailUs == 1005, "poll tail is T");
    constexpr SubmitPathFacts managed{1, 1, 0, true, 0};
    static_assert(estimateCost(managed).tailUs == 2255, "managed runtime dominates the tail");

    EXPECT_EQ(std::string(submitPathName(kernelDirect)), std::string("kernel-direct"));
    EXPECT_EQ(std::string(submitPathName(hmXbox)), std::string("usermode-shm"));
    constexpr SubmitPathFacts brokered{2, 2, 1, false, 0};
    EXPECT_EQ(std::string(submitPathName(brokered)), std::string("usermode-broker"));
}

static void test_derived_tier_separates_rows_within_one_backend() {
    TEST("hidmaestro xbox ranks worse than hidmaestro ds4, with no special case");
    const BackendDescriptor* hm = backendDescriptorById(BACKEND_ID_HIDMAESTRO);
    EXPECT(hm != nullptr);
    EXPECT(tierFor(hm, CONTROLLER_TYPE_XBOX) == LatencyTier::Medium);
    EXPECT(tierFor(hm, CONTROLLER_TYPE_PLAYSTATION) == LatencyTier::Low);
    EXPECT(tierFor(hm, CONTROLLER_TYPE_DUALSENSE) == LatencyTier::Low);
    EXPECT(tierFor(hm, CONTROLLER_TYPE_SWITCHPRO) == LatencyTier::Low);
    EXPECT(latencyTierRank(tierFor(hm, CONTROLLER_TYPE_PLAYSTATION)) <
           latencyTierRank(tierFor(hm, CONTROLLER_TYPE_XBOX)));

    const BackendDescriptor* uinput = backendDescriptorById(BACKEND_ID_UINPUT);
    EXPECT(uinput != nullptr);
    EXPECT(tierFor(uinput, CONTROLLER_TYPE_XBOX) == LatencyTier::Lowest);
    EXPECT(tierFor(uinput, CONTROLLER_TYPE_SWITCHPRO) == LatencyTier::Lowest);
}

static void test_buildBackendsJson_lifecycle_and_submit_latency() {
    TEST("buildBackendsJson — lifecycle, driverVersion, derived detail block");
    std::vector<BackendRuntimeStatus> statuses = {{BACKEND_ID_VIGEM, true, ""}};
    std::string json = buildBackendsJson(statuses);
    EXPECT(contains(json, "\"lifecycle\":\"eol\""));
    EXPECT(contains(json, "\"eolDate\":\"2023-11-02\""));
    EXPECT(contains(json, "\"driverVersion\":null"));
    EXPECT(contains(json, "\"submitPath\":\"kernel-direct\""));
    EXPECT(contains(json, "\"nominalUs\":2"));
    EXPECT(contains(json, "\"tailUs\":5"));
    EXPECT(contains(json, "\"score\":2"));
    EXPECT(contains(json, "\"kernelCrossings\":1"));
    EXPECT(contains(json, "\"threadWakeups\":0"));
    EXPECT(contains(json, "\"managedRuntime\":false"));
    EXPECT(contains(json, "\"pollIntervalUs\":0"));
    EXPECT(contains(json, "\"motionRequires\":\"vigembus>=1.17\""));
    EXPECT(contains(json, "\"motionRequires\":null"));

    std::vector<BackendRuntimeStatus> withVersion = {
        {BACKEND_ID_VIGEM, true, std::string(), "1.22.0"}};
    EXPECT(contains(buildBackendsJson(withVersion), "\"driverVersion\":\"1.22.0\""));

    std::string hmJson = buildBackendsJson({{BACKEND_ID_HIDMAESTRO, true, ""}});
    EXPECT(contains(hmJson, "\"lifecycle\":\"supported\""));
    EXPECT(contains(hmJson, "\"eolDate\":null"));
    EXPECT(contains(hmJson, "\"submitPath\":\"usermode-shm\""));
    EXPECT(contains(hmJson, "\"threadWakeups\":2"));
    EXPECT(contains(hmJson, "\"threadWakeups\":1"));
}

static void test_buildBackendsJson_stays_backwards_compatible() {
    TEST("buildBackendsJson — flat latency/latencyRank still emitted");
    std::string json = buildBackendsJson({{BACKEND_ID_HIDMAESTRO, true, ""}});
    EXPECT(contains(json, "\"latency\":\"medium\""));
    EXPECT(contains(json, "\"latencyRank\":2"));
    EXPECT(contains(json, "\"latency\":\"low\""));
    EXPECT(contains(json, "\"latencyRank\":1"));
    EXPECT(contains(json, "\"type\":0"));
    EXPECT(contains(json, "\"name\":\"xbox\""));
    EXPECT(contains(json, "\"motion\":true"));
    EXPECT(contains(json, "\"touchpad\":true"));
    EXPECT(contains(json, "\"lightbar\":true"));
}

static void test_buildBackendsJson_parses_and_agrees_with_itself() {
    TEST("buildBackendsJson — valid JSON, flat fields agree with the detail block");
    std::vector<BackendRuntimeStatus> statuses = {
        {BACKEND_ID_VIGEM, true, ""},
        {BACKEND_ID_HIDMAESTRO, false, "DRIVER_MISSING"},
    };
    Json parsed;
    EXPECT(jsonParse(buildBackendsJson(statuses), parsed));
    EXPECT(parsed.is_array());
    EXPECT_EQ(parsed.size(), static_cast<size_t>(2));

    const Json& vigem = parsed[0];
    EXPECT_EQ(vigem["id"].get<std::string>(), std::string("vigem"));
    EXPECT(vigem["errorCode"].is_null());
    EXPECT(vigem["driverVersion"].is_null());
    EXPECT_EQ(vigem["lifecycle"].get<std::string>(), std::string("eol"));
    EXPECT_EQ(vigem["eolDate"].get<std::string>(), std::string("2023-11-02"));
    EXPECT(vigem["controllers"].is_array());

    const Json& xbox = vigem["controllers"][0];
    EXPECT(xbox["motionRequires"].is_null());
    EXPECT_EQ(xbox["latency"].get<std::string>(), std::string("lowest"));
    const Json& sl = xbox["submitLatency"];
    EXPECT_EQ(sl["submitPath"].get<std::string>(), std::string("kernel-direct"));
    EXPECT_EQ(sl["score"].get<int>(), 2);
    EXPECT_EQ(sl["nominalUs"].get<int>(), 2);
    EXPECT_EQ(sl["tailUs"].get<int>(), 5);
    EXPECT_EQ(sl["facts"]["kernelCrossings"].get<int>(), 1);
    EXPECT_EQ(sl["facts"]["threadWakeups"].get<int>(), 0);
    EXPECT_EQ(xbox["latencyRank"].get<int>(), sl["rank"].get<int>());
    EXPECT_EQ(xbox["latency"].get<std::string>(), sl["tier"].get<std::string>());

    const Json& hm = parsed[1];
    EXPECT_EQ(hm["errorCode"].get<std::string>(), std::string("DRIVER_MISSING"));
    EXPECT_EQ(hm["lifecycle"].get<std::string>(), std::string("supported"));
    EXPECT(hm["eolDate"].is_null());
    const Json& hmXbox = hm["controllers"][0];
    EXPECT_EQ(hmXbox["submitLatency"]["facts"]["threadWakeups"].get<int>(), 2);
    EXPECT_EQ(hmXbox["latencyRank"].get<int>(), 2);
    const Json& hmDs4 = hm["controllers"][1];
    EXPECT_EQ(hmDs4["submitLatency"]["facts"]["threadWakeups"].get<int>(), 1);
    EXPECT_EQ(hmDs4["latencyRank"].get<int>(), 1);
    EXPECT_EQ(hmDs4["motionRequires"].get<std::string>(), std::string("hidmaestro>=1.7"));
}

int main() {
    test_latencyTier_names_and_ranks();
    test_buildBackendsJson_parses_and_agrees_with_itself();
    test_estimateCost_matches_the_documented_table();
    test_derived_tier_separates_rows_within_one_backend();
    test_buildBackendsJson_lifecycle_and_submit_latency();
    test_buildBackendsJson_stays_backwards_compatible();
    test_descriptorById_known_and_unknown();
    test_cross_backend_latency_is_comparable();
    test_hidmaestro_widens_the_windows_type_set();
    test_buildBackendsJson_available_backend();
    test_buildBackendsJson_unavailable_backend();
    test_buildBackendsJson_skips_unknown_ids();
    test_buildBackendsJson_empty_and_multi();
    test_deriveCatalogTraits_windows_union();
    test_deriveCatalogTraits_order_decides_requires();
    test_uinput_advertises_no_lightbar_it_cannot_deliver();
    test_rawOutput_surfaces_hidmaestro_only();
    test_controllerAudio_surfaces_hidmaestroSonyOnly();
    test_deriveCatalogTraits_uinput_matches_legacy();
    test_deriveCatalogTraits_machid_matches_legacy();
    test_deriveCatalogTraits_ignores_availability();
    test_deriveCatalogTraits_empty_and_unknown();

    std::cout << "backend_registry: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
