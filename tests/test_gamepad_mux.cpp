// SPDX-License-Identifier: LGPL-3.0-or-later

// core/gamepad_mux — the composite port that shares one serial space across
// preference-ordered backends. Pure, driven entirely by mock ports: routing,
// plug fallback, owner bookkeeping across unplug/quarantine, callback fan-in,
// and the static-union identity gate.
#include "test_util.h"

#include "../src/core/gamepad_mux.h"

#include <set>
#include <string>
#include <vector>

using namespace satellite;

namespace {

struct MockPort : IGamepadPort {
    std::string name;
    std::set<GamepadIdentity> identities;
    bool busOpen = false;
    bool ensureBusOpenResult = true;
    bool pluginResult = true;
    bool unplugResult = true;
    bool relMouse = false;
    bool motionForType = false;

    int ensureBusOpenCalls = 0;
    int closeBusCalls = 0;
    int pluginCalls = 0;
    int unplugCalls = 0;
    int submitCalls = 0;
    int motionCalls = 0;
    int batteryCalls = 0;
    int touchpadCalls = 0;
    int relMouseCalls = 0;
    uint32_t lastSerial = 0;
    std::set<uint32_t> plugged;
    RumbleCallback rumbleCb;
    LightbarCallback lightbarCb;

    explicit MockPort(std::string n, std::set<GamepadIdentity> ids)
        : name(std::move(n)), identities(std::move(ids)) {}

    bool ensureBusOpen() override {
        ensureBusOpenCalls++;
        busOpen = ensureBusOpenResult;
        return busOpen;
    }
    void closeBus() override {
        closeBusCalls++;
        busOpen = false;
    }
    bool isBusOpen() const override { return busOpen; }
    const char* backendId() const override { return name.c_str(); }
    bool supportsIdentity(GamepadIdentity identity) const override {
        return identities.count(identity) != 0;
    }
    bool pluginDevice(uint32_t serial, GamepadIdentity) override {
        pluginCalls++;
        lastSerial = serial;
        if (!pluginResult) return false;
        plugged.insert(serial);
        return true;
    }
    bool unplugDevice(uint32_t serial) override {
        unplugCalls++;
        if (!unplugResult) return false;
        plugged.erase(serial);
        return true;
    }
    bool isDevicePlugged(uint32_t serial) const override { return plugged.count(serial) != 0; }
    bool submitReport(uint32_t serial, const GamepadReport&) override {
        submitCalls++;
        lastSerial = serial;
        return true;
    }
    bool submitMotion(uint32_t serial, const MotionReport&) override {
        motionCalls++;
        lastSerial = serial;
        return true;
    }
    bool submitBattery(uint32_t serial, const BatteryReport&) override {
        batteryCalls++;
        lastSerial = serial;
        return true;
    }
    bool submitTouchpad(uint32_t serial, const TouchpadReport&) override {
        touchpadCalls++;
        lastSerial = serial;
        return true;
    }
    bool submitRelativeMouse(int, int, bool) override {
        relMouseCalls++;
        return true;
    }
    bool supportsRelativeMouse() const override { return relMouse; }
    bool supportsMotionForType(uint8_t) const override { return motionForType; }
    bool motionBackendOk(uint32_t) const override { return true; }
    void setRumbleCallback(RumbleCallback cb) override { rumbleCb = std::move(cb); }
    void setLightbarCallback(LightbarCallback cb) override { lightbarCb = std::move(cb); }
};

MockPort makeVigemLike(const char* name = "vigem") {
    return MockPort(name, {GamepadIdentity::Xbox, GamepadIdentity::DS4});
}

MockPort makeHidMaestroLike(const char* name = "hidmaestro") {
    return MockPort(name, {GamepadIdentity::Xbox, GamepadIdentity::DS4, GamepadIdentity::DualSense,
                           GamepadIdentity::SwitchPro});
}

} // namespace

static void test_identity_union() {
    TEST("supportsIdentity is the static union of the children");
    MockPort a = makeVigemLike();
    MockPort b = makeHidMaestroLike();
    GamepadMux mux({&a, &b});
    EXPECT(mux.supportsIdentity(GamepadIdentity::Xbox));
    EXPECT(mux.supportsIdentity(GamepadIdentity::DS4));
    EXPECT(mux.supportsIdentity(GamepadIdentity::DualSense));
    EXPECT(mux.supportsIdentity(GamepadIdentity::SwitchPro));

    GamepadMux vigemOnly({&a});
    EXPECT(!vigemOnly.supportsIdentity(GamepadIdentity::DualSense));
}

static void test_bus_lifecycle_any_child() {
    TEST("ensureBusOpen/isBusOpen succeed when ANY child does; closeBus hits all");
    MockPort a = makeVigemLike();
    MockPort b = makeHidMaestroLike();
    a.ensureBusOpenResult = false;
    GamepadMux mux({&a, &b});

    EXPECT(!mux.isBusOpen());
    EXPECT(mux.ensureBusOpen());
    EXPECT_EQ(a.ensureBusOpenCalls, 1);
    EXPECT_EQ(b.ensureBusOpenCalls, 1);
    EXPECT(mux.isBusOpen());

    mux.closeBus();
    EXPECT_EQ(a.closeBusCalls, 1);
    EXPECT_EQ(b.closeBusCalls, 1);
    EXPECT(!mux.isBusOpen());

    a.ensureBusOpenResult = false;
    b.ensureBusOpenResult = false;
    EXPECT(!mux.ensureBusOpen());
}

static void test_plug_prefers_first_supporting_child() {
    TEST("pluginDevice routes to the first child supporting the identity");
    MockPort a = makeVigemLike();
    MockPort b = makeHidMaestroLike();
    GamepadMux mux({&a, &b});

    EXPECT(mux.pluginDevice(1, GamepadIdentity::Xbox));
    EXPECT_EQ(a.pluginCalls, 1);
    EXPECT_EQ(b.pluginCalls, 0);
    EXPECT(mux.ownerOf(1) == &a);

    EXPECT(mux.pluginDevice(2, GamepadIdentity::DualSense));
    EXPECT_EQ(a.pluginCalls, 1); // vigem never asked for an identity it lacks
    EXPECT_EQ(b.pluginCalls, 1);
    EXPECT(mux.ownerOf(2) == &b);
}

static void test_plug_falls_back_when_preferred_refuses() {
    TEST("pluginDevice falls back to the next child when the first refuses");
    MockPort a = makeVigemLike();
    MockPort b = makeHidMaestroLike();
    a.pluginResult = false;
    GamepadMux mux({&a, &b});

    EXPECT(mux.pluginDevice(3, GamepadIdentity::Xbox));
    EXPECT_EQ(a.pluginCalls, 1);
    EXPECT_EQ(b.pluginCalls, 1);
    EXPECT(mux.ownerOf(3) == &b);
}

static void test_plug_fails_when_no_child_accepts() {
    TEST("pluginDevice fails cleanly when every supporting child refuses");
    MockPort a = makeVigemLike();
    MockPort b = makeHidMaestroLike();
    a.pluginResult = false;
    b.pluginResult = false;
    GamepadMux mux({&a, &b});

    EXPECT(!mux.pluginDevice(4, GamepadIdentity::DS4));
    EXPECT(mux.ownerOf(4) == nullptr);
    EXPECT(!mux.isDevicePlugged(4));
}

static void test_plug_rejects_invalid_serials() {
    TEST("pluginDevice bounds-checks the serial");
    MockPort a = makeVigemLike();
    GamepadMux mux({&a});
    EXPECT(!mux.pluginDevice(0, GamepadIdentity::Xbox));
    EXPECT(!mux.pluginDevice(MAX_BACKEND_CONTROLLERS + 1, GamepadIdentity::Xbox));
    EXPECT_EQ(a.pluginCalls, 0);
}

static void test_per_serial_routing() {
    TEST("submits route to the owning child only");
    MockPort a = makeVigemLike();
    MockPort b = makeHidMaestroLike();
    GamepadMux mux({&a, &b});
    EXPECT(mux.pluginDevice(1, GamepadIdentity::Xbox));      // a
    EXPECT(mux.pluginDevice(2, GamepadIdentity::DualSense)); // b

    GamepadReport rpt{};
    EXPECT(mux.submitReport(1, rpt));
    EXPECT(mux.submitReport(2, rpt));
    EXPECT_EQ(a.submitCalls, 1);
    EXPECT_EQ(b.submitCalls, 1);
    EXPECT_EQ(a.lastSerial, 1u);
    EXPECT_EQ(b.lastSerial, 2u);

    MotionReport motion{};
    BatteryReport battery{};
    TouchpadReport touch{};
    EXPECT(mux.submitMotion(2, motion));
    EXPECT(mux.submitBattery(2, battery));
    EXPECT(mux.submitTouchpad(2, touch));
    EXPECT_EQ(b.motionCalls, 1);
    EXPECT_EQ(b.batteryCalls, 1);
    EXPECT_EQ(b.touchpadCalls, 1);
    EXPECT_EQ(a.motionCalls, 0);

    // Unowned serial: no child sees the call.
    EXPECT(!mux.submitReport(9, rpt));
    EXPECT_EQ(a.submitCalls, 1);
    EXPECT_EQ(b.submitCalls, 1);
}

static void test_unplug_clears_owner() {
    TEST("unplugDevice releases the owner so the serial can rebind elsewhere");
    MockPort a = makeVigemLike();
    MockPort b = makeHidMaestroLike();
    GamepadMux mux({&a, &b});
    EXPECT(mux.pluginDevice(1, GamepadIdentity::Xbox));
    EXPECT(mux.unplugDevice(1));
    EXPECT(mux.ownerOf(1) == nullptr);
    EXPECT(!mux.isDevicePlugged(1));

    // Same serial can now land on the other child.
    a.pluginResult = false;
    EXPECT(mux.pluginDevice(1, GamepadIdentity::Xbox));
    EXPECT(mux.ownerOf(1) == &b);
}

static void test_unplug_unconfirmed_keeps_owner() {
    TEST("a false unplug keeps the owner: the quarantined serial still routes");
    MockPort a = makeVigemLike();
    GamepadMux mux({&a});
    EXPECT(mux.pluginDevice(1, GamepadIdentity::Xbox));
    a.unplugResult = false;
    EXPECT(!mux.unplugDevice(1));
    EXPECT(mux.ownerOf(1) == &a);

    a.unplugResult = true;
    EXPECT(mux.unplugDevice(1));
    EXPECT(mux.ownerOf(1) == nullptr);
}

static void test_unplug_unowned_serial_is_gone() {
    TEST("unplugging a serial nobody owns reports device-gone");
    MockPort a = makeVigemLike();
    GamepadMux mux({&a});
    EXPECT(mux.unplugDevice(7));
    EXPECT_EQ(a.unplugCalls, 0);
}

static void test_callbacks_fan_in() {
    TEST("rumble/lightbar callbacks reach both children and fan into one sink");
    MockPort a = makeVigemLike();
    MockPort b = makeHidMaestroLike();
    GamepadMux mux({&a, &b});

    std::vector<uint32_t> rumbleSerials;
    std::vector<uint32_t> lightbarSerials;
    mux.setRumbleCallback(
        [&](uint32_t serial, const RumbleReport&) { rumbleSerials.push_back(serial); });
    mux.setLightbarCallback(
        [&](uint32_t serial, uint8_t, uint8_t, uint8_t) { lightbarSerials.push_back(serial); });

    EXPECT(a.rumbleCb != nullptr);
    EXPECT(b.rumbleCb != nullptr);
    RumbleReport r{};
    a.rumbleCb(1, r);
    b.rumbleCb(2, r);
    EXPECT_EQ(rumbleSerials.size(), static_cast<size_t>(2));
    EXPECT_EQ(rumbleSerials[0], 1u);
    EXPECT_EQ(rumbleSerials[1], 2u);

    b.lightbarCb(2, 10, 20, 30);
    EXPECT_EQ(lightbarSerials.size(), static_cast<size_t>(1));
    EXPECT_EQ(lightbarSerials[0], 2u);
}

static void test_relative_mouse_first_supporting() {
    TEST("relative mouse goes to the first child that supports it");
    MockPort a = makeVigemLike();
    MockPort b = makeHidMaestroLike();
    b.relMouse = true;
    GamepadMux mux({&a, &b});
    EXPECT(mux.supportsRelativeMouse());
    EXPECT(mux.submitRelativeMouse(1, 2, false));
    EXPECT_EQ(a.relMouseCalls, 0);
    EXPECT_EQ(b.relMouseCalls, 1);

    MockPort c = makeVigemLike("c");
    GamepadMux none({&c});
    EXPECT(!none.supportsRelativeMouse());
    EXPECT(!none.submitRelativeMouse(1, 2, false));
    EXPECT_EQ(c.relMouseCalls, 0);
}

static void test_motion_for_type_follows_preference() {
    TEST("supportsMotionForType asks the child the plug router would pick");
    MockPort a = makeVigemLike();
    MockPort b = makeHidMaestroLike();
    a.motionForType = true;
    b.motionForType = true;
    GamepadMux mux({&a, &b});
    // DS4 → vigem-like child answers; DualSense → only the hidmaestro-like
    // child supports the identity, so its answer is used.
    EXPECT(mux.supportsMotionForType(CONTROLLER_TYPE_PLAYSTATION));
    EXPECT(mux.supportsMotionForType(CONTROLLER_TYPE_DUALSENSE));
    b.motionForType = false;
    EXPECT(mux.supportsMotionForType(CONTROLLER_TYPE_PLAYSTATION));
    EXPECT(!mux.supportsMotionForType(CONTROLLER_TYPE_DUALSENSE));

    GamepadMux vigemOnly({&a});
    EXPECT(!vigemOnly.supportsMotionForType(CONTROLLER_TYPE_DUALSENSE));
}

static void test_motion_backend_ok_owner_scoped() {
    TEST("motionBackendOk defers to the owner; unowned serials stay ok");
    MockPort a = makeVigemLike();
    GamepadMux mux({&a});
    EXPECT(mux.motionBackendOk(5));
    EXPECT(mux.pluginDevice(5, GamepadIdentity::DS4));
    EXPECT(mux.motionBackendOk(5));
}

static void test_backend_id_for_serial_names_the_owner() {
    TEST("backendIdForSerial names the child that took the plug");
    MockPort a("vigem", {GamepadIdentity::Xbox, GamepadIdentity::DS4});
    MockPort b("hidmaestro", {GamepadIdentity::Xbox, GamepadIdentity::DualSense});
    GamepadMux mux({&a, &b});

    EXPECT_EQ(std::string(mux.backendIdForSerial(1)), std::string(""));
    EXPECT(mux.pluginDevice(1, GamepadIdentity::Xbox));
    EXPECT_EQ(std::string(mux.backendIdForSerial(1)), std::string("vigem"));
    EXPECT(mux.pluginDevice(2, GamepadIdentity::DualSense));
    EXPECT_EQ(std::string(mux.backendIdForSerial(2)), std::string("hidmaestro"));
    EXPECT(mux.unplugDevice(1));
    EXPECT_EQ(std::string(mux.backendIdForSerial(1)), std::string(""));
}

static void test_preferred_backend_wins_when_it_can() {
    TEST("preferredBackend jumps a later child ahead of the default order");
    MockPort a("vigem", {GamepadIdentity::Xbox, GamepadIdentity::DS4});
    MockPort b("hidmaestro", {GamepadIdentity::Xbox, GamepadIdentity::DualSense});
    GamepadMux mux({&a, &b});

    EXPECT(mux.pluginDevicePreferring(1, GamepadIdentity::Xbox, "hidmaestro"));
    EXPECT_EQ(std::string(mux.backendIdForSerial(1)), std::string("hidmaestro"));
    EXPECT_EQ(a.pluginCalls, 0);
    EXPECT_EQ(b.pluginCalls, 1);

    EXPECT(mux.pluginDevicePreferring(2, GamepadIdentity::Xbox, ""));
    EXPECT_EQ(std::string(mux.backendIdForSerial(2)), std::string("vigem"));
}

static void test_preferred_backend_is_a_hint_not_a_mandate() {
    TEST("an unhonourable preference still plugs, on the host's own order");
    MockPort a("vigem", {GamepadIdentity::Xbox, GamepadIdentity::DS4});
    MockPort b("hidmaestro", {GamepadIdentity::Xbox, GamepadIdentity::DualSense});

    GamepadMux mux({&a, &b});
    EXPECT(mux.pluginDevicePreferring(1, GamepadIdentity::DS4, "hidmaestro"));
    EXPECT_EQ(std::string(mux.backendIdForSerial(1)), std::string("vigem"));

    MockPort c("vigem", {GamepadIdentity::Xbox});
    MockPort d("hidmaestro", {GamepadIdentity::Xbox});
    d.pluginResult = false;
    GamepadMux mux2({&c, &d});
    EXPECT(mux2.pluginDevicePreferring(2, GamepadIdentity::Xbox, "hidmaestro"));
    EXPECT_EQ(std::string(mux2.backendIdForSerial(2)), std::string("vigem"));
    EXPECT_EQ(d.pluginCalls, 1);

    MockPort e("vigem", {GamepadIdentity::Xbox});
    GamepadMux mux3({&e});
    EXPECT(mux3.pluginDevicePreferring(3, GamepadIdentity::Xbox, "does-not-exist"));
    EXPECT_EQ(std::string(mux3.backendIdForSerial(3)), std::string("vigem"));
}

int main() {
    test_identity_union();
    test_backend_id_for_serial_names_the_owner();
    test_preferred_backend_wins_when_it_can();
    test_preferred_backend_is_a_hint_not_a_mandate();
    test_bus_lifecycle_any_child();
    test_plug_prefers_first_supporting_child();
    test_plug_falls_back_when_preferred_refuses();
    test_plug_fails_when_no_child_accepts();
    test_plug_rejects_invalid_serials();
    test_per_serial_routing();
    test_unplug_clears_owner();
    test_unplug_unconfirmed_keeps_owner();
    test_unplug_unowned_serial_is_gone();
    test_callbacks_fan_in();
    test_relative_mouse_first_supporting();
    test_motion_for_type_follows_preference();
    test_motion_backend_ok_owner_scoped();

    std::cout << "gamepad_mux: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
