// SPDX-License-Identifier: LGPL-3.0-or-later

// Drives the real hidmaestro_adapter.cpp against a fake provisioner whose
// "sections" are in-process anonymous file mappings, so the seqlock submit
// path, per-profile packing, merged motion/touch/battery state, plug/unplug
// bookkeeping, and the output-ring rumble/lightbar worker run without the
// driver, the helper process, or elevation.
#include "test_util.h"

#include "../src/platform/windows/hidmaestro_adapter.h"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace satellite::hidmaestro;

namespace {

uint32_t u32at(const uint8_t* p, size_t off) {
    uint32_t v;
    std::memcpy(&v, p + off, sizeof(v));
    return v;
}

void storeU32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, sizeof(v)); }
void storeU16(uint8_t* p, uint16_t v) { std::memcpy(p, &v, sizeof(v)); }

struct FakeProvisioner : IHidMaestroProvisioner {
    struct Fake {
        HANDLE inputSection = nullptr;
        HANDLE outputSection = nullptr;
        uint8_t* inputView = nullptr;
        uint8_t* outputView = nullptr;
        HANDLE inputEvent = nullptr;
        HANDLE companionEvent = nullptr;
        HANDLE outputEvent = nullptr;
    };

    bool installedVal = true;
    bool ready = false;
    bool ensureReadyResult = true;
    bool provisionResult = true;
    bool deprovisionResult = true;
    bool withOutputSection = true;
    int provisionCalls = 0;
    int deprovisionCalls = 0;
    int shutdownCalls = 0;
    GamepadIdentity lastIdentity = GamepadIdentity::Xbox;
    std::map<uint32_t, Fake> live;

    ~FakeProvisioner() override {
        for (auto& [serial, f] : live) destroy(f);
        live.clear();
    }

    static HANDLE dup(HANDLE h) {
        HANDLE out = nullptr;
        DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &out, 0, FALSE,
                        DUPLICATE_SAME_ACCESS);
        return out;
    }

    static void destroy(Fake& f) {
        if (f.inputView) UnmapViewOfFile(f.inputView);
        if (f.outputView) UnmapViewOfFile(f.outputView);
        for (HANDLE h :
             {f.inputSection, f.outputSection, f.inputEvent, f.companionEvent, f.outputEvent}) {
            if (h) CloseHandle(h);
        }
        f = Fake{};
    }

    bool installed() const override { return installedVal; }
    bool isReady() const override { return ready; }
    bool ensureReady() override {
        if (!ensureReadyResult) return false;
        ready = true;
        return true;
    }
    void shutdown() override {
        shutdownCalls++;
        ready = false;
    }

    bool provision(uint32_t serial, GamepadIdentity identity, ProvisionResult& out) override {
        provisionCalls++;
        lastIdentity = identity;
        if (!provisionResult) return false;

        Fake f;
        f.inputSection = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                            INPUT_SECTION_SIZE, nullptr);
        f.inputView = static_cast<uint8_t*>(
            MapViewOfFile(f.inputSection, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0));
        f.inputEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        f.companionEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (withOutputSection) {
            f.outputSection = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                                 OUTPUT_SECTION_SIZE, nullptr);
            f.outputView = static_cast<uint8_t*>(
                MapViewOfFile(f.outputSection, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0));
            f.outputEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        }

        out.controllerIndex = serial;
        out.inputSection = reinterpret_cast<uint64_t>(dup(f.inputSection));
        out.inputEvent = reinterpret_cast<uint64_t>(dup(f.inputEvent));
        out.companionEvent = identity == GamepadIdentity::Xbox
                                 ? reinterpret_cast<uint64_t>(dup(f.companionEvent))
                                 : 0;
        out.outputSection = f.outputSection ? reinterpret_cast<uint64_t>(dup(f.outputSection)) : 0;
        out.outputEvent = f.outputEvent ? reinterpret_cast<uint64_t>(dup(f.outputEvent)) : 0;

        auto existing = live.find(serial);
        if (existing != live.end()) destroy(existing->second);
        live[serial] = f;
        return true;
    }

    bool deprovision(uint32_t serial) override {
        deprovisionCalls++;
        auto it = live.find(serial);
        if (it != live.end()) {
            destroy(it->second);
            live.erase(it);
        }
        return deprovisionResult;
    }

    // Publish an output packet the way the driver does, then ring the doorbell.
    void publishOutput(uint32_t serial, uint8_t source, uint8_t reportId,
                       const std::vector<uint8_t>& data) {
        Fake& f = live.at(serial);
        const uint32_t newSeq = u32at(f.outputView, 0) + 1;
        storeU32(f.outputView, newSeq);
        uint8_t* slot = f.outputView + OUTPUT_HEADER_SIZE +
                        ((newSeq - 1) % OUTPUT_RING_SLOTS) * OUTPUT_SLOT_SIZE;
        slot[OUTPUT_SLOT_SOURCE_OFFSET] = source;
        slot[OUTPUT_SLOT_REPORT_ID_OFFSET] = reportId;
        storeU16(slot + OUTPUT_SLOT_SIZE_OFFSET, static_cast<uint16_t>(data.size()));
        if (!data.empty()) std::memcpy(slot + OUTPUT_SLOT_DATA_OFFSET, data.data(), data.size());
        storeU32(slot + OUTPUT_SLOT_SEQNO_OFFSET, newSeq);
        SetEvent(f.outputEvent);
    }

    const uint8_t* inputSection(uint32_t serial) const { return live.at(serial).inputView; }
    uint32_t inputSeq(uint32_t serial) const { return u32at(live.at(serial).inputView, 0); }
    const uint8_t* inputData(uint32_t serial) const {
        return live.at(serial).inputView + INPUT_DATA_OFFSET;
    }
};

// Latching sink for rumble/lightbar callbacks with a condition variable so
// the tests wait deterministically instead of sleeping.
struct CallbackSink {
    std::mutex m;
    std::condition_variable cv;
    int rumbleCount = 0;
    int lightbarCount = 0;
    uint32_t lastSerial = 0;
    RumbleReport lastRumble{};
    uint8_t r = 0, g = 0, b = 0;

    void install(HidMaestroAdapter& adapter) {
        adapter.setRumbleCallback([this](uint32_t serial, const RumbleReport& rr) {
            std::lock_guard<std::mutex> lk(m);
            rumbleCount++;
            lastSerial = serial;
            lastRumble = rr;
            cv.notify_all();
        });
        adapter.setLightbarCallback([this](uint32_t serial, uint8_t rr, uint8_t gg, uint8_t bb) {
            std::lock_guard<std::mutex> lk(m);
            lightbarCount++;
            lastSerial = serial;
            r = rr;
            g = gg;
            b = bb;
            cv.notify_all();
        });
    }

    bool waitRumble(int atLeast, int timeoutMs = 3000) {
        std::unique_lock<std::mutex> lk(m);
        return cv.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                           [&] { return rumbleCount >= atLeast; });
    }

    bool waitLightbar(int atLeast, int timeoutMs = 3000) {
        std::unique_lock<std::mutex> lk(m);
        return cv.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                           [&] { return lightbarCount >= atLeast; });
    }
};

} // namespace

static void test_bus_open_probe_only() {
    TEST("ensureBusOpen — passive: installed-ness only, never spawns the helper");
    FakeProvisioner prov;
    prov.installedVal = false;
    HidMaestroAdapter adapter(prov);
    EXPECT(!adapter.ensureBusOpen());
    EXPECT(!adapter.isBusOpen());

    prov.installedVal = true;
    EXPECT(adapter.ensureBusOpen());
    EXPECT(adapter.isBusOpen());
    EXPECT(!prov.ready); // ensureReady untouched
}

static void test_identity_support() {
    TEST("supportsIdentity — all four identities materialize");
    FakeProvisioner prov;
    HidMaestroAdapter adapter(prov);
    EXPECT(adapter.supportsIdentity(GamepadIdentity::Xbox));
    EXPECT(adapter.supportsIdentity(GamepadIdentity::DS4));
    EXPECT(adapter.supportsIdentity(GamepadIdentity::DualSense));
    EXPECT(adapter.supportsIdentity(GamepadIdentity::SwitchPro));
}

static void test_plugin_lifecycle() {
    TEST("pluginDevice — helper spun up lazily, neutral frame written");
    FakeProvisioner prov;
    HidMaestroAdapter adapter(prov);
    EXPECT(adapter.ensureBusOpen());
    EXPECT(adapter.pluginDevice(1, GamepadIdentity::Xbox));
    EXPECT(prov.ready);
    EXPECT_EQ(prov.provisionCalls, 1);
    EXPECT(adapter.isDevicePlugged(1));
    EXPECT(!adapter.isDevicePlugged(2));
    // Neutral frame published at plug: seq even and nonzero, 18-byte report.
    EXPECT_EQ(prov.inputSeq(1), (uint32_t)2);
    EXPECT_EQ(u32at(prov.inputSection(1), INPUT_DATASIZE_OFFSET), (uint32_t)X360_REPORT_BYTES);

    EXPECT(adapter.unplugDevice(1));
    EXPECT_EQ(prov.deprovisionCalls, 1);
    EXPECT(!adapter.isDevicePlugged(1));
}

static void test_plugin_denied_elevation() {
    TEST("pluginDevice — a declined helper start fails the plug cleanly");
    FakeProvisioner prov;
    prov.ensureReadyResult = false;
    HidMaestroAdapter adapter(prov);
    EXPECT(adapter.ensureBusOpen());
    EXPECT(!adapter.pluginDevice(1, GamepadIdentity::Xbox));
    EXPECT_EQ(prov.provisionCalls, 0);
    EXPECT(!adapter.isDevicePlugged(1));
}

static void test_plugin_provision_failure() {
    TEST("pluginDevice — provision failure leaves no slot state behind");
    FakeProvisioner prov;
    prov.provisionResult = false;
    HidMaestroAdapter adapter(prov);
    EXPECT(adapter.ensureBusOpen());
    EXPECT(!adapter.pluginDevice(1, GamepadIdentity::DS4));
    EXPECT(!adapter.isDevicePlugged(1));
    GamepadReport rpt{};
    EXPECT(!adapter.submitReport(1, rpt));
}

static void test_unplug_unconfirmed() {
    TEST("unplugDevice — an unconfirmed teardown reports false (quarantine)");
    FakeProvisioner prov;
    prov.deprovisionResult = false;
    HidMaestroAdapter adapter(prov);
    EXPECT(adapter.ensureBusOpen());
    EXPECT(adapter.pluginDevice(3, GamepadIdentity::Xbox));
    EXPECT(!adapter.unplugDevice(3));
    // Unplugging a never-plugged serial is trivially gone.
    EXPECT(adapter.unplugDevice(9));
}

static void test_xbox_submit_bytes() {
    TEST("submitReport(Xbox) — section carries the packed report + GIP slice");
    FakeProvisioner prov;
    HidMaestroAdapter adapter(prov);
    EXPECT(adapter.ensureBusOpen());
    EXPECT(adapter.pluginDevice(1, GamepadIdentity::Xbox));

    GamepadReport rpt{};
    rpt.sThumbLX = 1000;
    rpt.bLeftTrigger = 255;
    rpt.wButtons = 0x1000 | 0x0400; // A + Guide
    EXPECT(adapter.submitReport(1, rpt));

    const uint8_t* sec = prov.inputSection(1);
    EXPECT_EQ(u32at(sec, INPUT_SEQNO_OFFSET), (uint32_t)4); // neutral + this frame
    EXPECT_EQ(u32at(sec, INPUT_DATASIZE_OFFSET), (uint32_t)X360_REPORT_BYTES);
    uint8_t expected[X360_REPORT_BYTES];
    packX360Report(rpt, expected);
    EXPECT_EQ(std::memcmp(sec + INPUT_DATA_OFFSET, expected, X360_REPORT_BYTES), 0);
    uint8_t expectedGip[INPUT_GIP_LENGTH];
    packGip(rpt, expectedGip);
    EXPECT_EQ(std::memcmp(sec + INPUT_GIP_OFFSET, expectedGip, INPUT_GIP_LENGTH), 0);
    EXPECT_EQ(u32at(sec, INPUT_EXT_SIZE_OFFSET), (uint32_t)0);
}

static void test_ds4_submit_merges_motion_touch_battery() {
    TEST("DS4 — motion/touch/battery merge into every subsequent frame");
    FakeProvisioner prov;
    HidMaestroAdapter adapter(prov);
    EXPECT(adapter.ensureBusOpen());
    EXPECT(adapter.pluginDevice(2, GamepadIdentity::DS4));

    MotionReport motion{};
    motion.gyroX = 100;
    EXPECT(adapter.submitMotion(2, motion));
    const uint8_t* data = prov.inputData(2);
    EXPECT_EQ(u32at(prov.inputSection(2), INPUT_DATASIZE_OFFSET), (uint32_t)DS4_PAYLOAD_BYTES);
    const int16_t gyro = static_cast<int16_t>(data[12] | (data[13] << 8));
    EXPECT_EQ(gyro, sonyImuFromWire(100));

    TouchpadReport touch{};
    touch.finger0.active = true;
    touch.buttonPressed = true;
    EXPECT(adapter.submitTouchpad(2, touch));
    EXPECT_EQ(data[34] & 0x80, 0x00); // finger 0 down
    EXPECT_EQ(data[34] & 0x7F, 1);    // first contact bumped the tracking id

    BatteryReport batt;
    batt.level = 30;
    batt.status = BATTERY_STATUS_DISCHARGING;
    EXPECT(adapter.submitBattery(2, batt));
    EXPECT_EQ(data[11], (uint8_t)3);

    // The gamepad frame keeps all of the merged state riding along.
    GamepadReport rpt{};
    rpt.wButtons = 0x1000;
    EXPECT(adapter.submitReport(2, rpt));
    EXPECT_EQ(static_cast<int16_t>(data[12] | (data[13] << 8)), sonyImuFromWire(100));
    EXPECT_EQ(data[11], (uint8_t)3);
    EXPECT_EQ(data[4] & 0x20, 0x20); // Cross
}

static void test_ds5_submit() {
    TEST("DualSense — seq counter advances, motion + touch at DS5 offsets");
    FakeProvisioner prov;
    HidMaestroAdapter adapter(prov);
    EXPECT(adapter.ensureBusOpen());
    EXPECT(adapter.pluginDevice(4, GamepadIdentity::DualSense));

    const uint8_t* data = prov.inputData(4);
    GamepadReport rpt{};
    EXPECT(adapter.submitReport(4, rpt));
    const uint8_t seqA = data[6];
    EXPECT(adapter.submitReport(4, rpt));
    EXPECT_EQ(static_cast<uint8_t>(data[6] - seqA), (uint8_t)1);
    EXPECT_EQ(u32at(prov.inputSection(4), INPUT_DATASIZE_OFFSET), (uint32_t)DS5_PAYLOAD_BYTES);

    MotionReport motion{};
    motion.accelZ = 500;
    EXPECT(adapter.submitMotion(4, motion));
    EXPECT_EQ(static_cast<int16_t>(data[25] | (data[26] << 8)), sonyImuFromWire(500));

    TouchpadReport touch{};
    touch.finger0.active = true;
    EXPECT(adapter.submitTouchpad(4, touch));
    EXPECT_EQ(data[32] & 0x80, 0x00);

    BatteryReport batt;
    batt.level = 70;
    batt.status = BATTERY_STATUS_CHARGING;
    EXPECT(adapter.submitBattery(4, batt));
    EXPECT_EQ(data[52], (uint8_t)(0x10 | 7));
}

static void test_switch_submit() {
    TEST("SwitchPro — pad and motion merge into the 48-byte body");
    FakeProvisioner prov;
    HidMaestroAdapter adapter(prov);
    EXPECT(adapter.ensureBusOpen());
    EXPECT(adapter.pluginDevice(5, GamepadIdentity::SwitchPro));

    const uint8_t* data = prov.inputData(5);
    GamepadReport rpt{};
    rpt.wButtons = 0x1000; // A (south) -> wire B
    EXPECT(adapter.submitReport(5, rpt));
    EXPECT_EQ(u32at(prov.inputSection(5), INPUT_DATASIZE_OFFSET), (uint32_t)SWITCH_BODY_BYTES);
    EXPECT_EQ(data[2], (uint8_t)0x04);

    MotionReport motion{};
    motion.accelY = 32767;
    EXPECT(adapter.submitMotion(5, motion));
    EXPECT_EQ(data[2], (uint8_t)0x04); // pad state still merged in
    const int16_t az = static_cast<int16_t>(data[12 + 4] | (data[12 + 5] << 8));
    EXPECT_EQ(az, (int16_t)16384);

    // No touchpad/battery surface on this profile.
    TouchpadReport touch{};
    EXPECT(!adapter.submitTouchpad(5, touch));
    BatteryReport batt;
    EXPECT(!adapter.submitBattery(5, batt));
}

static void test_motion_support_matrix() {
    TEST("supportsMotionForType — everything but Xbox");
    FakeProvisioner prov;
    HidMaestroAdapter adapter(prov);
    EXPECT(!adapter.supportsMotionForType(CONTROLLER_TYPE_XBOX));
    EXPECT(adapter.supportsMotionForType(CONTROLLER_TYPE_PLAYSTATION));
    EXPECT(adapter.supportsMotionForType(CONTROLLER_TYPE_DUALSENSE));
    EXPECT(adapter.supportsMotionForType(CONTROLLER_TYPE_SWITCHPRO));
    GamepadReport rpt{};
    (void)rpt;
    MotionReport motion{};
    FakeProvisioner prov2;
    HidMaestroAdapter adapter2(prov2);
    EXPECT(adapter2.ensureBusOpen());
    EXPECT(adapter2.pluginDevice(1, GamepadIdentity::Xbox));
    EXPECT(!adapter2.submitMotion(1, motion));
}

static void test_rumble_worker_xbox() {
    TEST("output worker — XUSB SET_STATE packets surface as rumble callbacks");
    FakeProvisioner prov;
    HidMaestroAdapter adapter(prov);
    CallbackSink sink;
    sink.install(adapter);
    EXPECT(adapter.ensureBusOpen());
    EXPECT(adapter.pluginDevice(1, GamepadIdentity::Xbox));

    prov.publishOutput(1, OUTPUT_SOURCE_XINPUT, 0, {0x00, 0x08, 0x40, 0x80, 0x00});
    EXPECT(sink.waitRumble(1));
    EXPECT_EQ(sink.lastSerial, (uint32_t)1);
    EXPECT_EQ(sink.lastRumble.strongMagnitude, (uint16_t)(0x40 * 257));
    EXPECT_EQ(sink.lastRumble.weakMagnitude, (uint16_t)(0x80 * 257));

    // A burst is drained in order, not coalesced.
    prov.publishOutput(1, OUTPUT_SOURCE_XINPUT, 0, {0x00, 0x08, 0x01, 0x01, 0x00});
    prov.publishOutput(1, OUTPUT_SOURCE_XINPUT, 0, {0x00, 0x08, 0x02, 0x02, 0x00});
    EXPECT(sink.waitRumble(3));
    EXPECT_EQ(sink.lastRumble.strongMagnitude, (uint16_t)(0x02 * 257));
}

static void test_rumble_and_lightbar_worker_ds4() {
    TEST("output worker — DS4 0x05 packets surface rumble + lightbar");
    FakeProvisioner prov;
    HidMaestroAdapter adapter(prov);
    CallbackSink sink;
    sink.install(adapter);
    EXPECT(adapter.ensureBusOpen());
    EXPECT(adapter.pluginDevice(2, GamepadIdentity::DS4));

    std::vector<uint8_t> report(31, 0);
    report[0] = 0x03;
    report[3] = 10;
    report[4] = 20;
    report[5] = 7;
    report[6] = 8;
    report[7] = 9;
    prov.publishOutput(2, OUTPUT_SOURCE_HID_OUTPUT, 0x05, report);
    EXPECT(sink.waitRumble(1));
    EXPECT(sink.waitLightbar(1));
    EXPECT_EQ(sink.lastSerial, (uint32_t)2);
    EXPECT_EQ(sink.lastRumble.strongMagnitude, (uint16_t)(20 * 257));
    EXPECT_EQ(sink.r, (uint8_t)7);
    EXPECT_EQ(sink.g, (uint8_t)8);
    EXPECT_EQ(sink.b, (uint8_t)9);

    // Feature-read echoes (source 3) are informational, never rumble.
    prov.publishOutput(2, OUTPUT_SOURCE_HID_FEATURE_READ, 0x05, {});
    prov.publishOutput(2, OUTPUT_SOURCE_HID_OUTPUT, 0x05, report);
    EXPECT(sink.waitRumble(2));
    EXPECT_EQ(sink.rumbleCount, 2);
}

static void test_close_bus() {
    TEST("closeBus — tears down slots, deprovisions, shuts the helper down");
    FakeProvisioner prov;
    HidMaestroAdapter adapter(prov);
    EXPECT(adapter.ensureBusOpen());
    EXPECT(adapter.pluginDevice(1, GamepadIdentity::Xbox));
    EXPECT(adapter.pluginDevice(2, GamepadIdentity::DualSense));
    adapter.closeBus();
    EXPECT(!adapter.isBusOpen());
    EXPECT_EQ(prov.deprovisionCalls, 2);
    EXPECT(prov.shutdownCalls >= 1);
    EXPECT(!adapter.isDevicePlugged(1));
    EXPECT(!adapter.isDevicePlugged(2));
}

int main() {
    test_bus_open_probe_only();
    test_identity_support();
    test_plugin_lifecycle();
    test_plugin_denied_elevation();
    test_plugin_provision_failure();
    test_unplug_unconfirmed();
    test_xbox_submit_bytes();
    test_ds4_submit_merges_motion_touch_battery();
    test_ds5_submit();
    test_switch_submit();
    test_motion_support_matrix();
    test_rumble_worker_xbox();
    test_rumble_and_lightbar_worker_ds4();
    test_close_bus();

    std::cout << "hidmaestro_adapter: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
