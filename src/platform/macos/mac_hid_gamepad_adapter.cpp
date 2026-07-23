// SPDX-License-Identifier: LGPL-3.0-or-later
// IOKit shell for the macOS virtual-DS4 backend. All byte shaping lives in
// ds4_report.h (pure); this file owns IOHIDUserDevice lifecycles, the
// per-device dispatch queue, and the set/get-report plumbing. Locking
// protocol is documented in mac_hid_gamepad_adapter.h.
#include "mac_hid_gamepad_adapter.h"

#include "ds4_report.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <vector>

// SATELLITE_HAS_IOHIDUSERDEVICE comes from the header (shared with the test
// binaries); the stub branch below preserves the historical inert behavior
// when the SDK lacks the API.
#if SATELLITE_HAS_IOHIDUSERDEVICE

#if __has_include(<IOKit/hidsystem/IOHIDUserDevice.h>)
#include <IOKit/hidsystem/IOHIDUserDevice.h>
#else
#include <IOKit/hid/IOHIDUserDevice.h>
#endif

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDUsageTables.h>
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>

namespace {

inline bool isValidSerial(uint32_t serial) {
    return serial >= 1 && serial <= MAX_BACKEND_CONTROLLERS;
}

void setDictNumber(CFMutableDictionaryRef dict, CFStringRef key, int32_t value) {
    CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &value);
    if (n != nullptr) {
        CFDictionarySetValue(dict, key, n);
        CFRelease(n);
    }
}

void setDictString(CFMutableDictionaryRef dict, CFStringRef key, const char* value) {
    CFStringRef s = CFStringCreateWithCString(kCFAllocatorDefault, value, kCFStringEncodingUTF8);
    if (s != nullptr) {
        CFDictionarySetValue(dict, key, s);
        CFRelease(s);
    }
}

// Minimal vendor-page descriptor for the entitlement probe. Deliberately NOT
// the DS4 descriptor: the probe device exists for a few milliseconds and a
// transient "DualShock 4" appearing in the HID system could confuse games or
// the OS gamepad UI; a vendor-defined usage is adopted by nothing.
const uint8_t kProbeDescriptor[] = {
    0x06, 0x00, 0xFF, // Usage Page (Vendor 0xFF00)
    0x09, 0x01,       // Usage (0x01)
    0xA1, 0x01,       // Collection (Application)
    0x15, 0x00,       //   Logical Minimum (0)
    0x26, 0xFF, 0x00, //   Logical Maximum (255)
    0x75, 0x08,       //   Report Size (8)
    0x95, 0x01,       //   Report Count (1)
    0x09, 0x01,       //   Usage (0x01)
    0x81, 0x02,       //   Input (Data,Var,Abs)
    0xC0,             // End Collection
};

// Attempt one minimal create. NULL means no entitlement (or HID stack
// refusal); the result is cached by runtimeAvailable() because entitlements
// cannot change within a process lifetime.
bool probeCreateOnce() {
    CFDataRef desc = CFDataCreate(kCFAllocatorDefault, kProbeDescriptor, sizeof(kProbeDescriptor));
    if (desc == nullptr) return false;
    CFMutableDictionaryRef props = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (props == nullptr) {
        CFRelease(desc);
        return false;
    }
    CFDictionarySetValue(props, CFSTR(kIOHIDReportDescriptorKey), desc);
    setDictString(props, CFSTR(kIOHIDProductKey), "Satellite Capability Probe");

    IOHIDUserDeviceRef dev = IOHIDUserDeviceCreateWithProperties(kCFAllocatorDefault, props, 0);
    const bool ok = (dev != nullptr);
    // Never scheduled/activated, so a plain release is the documented teardown.
    if (dev != nullptr) CFRelease(dev);
    CFRelease(props);
    CFRelease(desc);
    return ok;
}

// Confirmed-teardown timeout. The cancel handler normally fires in
// microseconds; a miss means something is badly wrong and the caller must
// quarantine the serial instead of freeing a possibly-live device.
const int64_t kCancelTimeoutNs = 2LL * NSEC_PER_SEC;

} // namespace

// All IOKit state for one plugged serial. Owned via unique_ptr in slots_;
// mutated only under mtx_ (see the header's locking protocol).
struct MacHidGamepadAdapter::Slot {
    IOHIDUserDeviceRef device = nullptr;
    dispatch_queue_t queue = nullptr;         // serial queue for set/get blocks
    dispatch_semaphore_t cancelled = nullptr; // signalled by the cancel handler
    bool isDS4 = false;
    Ds4InputState state{};
    uint8_t touchPacketSeq = 0; // next touch frame counter (use-then-increment)
    std::chrono::steady_clock::time_point lastSubmit{};
    uint8_t buf[DS4V2_INPUT_REPORT_BYTES] = {};
};

// Everything a kernel set-report block may touch, shared so the block can
// capture it weakly (see the header's locking protocol): a quarantined
// device outlives the adapter, and its late blocks must no-op.
struct MacHidGamepadAdapter::CallbackHub {
    std::mutex mtx;
    RumbleCallback rumbleCb;
    LightbarCallback lightbarCb;

    // Runs on a slot's dispatch queue (or a test thread). Parses the DS4
    // output report and fans it out to the two sinks, honoring the report's
    // valid flags so a colour-only write cannot zero the motors with stale
    // bytes. Callbacks are snapshotted under mtx and invoked with the lock
    // dropped.
    void dispatchOutputReport(uint32_t serial, uint32_t reportId, const uint8_t* data, size_t len) {
        const Ds4OutputReport out = ds4ParseOutputReport(reportId, data, len);
        if (!out.valid) return;

        RumbleCallback rcb;
        LightbarCallback lcb;
        {
            std::lock_guard<std::mutex> lk(mtx);
            rcb = rumbleCb;
            lcb = lightbarCb;
        }
        if (out.rumbleValid && rcb) rcb(serial, ds4RumbleFromOutput(out));
        if (out.lightbarValid && lcb) lcb(serial, out.r, out.g, out.b);
    }
};

MacHidGamepadAdapter::MacHidGamepadAdapter() : hub_(std::make_shared<CallbackHub>()) {}

MacHidGamepadAdapter::~MacHidGamepadAdapter() { closeBus(); }

bool MacHidGamepadAdapter::runtimeAvailable() {
    // -1 unknown, 0 unavailable, 1 available. The probe is attempted at most
    // once per process; probeBackend() runs per HTTP request and must stay
    // cheap and side-effect-free after the first call.
    static std::atomic<int> cached{-1};
    int v = cached.load(std::memory_order_acquire);
    if (v == -1) {
        v = probeCreateOnce() ? 1 : 0;
        cached.store(v, std::memory_order_release);
    }
    return v == 1;
}

bool MacHidGamepadAdapter::ensureBusOpen() {
    std::lock_guard<std::mutex> lk(mtx_);
    // The "bus" is the per-process ability to create IOHIDUserDevices;
    // individual devices are created per plugin, like uinput nodes.
    busOpen_ = runtimeAvailable();
    return busOpen_;
}

void MacHidGamepadAdapter::closeBus() {
    std::vector<uint32_t> serials;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        serials.reserve(slots_.size());
        for (auto& [serial, _] : slots_) serials.push_back(serial);
    }
    for (uint32_t serial : serials) (void)unplugDevice(serial);

    std::lock_guard<std::mutex> lk(mtx_);
    busOpen_ = false;
}

bool MacHidGamepadAdapter::isBusOpen() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return busOpen_;
}

bool MacHidGamepadAdapter::plugCommon(uint32_t serial, bool isDS4) {
    if (!isValidSerial(serial)) return false;

    std::lock_guard<std::mutex> lk(mtx_);
    if (!busOpen_) return false;
    if (slots_.count(serial) != 0) return false;

    // Device properties: the DS4 v2 identity, so macOS's DualShock support
    // adopts the pad. The serial string mirrors real hardware (MAC-formatted),
    // distinct per backend serial.
    CFDataRef desc = CFDataCreate(kCFAllocatorDefault, DS4V2_REPORT_DESCRIPTOR,
                                  static_cast<CFIndex>(DS4V2_REPORT_DESCRIPTOR_BYTES));
    if (desc == nullptr) return false;
    CFMutableDictionaryRef props = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (props == nullptr) {
        CFRelease(desc);
        return false;
    }
    CFDictionarySetValue(props, CFSTR(kIOHIDReportDescriptorKey), desc);
    setDictNumber(props, CFSTR(kIOHIDVendorIDKey), DS4V2_VENDOR_ID);
    setDictNumber(props, CFSTR(kIOHIDProductIDKey), DS4V2_PRODUCT_ID);
    setDictNumber(props, CFSTR(kIOHIDVersionNumberKey), DS4V2_VERSION_BCD);
    setDictString(props, CFSTR(kIOHIDManufacturerKey), DS4V2_MANUFACTURER_STRING);
    setDictString(props, CFSTR(kIOHIDProductKey), DS4V2_PRODUCT_STRING);
    char serialStr[18];
    ds4SerialString(serial, serialStr);
    setDictString(props, CFSTR(kIOHIDSerialNumberKey), serialStr);
    setDictString(props, CFSTR(kIOHIDTransportKey), "USB");
    setDictNumber(props, CFSTR(kIOHIDCountryCodeKey), 0);
    setDictNumber(props, CFSTR(kIOHIDPrimaryUsagePageKey), kHIDPage_GenericDesktop);
    setDictNumber(props, CFSTR(kIOHIDPrimaryUsageKey), kHIDUsage_GD_GamePad);
    // 250 Hz, the DS4's USB poll interval, in microseconds.
    setDictNumber(props, CFSTR(kIOHIDReportIntervalKey), 4000);

    IOHIDUserDeviceRef dev = IOHIDUserDeviceCreateWithProperties(kCFAllocatorDefault, props, 0);
    CFRelease(props);
    CFRelease(desc);
    if (dev == nullptr) {
        std::fprintf(stderr,
                     "satellite: IOHIDUserDevice create failed for serial=%u; "
                     "virtual pad unavailable\n",
                     serial);
        return false;
    }

    auto slot = std::make_unique<Slot>();
    slot->device = dev;
    slot->isDS4 = isDS4;
    char queueName[64];
    std::snprintf(queueName, sizeof(queueName), "com.tinkernorth.satellite.machid.%u", serial);
    slot->queue = dispatch_queue_create(queueName, DISPATCH_QUEUE_SERIAL);
    slot->cancelled = dispatch_semaphore_create(0);
    if (slot->queue == nullptr || slot->cancelled == nullptr) {
        if (slot->queue != nullptr) dispatch_release(slot->queue);
        if (slot->cancelled != nullptr) dispatch_release(slot->cancelled);
        CFRelease(dev);
        return false;
    }

    // Output reports (rumble + lightbar) from whatever game adopted the pad.
    // Registered before activation per the IOHIDUserDevice contract. The block
    // (copied by the API) captures the hub weakly, never `this`: on the
    // cancel-timeout quarantine path the device outlives the adapter, and a
    // late invocation must degrade to a no-op.
    std::weak_ptr<CallbackHub> weakHub = hub_;
    IOHIDUserDeviceRegisterSetReportBlock(dev, ^IOReturn(IOHIDReportType type, uint32_t reportID,
                                                         const uint8_t* report,
                                                         CFIndex reportLength) {
      if (type != kIOHIDReportTypeOutput || report == nullptr || reportLength <= 0)
          return kIOReturnUnsupported;
      std::shared_ptr<CallbackHub> hub = weakHub.lock();
      if (hub == nullptr) return kIOReturnUnsupported;
      hub->dispatchOutputReport(serial, reportID, report, static_cast<size_t>(reportLength));
      return kIOReturnSuccess;
    });

    // Feature-report reads (calibration / firmware / pairing): served from the
    // pure blob table so DS4 drivers complete their adoption handshake.
    IOHIDUserDeviceRegisterGetReportBlock(dev, ^IOReturn(IOHIDReportType type, uint32_t reportID,
                                                         uint8_t* report, CFIndex* reportLength) {
      if (type != kIOHIDReportTypeFeature || report == nullptr || reportLength == nullptr)
          return kIOReturnUnsupported;
      uint8_t blob[64];
      const size_t n = ds4FeatureReport(reportID, serial, blob);
      if (n == 0) return kIOReturnUnsupported;
      size_t copy = n;
      if (*reportLength >= 0 && static_cast<size_t>(*reportLength) < copy)
          copy = static_cast<size_t>(*reportLength);
      std::memcpy(report, blob, copy);
      *reportLength = static_cast<CFIndex>(copy);
      return kIOReturnSuccess;
    });

    dispatch_semaphore_t cancelled = slot->cancelled;
    IOHIDUserDeviceSetDispatchQueue(dev, slot->queue);
    IOHIDUserDeviceSetCancelHandler(dev, ^{ dispatch_semaphore_signal(cancelled); });
    IOHIDUserDeviceActivate(dev);

    // Prime with one neutral report (centred sticks, hat released, battery
    // wired-full) so the pad is not a stuck corner before the first frame,
    // mirroring the ViGEm plug-in centring.
    (void)submitLocked(*slot);

    slots_.emplace(serial, std::move(slot));
    return true;
}

bool MacHidGamepadAdapter::pluginDevice(uint32_t serial, GamepadIdentity identity) {
    // macOS adopts no XUSB-shaped HID device, so an Xbox-typed slot publishes the
    // same DS4 hardware identity; isDS4 only gates motion/touch/battery routing.
    return plugCommon(serial, /*isDS4=*/identity != GamepadIdentity::Xbox);
}

bool MacHidGamepadAdapter::supportsIdentity(GamepadIdentity identity) const {
    // Entitled IOHIDUserDevice materializes Xbox (as DS4) and DS4. DualSense (own
    // report codec) and Switch Pro (subcommand handshake) are not yet wired here.
    return identity == GamepadIdentity::Xbox || identity == GamepadIdentity::DS4;
}

bool MacHidGamepadAdapter::unplugDevice(uint32_t serial) {
    if (!isValidSerial(serial)) return true; // nothing to remove

    std::unique_ptr<Slot> slot;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = slots_.find(serial);
        if (it == slots_.end()) return true; // never plugged, already gone
        slot = std::move(it->second);
        slots_.erase(it);
        // From here no submit can reach the device: submits look the slot up
        // in the map under mtx_.
    }

    // Cancel + wait OUTSIDE mtx_ (an in-flight set-report block may be
    // contending for mtx_ to snapshot callbacks; holding it here deadlocks).
    IOHIDUserDeviceCancel(slot->device);
    const long rc = dispatch_semaphore_wait(slot->cancelled,
                                            dispatch_time(DISPATCH_TIME_NOW, kCancelTimeoutNs));
    if (rc != 0) {
        // Unconfirmed teardown: deliberately leak the refs (freeing them under
        // a possibly-live callback risks a use-after-free) and report failure
        // so SessionService quarantines the serial.
        std::fprintf(stderr,
                     "satellite: IOHIDUserDevice cancel timed out for serial=%u; "
                     "serial will be quarantined\n",
                     serial);
        (void)slot.release();
        return false;
    }
    CFRelease(slot->device);
    dispatch_release(slot->queue);
    dispatch_release(slot->cancelled);
    return true;
}

bool MacHidGamepadAdapter::isDevicePlugged(uint32_t serial) const {
    std::lock_guard<std::mutex> lk(mtx_);
    return slots_.find(serial) != slots_.end();
}

// Caller holds mtx_. Advances the free-running DS4 timestamp exactly like the
// Windows adapter (~5.33 us/unit, 16/3), packs the current state, and hands
// the report to the kernel. The 6-bit frame counter advances per packed
// report, as real hardware does per USB frame.
bool MacHidGamepadAdapter::submitLocked(Slot& slot) {
    if (slot.device == nullptr) return false;
    const auto now = std::chrono::steady_clock::now();
    if (slot.lastSubmit.time_since_epoch().count() != 0) {
        const auto us =
            std::chrono::duration_cast<std::chrono::microseconds>(now - slot.lastSubmit).count();
        slot.state.timestamp = static_cast<uint16_t>(slot.state.timestamp + (us * 3) / 16);
    }
    slot.lastSubmit = now;

    ds4PackInputReport(slot.state, slot.buf);
    slot.state.frameCounter = static_cast<uint8_t>((slot.state.frameCounter + 1) & 0x3F);

    const IOReturn ret = IOHIDUserDeviceHandleReportWithTimeStamp(
        slot.device, mach_absolute_time(), slot.buf, DS4V2_INPUT_REPORT_BYTES);
    return ret == kIOReturnSuccess;
}

bool MacHidGamepadAdapter::submitReport(uint32_t serial, const GamepadReport& report) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = slots_.find(serial);
    if (it == slots_.end()) return false;
    it->second->state.pad = report;
    return submitLocked(*it->second);
}

bool MacHidGamepadAdapter::submitMotion(uint32_t serial, const MotionReport& report) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = slots_.find(serial);
    if (it == slots_.end() || !it->second->isDS4) return false;
    // Wire scale == DS4 raw scale; the calibration feature report makes
    // consumer scaling the identity (see ds4_report.h).
    it->second->state.motion = report;
    return submitLocked(*it->second);
}

bool MacHidGamepadAdapter::supportsMotionForType(uint8_t controllerType) const {
    // Backend-shape, not per-serial: the motion-capable supported types route
    // motion, matching the ViGEm and uinput adapters. (Unobservable unentitled:
    // callers gate on ctrl.active, which needs an open bus that never opens
    // without the entitlement.)
    return supportsIdentity(controllerIdentity(controllerType)) &&
           controllerTypeHasMotion(controllerType);
}

bool MacHidGamepadAdapter::motionBackendOk(uint32_t serial) const {
    // Unknown serial (unplug/query race) reads true so the web UI never shows
    // a phantom "broken backend" badge; plugged Xbox-typed slots have no IMU
    // surface (parity with ViGEm); plugged DS4 slots always do, because the
    // IMU rides the one input report rather than a separate sink.
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = slots_.find(serial);
    if (it == slots_.end()) return true;
    return it->second->isDS4;
}

bool MacHidGamepadAdapter::submitBattery(uint32_t serial, const BatteryReport& report) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = slots_.find(serial);
    if (it == slots_.end() || !it->second->isDS4) return false;
    it->second->state.batteryByte = ds4BatteryByte(report);
    return submitLocked(*it->second);
}

bool MacHidGamepadAdapter::submitTouchpad(uint32_t serial, const TouchpadReport& report) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = slots_.find(serial);
    if (it == slots_.end() || !it->second->isDS4) return false;
    Slot& slot = *it->second;
    Ds4InputState& st = slot.state;

    // Fresh tracking id on each up->down transition so consumers read a new
    // contact rather than a teleporting drag (same policy as ViGEm/uinput).
    if (report.finger0.active && !st.finger0.active)
        st.touchTrackingId0 = static_cast<uint8_t>((st.touchTrackingId0 + 1) & 0x7F);
    if (report.finger1.active && !st.finger1.active)
        st.touchTrackingId1 = static_cast<uint8_t>((st.touchTrackingId1 + 1) & 0x7F);
    st.finger0 = report.finger0;
    st.finger1 = report.finger1;
    st.touchpadButtonPressed = report.buttonPressed;
    // Use-then-increment: the packed frame keeps this counter value until the
    // next touch sample, so plain gamepad frames re-send the same touch frame
    // and consumers correctly treat it as stale.
    st.touchPacketCounter = slot.touchPacketSeq++;
    return submitLocked(slot);
}

void MacHidGamepadAdapter::setRumbleCallback(RumbleCallback cb) {
    std::lock_guard<std::mutex> lk(hub_->mtx);
    hub_->rumbleCb = std::move(cb);
}

void MacHidGamepadAdapter::setLightbarCallback(LightbarCallback cb) {
    std::lock_guard<std::mutex> lk(hub_->mtx);
    hub_->lightbarCb = std::move(cb);
}

void MacHidGamepadAdapter::handleOutputReport(uint32_t serial, uint32_t reportId,
                                              const uint8_t* data, size_t len) {
    hub_->dispatchOutputReport(serial, reportId, data, len);
}

#ifdef SATELLITE_BUILD_TESTS
void MacHidGamepadAdapter::injectOutputReportForTest(uint32_t serial, const uint8_t* data,
                                                     size_t len) {
    handleOutputReport(serial, DS4V2_OUTPUT_REPORT_ID, data, len);
}

std::weak_ptr<void> MacHidGamepadAdapter::callbackHubForTest() const { return hub_; }
#endif

#else // !SATELLITE_HAS_IOHIDUSERDEVICE

// SDK without IOHIDUserDevice: behavior-identical to the historical macOS
// stub. The bus reports unavailable and plug-in/submit are refused;
// SessionService still runs end-to-end and clients learn no controllers can
// attach via the backendUnavailable apply result.

struct MacHidGamepadAdapter::Slot {};
struct MacHidGamepadAdapter::CallbackHub {};

MacHidGamepadAdapter::MacHidGamepadAdapter() = default;
MacHidGamepadAdapter::~MacHidGamepadAdapter() = default;

bool MacHidGamepadAdapter::runtimeAvailable() { return false; }
bool MacHidGamepadAdapter::ensureBusOpen() { return false; }
void MacHidGamepadAdapter::closeBus() {}
bool MacHidGamepadAdapter::isBusOpen() const { return false; }
bool MacHidGamepadAdapter::plugCommon(uint32_t, bool) { return false; }
bool MacHidGamepadAdapter::pluginDevice(uint32_t, GamepadIdentity) { return false; }
bool MacHidGamepadAdapter::supportsIdentity(GamepadIdentity) const { return false; }
bool MacHidGamepadAdapter::unplugDevice(uint32_t) { return true; } // nothing existed to remove
bool MacHidGamepadAdapter::isDevicePlugged(uint32_t) const { return false; }
bool MacHidGamepadAdapter::submitLocked(Slot&) { return false; }
bool MacHidGamepadAdapter::submitReport(uint32_t, const GamepadReport&) { return false; }
bool MacHidGamepadAdapter::submitMotion(uint32_t, const MotionReport&) { return false; }
bool MacHidGamepadAdapter::supportsMotionForType(uint8_t) const { return false; }
bool MacHidGamepadAdapter::motionBackendOk(uint32_t) const { return true; }
bool MacHidGamepadAdapter::submitBattery(uint32_t, const BatteryReport&) { return false; }
bool MacHidGamepadAdapter::submitTouchpad(uint32_t, const TouchpadReport&) { return false; }
void MacHidGamepadAdapter::setRumbleCallback(RumbleCallback) {}
void MacHidGamepadAdapter::setLightbarCallback(LightbarCallback) {}
void MacHidGamepadAdapter::handleOutputReport(uint32_t, uint32_t, const uint8_t*, size_t) {}

#ifdef SATELLITE_BUILD_TESTS
void MacHidGamepadAdapter::injectOutputReportForTest(uint32_t, const uint8_t*, size_t) {}
std::weak_ptr<void> MacHidGamepadAdapter::callbackHubForTest() const { return {}; }
#endif

#endif // SATELLITE_HAS_IOHIDUSERDEVICE
