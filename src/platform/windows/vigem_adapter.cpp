// SPDX-License-Identifier: LGPL-3.0-or-later
#include "vigem_adapter.h"

#include "core/ds4_report.h"
#include "core/touchpad_codec.h"
#include "pointer_inject.h"
#include "vigem.h"

extern HANDLE openVigemBus();
extern bool pluginTarget(HANDLE bus, unsigned long serial);
extern bool pluginTargetDS4(HANDLE bus, unsigned long serial);
extern bool unplugTarget(HANDLE bus, unsigned long serial);
extern bool waitNextXusbNotification(HANDLE bus, unsigned long serial, HANDLE cancel,
                                     XUSB_REQUEST_NOTIFICATION& out);
extern bool waitNextDS4Notification(HANDLE bus, unsigned long serial, HANDLE cancel,
                                    DS4_REQUEST_NOTIFICATION& out);

namespace {

inline bool isValidSerial(uint32_t serial) {
    return serial >= 1 && serial <= MAX_BACKEND_CONTROLLERS;
}

} // namespace

ViGEmAdapter::ViGEmAdapter() = default;

ViGEmAdapter::~ViGEmAdapter() { closeBus(); }

bool ViGEmAdapter::ensureBusOpen() {
    std::lock_guard<std::mutex> lk(busMtx_);
    if (busHandle_ != INVALID_HANDLE_VALUE) return true;
    busHandle_ = openVigemBus();
    return busHandle_ != INVALID_HANDLE_VALUE;
}

void ViGEmAdapter::closeBus() {
    // Stop notification workers first: they hold pending IOCTLs on busHandle_.
    std::vector<uint32_t> serials;
    {
        std::lock_guard<std::mutex> lk(busMtx_);
        serials.reserve(notifWorkers_.size());
        for (auto& [serial, _] : notifWorkers_) serials.push_back(serial);
    }
    for (uint32_t serial : serials) {
        std::lock_guard<std::mutex> lk(busMtx_);
        stopNotificationWorker(serial);
    }

    std::lock_guard<std::mutex> lk(busMtx_);

    // Sync submits have all completed, so no IOCTL is in flight; safe to free
    // each slot's persistent event.
    for (uint32_t s = 1; s <= MAX_BACKEND_CONTROLLERS; s++) {
        IoSlot& slot = io_[s];
        slot.plugged.store(false, std::memory_order_release);
        if (slot.event) {
            CloseHandle(slot.event);
            slot.event = nullptr;
        }
        slot.isDS4 = false;
        slot.ds4 = {};
    }

    if (busHandle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(busHandle_);
        busHandle_ = INVALID_HANDLE_VALUE;
    }
}

bool ViGEmAdapter::isBusOpen() const {
    std::lock_guard<std::mutex> lk(busMtx_);
    return busHandle_ != INVALID_HANDLE_VALUE;
}

// Per-serial OVERLAPPED hEvent for the slot's sync submits. Created signalled
// so no teardown path can ever block on it; submit state is otherwise immaterial.
static HANDLE makeSlotEvent() {
    return CreateEventW(nullptr, /*manualReset=*/FALSE, /*initialState=*/TRUE, nullptr);
}

bool ViGEmAdapter::pluginDevice(uint32_t serial, GamepadIdentity identity) {
    if (!isValidSerial(serial)) return false;
    const bool isDS4 = identity == GamepadIdentity::DS4;
    std::lock_guard<std::mutex> lk(busMtx_);
    if (busHandle_ == INVALID_HANDLE_VALUE) return false;

    const unsigned long tgt = static_cast<unsigned long>(serial);
    const bool ok = isDS4 ? pluginTargetDS4(busHandle_, tgt) : pluginTarget(busHandle_, tgt);
    if (!ok) return false;

    IoSlot& slot = io_[serial];
    if (slot.event == nullptr) slot.event = makeSlotEvent();
    slot.isDS4 = isDS4;
    slot.ds4 = {};

    if (isDS4) {
        // Centre the sticks (0x80) so the pad isn't a stuck corner pre-first-frame.
        slot.ds4.report.Report.bThumbLX = 0x80;
        slot.ds4.report.Report.bThumbLY = 0x80;
        slot.ds4.report.Report.bThumbRX = 0x80;
        slot.ds4.report.Report.bThumbRY = 0x80;
        slot.ds4.report.Report.bBatteryLvl = 0x1B; // cable connected + fully charged
    }
    slot.plugged.store(true, std::memory_order_release);

    // Probe the EX path now so motionBackendOk(serial) reflects real IMU-sink
    // capability by the time the controller-add ACK is built (submitDS4Locked
    // latches exSupported off if this ViGEmBus is too old). Best-effort.
    if (isDS4) submitDS4Locked(serial);

    startNotificationWorker(serial, isDS4);
    return true;
}

// ViGEmBus materializes only Xbox360Wired + DualShock4Wired targets; DualSense
// and SwitchPro are impossible on this backend.
bool ViGEmAdapter::supportsIdentity(GamepadIdentity identity) const {
    return identity == GamepadIdentity::Xbox || identity == GamepadIdentity::DS4;
}

bool ViGEmAdapter::unplugDevice(uint32_t serial) {
    if (!isValidSerial(serial)) return true; // nothing to remove

    // Stop the worker first; explicit cancel keeps the unplug path deterministic.
    {
        std::lock_guard<std::mutex> lk(busMtx_);
        stopNotificationWorker(serial);
    }

    std::lock_guard<std::mutex> lk(busMtx_);
    IoSlot& slot = io_[serial];

    // A closed bus has no live targets; the device is gone by definition.
    if (busHandle_ == INVALID_HANDLE_VALUE) {
        slot.plugged.store(false, std::memory_order_release);
        slot.isDS4 = false;
        slot.ds4 = {};
        return true;
    }

    // Stop accepting submissions before unplug so a receiver can't race it.
    slot.plugged.store(false, std::memory_order_release);

    // Sync submits mean nothing holds slot.xsr/event now, so no drain wait.
    const bool ok = unplugTarget(busHandle_, static_cast<unsigned long>(serial));

    // Keep slot.event for a possible replug; final close is in closeBus.
    slot.isDS4 = false;
    slot.ds4 = {};
    return ok;
}

bool ViGEmAdapter::isDevicePlugged(uint32_t serial) const {
    if (!isValidSerial(serial)) return false;
    std::lock_guard<std::mutex> lk(busMtx_);
    if (busHandle_ == INVALID_HANDLE_VALUE) return false;
    return io_[serial].plugged.load(std::memory_order_acquire);
}

bool ViGEmAdapter::submitReport(uint32_t serial, const GamepadReport& report) {
    if (!isValidSerial(serial)) return false;

    // Route by the identity recorded at plug. DS4 folds into the running EX report
    // and submits under the held lock; Xbox holds busMtx_ only to snapshot the
    // handle + slot, then drops it for the IOCTL (buffer + event persist to closeBus).
    HANDLE bus;
    IoSlot* slot;
    {
        std::lock_guard<std::mutex> lk(busMtx_);
        if (busHandle_ == INVALID_HANDLE_VALUE) return false;
        slot = &io_[serial];
        if (slot->isDS4) return submitDS4ReportLocked(serial, report);
        if (!slot->plugged.load(std::memory_order_acquire) || slot->event == nullptr) return false;
        bus = busHandle_;
    }

    // GamepadReport and XUSB_REPORT are binary-compatible: one memcpy, no copy.
    static_assert(sizeof(GamepadReport) == sizeof(XUSB_REPORT),
                  "GamepadReport and XUSB_REPORT must match");
    return submitXusbSync(bus, static_cast<unsigned long>(serial), slot->xsr, slot->event, &report);
}

// ViGEm's DS4 report is the DualShock 4 report the core packs for every DS4
// shell, so the one button layout serves here too. These hold ViGEm's names to
// that layout: if the bus header ever moved a bit, this would fail to compile
// rather than mis-map a button.
static_assert(sonyBitFor(0x4000) == DS4_BUTTON_SQUARE && sonyBitFor(0x1000) == DS4_BUTTON_CROSS &&
                  sonyBitFor(0x2000) == DS4_BUTTON_CIRCLE &&
                  sonyBitFor(0x8000) == DS4_BUTTON_TRIANGLE,
              "face buttons: XUSB X/A/B/Y are the DS4's Square/Cross/Circle/Triangle");
static_assert(sonyBitFor(0x0100) == DS4_BUTTON_SHOULDER_LEFT &&
                  sonyBitFor(0x0200) == DS4_BUTTON_SHOULDER_RIGHT &&
                  sonyBitFor(0x0020) == DS4_BUTTON_SHARE &&
                  sonyBitFor(0x0010) == DS4_BUTTON_OPTIONS &&
                  sonyBitFor(0x0040) == DS4_BUTTON_THUMB_LEFT &&
                  sonyBitFor(0x0080) == DS4_BUTTON_THUMB_RIGHT,
              "shoulders, menu pair and thumb clicks: XUSB bits are the DS4's");
static_assert(SONY_BUTTON_L2 == DS4_BUTTON_TRIGGER_LEFT &&
                  SONY_BUTTON_R2 == DS4_BUTTON_TRIGGER_RIGHT,
              "the digital trigger bits sit where ViGEm expects them");
static_assert(ds4HatFromButtons(0x0001) == DS4_BUTTON_DPAD_NORTH &&
                  ds4HatFromButtons(0x0009) == DS4_BUTTON_DPAD_NORTHEAST &&
                  ds4HatFromButtons(0x0008) == DS4_BUTTON_DPAD_EAST &&
                  ds4HatFromButtons(0x000A) == DS4_BUTTON_DPAD_SOUTHEAST &&
                  ds4HatFromButtons(0x0002) == DS4_BUTTON_DPAD_SOUTH &&
                  ds4HatFromButtons(0x0006) == DS4_BUTTON_DPAD_SOUTHWEST &&
                  ds4HatFromButtons(0x0004) == DS4_BUTTON_DPAD_WEST &&
                  ds4HatFromButtons(0x0005) == DS4_BUTTON_DPAD_NORTHWEST &&
                  ds4HatFromButtons(0x0000) == DS4_BUTTON_DPAD_NONE,
              "the hat nibble is ViGEm's DS4_DPAD_DIRECTIONS");

// Caller holds busMtx_. Folds `report` into the DS4 slot's running EX report and
// submits it so any cached gyro/accel/touch rides along.
bool ViGEmAdapter::submitDS4ReportLocked(uint32_t serial, const GamepadReport& report) {
    IoSlot& slot = io_[serial];
    if (!slot.plugged.load(std::memory_order_relaxed) || !slot.isDS4) return false;

    // The EX report's leading fields are DS4_REPORT-identical.
    auto& er = slot.ds4.report.Report;
    er.bThumbLX = ds4StickByte(report.sThumbLX);
    er.bThumbLY = ds4StickByteInverted(report.sThumbLY);
    er.bThumbRX = ds4StickByte(report.sThumbRX);
    er.bThumbRY = ds4StickByteInverted(report.sThumbRY);
    // The hat and the buttons as the one Sony layout has them. No digital L2/R2
    // here: the bus synthesizes those from the analog triggers.
    er.wButtons = sonyButtonsFromXusb(report.wButtons, 0, 0);
    // Guide -> PS (bSpecial bit 0). Bit 1 is the touchpad click, owned by the
    // touchpad path, so it is carried over rather than cleared.
    const UCHAR ps = (report.wButtons & 0x0400) ? 0x01 : 0x00;
    er.bSpecial = static_cast<UCHAR>(ps | (slot.ds4.touchpadButton ? 0x02 : 0x00));
    er.bTriggerL = report.bLeftTrigger;
    er.bTriggerR = report.bRightTrigger;
    return submitDS4Locked(serial);
}

bool ViGEmAdapter::submitMotion(uint32_t serial, const MotionReport& report) {
    if (!isValidSerial(serial)) return false;
    std::lock_guard<std::mutex> lk(busMtx_);
    if (busHandle_ == INVALID_HANDLE_VALUE) return false;

    IoSlot& slot = io_[serial];
    if (!slot.plugged.load(std::memory_order_relaxed) || !slot.isDS4) return false;

    auto& er = slot.ds4.report.Report;
    er.wGyroX = report.gyroX;
    er.wGyroY = report.gyroY;
    er.wGyroZ = report.gyroZ;
    er.wAccelX = report.accelX;
    er.wAccelY = report.accelY;
    er.wAccelZ = report.accelZ;

    submitDS4Locked(serial);
    return slot.ds4.exSupported; // IMU fields only reach the host on the EX path
}

bool ViGEmAdapter::submitBattery(uint32_t serial, const BatteryReport& report) {
    if (!isValidSerial(serial)) return false;
    std::lock_guard<std::mutex> lk(busMtx_);
    if (busHandle_ == INVALID_HANDLE_VALUE) return false;

    IoSlot& slot = io_[serial];
    if (!slot.plugged.load(std::memory_order_relaxed) || !slot.isDS4) return false;

    slot.ds4.report.Report.bBatteryLvl = ds4BatteryByte(report);
    submitDS4Locked(serial);
    return slot.ds4.exSupported;
}

bool ViGEmAdapter::submitTouchpad(uint32_t serial, const TouchpadReport& report) {
    if (!isValidSerial(serial)) return false;
    std::lock_guard<std::mutex> lk(busMtx_);
    if (busHandle_ == INVALID_HANDLE_VALUE) return false;

    IoSlot& slot = io_[serial];
    if (!slot.plugged.load(std::memory_order_relaxed) || !slot.isDS4) return false;
    DS4State& st = slot.ds4;
    auto& er = st.report.Report;

    // Bump tracking id on up->down so a consumer reads a new contact, not a
    // teleporting drag.
    if (report.finger0.active && !st.fingerDown0)
        st.trackingId0 = static_cast<uint8_t>((st.trackingId0 + 1) & 0x7F);
    if (report.finger1.active && !st.fingerDown1)
        st.trackingId1 = static_cast<uint8_t>((st.trackingId1 + 1) & 0x7F);
    st.fingerDown0 = report.finger0.active;
    st.fingerDown1 = report.finger1.active;

    DS4_TOUCH& touch = er.sCurrentTouch;
    touch.bPacketCounter = st.touchPacket++;
    const auto f0 = ds4PackTouchFinger(report.finger0, st.trackingId0);
    const auto f1 = ds4PackTouchFinger(report.finger1, st.trackingId1);
    touch.bIsUpTrackingNum1 = f0[0];
    touch.bTouchData1[0] = f0[1];
    touch.bTouchData1[1] = f0[2];
    touch.bTouchData1[2] = f0[3];
    touch.bIsUpTrackingNum2 = f1[0];
    touch.bTouchData2[0] = f1[1];
    touch.bTouchData2[1] = f1[2];
    touch.bTouchData2[2] = f1[3];
    er.bTouchPacketsN = 1;

    // Trackpad click is bSpecial bit 1 (bit 0 = PS button). Cache it so
    // submitReport can re-apply it on plain gamepad frames.
    st.touchpadButton = report.buttonPressed;
    if (report.buttonPressed)
        er.bSpecial |= 0x02;
    else
        er.bSpecial = static_cast<UCHAR>(er.bSpecial & ~0x02);

    submitDS4Locked(serial);
    return st.exSupported;
}

bool ViGEmAdapter::submitRelativeMouse(int dx, int dy, const MouseButtons& buttons, int wheelV) {
    return injectRelativeMouse(relMouseBtns_, dx, dy, buttons, wheelV);
}

bool ViGEmAdapter::supportsMotionForType(uint8_t controllerType) const {
    return supportsIdentity(controllerIdentity(controllerType)) &&
           controllerTypeHasMotion(controllerType);
}

// Motion rides only the DS4 EX report, so true requires a plugged DS4 slot whose
// EX submit was accepted. X360 has no IMU surface; unplugged/unknown reads true
// (false would surface a phantom "broken backend" badge).
bool ViGEmAdapter::motionBackendOk(uint32_t serial) const {
    if (!isValidSerial(serial)) return true;
    std::lock_guard<std::mutex> lk(busMtx_);
    const IoSlot& slot = io_[serial];
    if (!slot.plugged.load(std::memory_order_acquire)) return true;
    if (!slot.isDS4) return false;
    return slot.ds4.exSupported;
}

// Caller holds busMtx_; `serial` must be a plugged DS4 slot.
bool ViGEmAdapter::submitDS4Locked(uint32_t serial) {
    IoSlot& slot = io_[serial];
    if (!slot.plugged.load(std::memory_order_relaxed) || !slot.isDS4 || slot.event == nullptr)
        return false;
    DS4State& st = slot.ds4;

    if (st.exSupported) {
        // Advance the free-running DS4 timestamp (~5.33 us/unit, 16/3). Skipped
        // on the first submit.
        const auto now = std::chrono::steady_clock::now();
        if (st.lastSubmit.time_since_epoch().count() != 0) {
            const auto us =
                std::chrono::duration_cast<std::chrono::microseconds>(now - st.lastSubmit).count();
            st.report.Report.wTimestamp =
                static_cast<USHORT>(st.report.Report.wTimestamp + (us * 3) / 16);
        }
        st.lastSubmit = now;

        if (submitDs4ExSync(busHandle_, (unsigned long)serial, slot.ds4Ex, slot.event, st.report)) {
            return true;
        }
        // EX rejected (ViGEmBus < 1.17): latch EX off and fall through to basic
        // so buttons/sticks keep working (no IMU). The sync submit makes this
        // observable; fire-and-forget returns success on ERROR_IO_PENDING, so
        // the rejection would be missed and PlayStation input would die here.
        st.exSupported = false;
    }

    DS4_REPORT basic;
    DS4_REPORT_INIT(&basic);
    basic.bThumbLX = st.report.Report.bThumbLX;
    basic.bThumbLY = st.report.Report.bThumbLY;
    basic.bThumbRX = st.report.Report.bThumbRX;
    basic.bThumbRY = st.report.Report.bThumbRY;
    basic.wButtons = st.report.Report.wButtons;
    basic.bSpecial = st.report.Report.bSpecial;
    basic.bTriggerL = st.report.Report.bTriggerL;
    basic.bTriggerR = st.report.Report.bTriggerR;
    return submitDs4Sync(busHandle_, (unsigned long)serial, slot.ds4Basic, slot.event, basic);
}

void ViGEmAdapter::setRumbleCallback(RumbleCallback cb) {
    std::lock_guard<std::mutex> lk(busMtx_);
    rumbleCb_ = std::move(cb);
}

void ViGEmAdapter::setLightbarCallback(LightbarCallback cb) {
    std::lock_guard<std::mutex> lk(busMtx_);
    lightbarCb_ = std::move(cb);
}

// Caller holds busMtx_.
void ViGEmAdapter::startNotificationWorker(uint32_t serial, bool isDS4) {
    auto& w = notifWorkers_[serial];
    w.cancel = CreateEvent(nullptr, TRUE /* manual reset */, FALSE, nullptr);
    w.isDS4 = isDS4;
    HANDLE cancelHandle = w.cancel;
    w.th = std::thread(
        [this, serial, isDS4, cancelHandle] { notificationLoop(serial, isDS4, cancelHandle); });
}

// Caller holds busMtx_.
void ViGEmAdapter::stopNotificationWorker(uint32_t serial) {
    auto it = notifWorkers_.find(serial);
    if (it == notifWorkers_.end()) return;
    NotificationWorker w = std::move(it->second);
    notifWorkers_.erase(it);
    if (w.cancel) SetEvent(w.cancel);
    // Drop + reacquire busMtx_ around the join so the worker's own lock
    // acquisition (for the rumble callback copy) doesn't deadlock.
    busMtx_.unlock();
    if (w.th.joinable()) w.th.join();
    if (w.cancel) CloseHandle(w.cancel);
    busMtx_.lock();
}

void ViGEmAdapter::notificationLoop(uint32_t serial, bool isDS4, HANDLE cancel) {
    HANDLE bus;
    {
        std::lock_guard<std::mutex> lk(busMtx_);
        bus = busHandle_;
    }
    if (bus == INVALID_HANDLE_VALUE) return;

    while (true) {
        if (isDS4) {
            DS4_REQUEST_NOTIFICATION n{};
            if (!waitNextDS4Notification(bus, (unsigned long)serial, cancel, n)) return;
            RumbleReport rr{};
            rr.strongMagnitude = static_cast<uint16_t>(n.LargeMotor) * 257;
            rr.weakMagnitude = static_cast<uint16_t>(n.SmallMotor) * 257;
            RumbleCallback rcb;
            LightbarCallback lcb;
            {
                std::lock_guard<std::mutex> lk(busMtx_);
                rcb = rumbleCb_;
                lcb = lightbarCb_;
            }
            if (rcb) rcb(serial, rr);
            if (lcb) lcb(serial, n.LightbarColor.Red, n.LightbarColor.Green, n.LightbarColor.Blue);
        } else {
            XUSB_REQUEST_NOTIFICATION n{};
            if (!waitNextXusbNotification(bus, (unsigned long)serial, cancel, n)) return;
            RumbleReport rr{};
            rr.strongMagnitude = static_cast<uint16_t>(n.LargeMotor) * 257;
            rr.weakMagnitude = static_cast<uint16_t>(n.SmallMotor) * 257;
            RumbleCallback cb;
            {
                std::lock_guard<std::mutex> lk(busMtx_);
                cb = rumbleCb_;
            }
            if (cb) cb(serial, rr);
        }
    }
}
