// SPDX-License-Identifier: LGPL-3.0-or-later
#include "vigem_adapter.h"

#include "core/touchpad_codec.h"
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

// Caller holds busMtx_. Folds `report` into the DS4 slot's running EX report and
// submits it so any cached gyro/accel/touch rides along.
bool ViGEmAdapter::submitDS4ReportLocked(uint32_t serial, const GamepadReport& report) {
    IoSlot& slot = io_[serial];
    if (!slot.plugged.load(std::memory_order_relaxed) || !slot.isDS4) return false;

    DS4_REPORT ds4;
    DS4_REPORT_INIT(&ds4);

    // Sticks: Xbox signed int16 -> DS4 unsigned byte; Y axes inverted.
    ds4.bThumbLX = (BYTE)((((int)report.sThumbLX + 32768) * 255) / 65535);
    ds4.bThumbLY = (BYTE)(255 - (((int)report.sThumbLY + 32768) * 255) / 65535);
    ds4.bThumbRX = (BYTE)((((int)report.sThumbRX + 32768) * 255) / 65535);
    ds4.bThumbRY = (BYTE)(255 - (((int)report.sThumbRY + 32768) * 255) / 65535);

    ds4.bTriggerL = report.bLeftTrigger;
    ds4.bTriggerR = report.bRightTrigger;

    USHORT ds4btn = 0;
    if (report.wButtons & 0x1000) ds4btn |= DS4_BUTTON_CROSS;          // A -> Cross
    if (report.wButtons & 0x2000) ds4btn |= DS4_BUTTON_CIRCLE;         // B -> Circle
    if (report.wButtons & 0x4000) ds4btn |= DS4_BUTTON_SQUARE;         // X -> Square
    if (report.wButtons & 0x8000) ds4btn |= DS4_BUTTON_TRIANGLE;       // Y -> Triangle
    if (report.wButtons & 0x0100) ds4btn |= DS4_BUTTON_SHOULDER_LEFT;  // LB
    if (report.wButtons & 0x0200) ds4btn |= DS4_BUTTON_SHOULDER_RIGHT; // RB
    if (report.wButtons & 0x0020) ds4btn |= DS4_BUTTON_SHARE;          // Back -> Share
    if (report.wButtons & 0x0010) ds4btn |= DS4_BUTTON_OPTIONS;        // Start -> Options
    if (report.wButtons & 0x0040) ds4btn |= DS4_BUTTON_THUMB_LEFT;     // LS
    if (report.wButtons & 0x0080) ds4btn |= DS4_BUTTON_THUMB_RIGHT;    // RS

    // D-Pad -> DS4 hat encoding
    bool up = (report.wButtons & 0x0001) != 0;
    bool down = (report.wButtons & 0x0002) != 0;
    bool left = (report.wButtons & 0x0004) != 0;
    bool right = (report.wButtons & 0x0008) != 0;

    DS4_DPAD_DIRECTIONS dpad = DS4_BUTTON_DPAD_NONE;
    if (up && right) {
        dpad = DS4_BUTTON_DPAD_NORTHEAST;
    } else if (up && left) {
        dpad = DS4_BUTTON_DPAD_NORTHWEST;
    } else if (down && right) {
        dpad = DS4_BUTTON_DPAD_SOUTHEAST;
    } else if (down && left) {
        dpad = DS4_BUTTON_DPAD_SOUTHWEST;
    } else if (up) {
        dpad = DS4_BUTTON_DPAD_NORTH;
    } else if (down) {
        dpad = DS4_BUTTON_DPAD_SOUTH;
    } else if (left) {
        dpad = DS4_BUTTON_DPAD_WEST;
    } else if (right) {
        dpad = DS4_BUTTON_DPAD_EAST;
    }

    ds4.wButtons = ds4btn;
    DS4_SET_DPAD(&ds4, dpad);

    // Guide -> PS button (special byte bit 0)
    if (report.wButtons & 0x0400) ds4.bSpecial |= 0x01;

    // Fold into the running EX report (leading fields are DS4_REPORT-identical)
    // and submit the whole thing so any cached gyro/accel rides along.
    auto& er = slot.ds4.report.Report;
    er.bThumbLX = ds4.bThumbLX;
    er.bThumbLY = ds4.bThumbLY;
    er.bThumbRX = ds4.bThumbRX;
    er.bThumbRY = ds4.bThumbRY;
    er.wButtons = ds4.wButtons;
    // Preserve the touchpad-click bit (bSpecial bit 1, owned by the touchpad
    // path); the gamepad frame only sets the PS button (bit 0).
    er.bSpecial =
        static_cast<UCHAR>((ds4.bSpecial & ~0x02) | (slot.ds4.touchpadButton ? 0x02 : 0x00));
    er.bTriggerL = ds4.bTriggerL;
    er.bTriggerR = ds4.bTriggerR;
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

// DS4 HID battery byte: bit 4 (0x10) = cable connected, low nibble = level
// (capacity ~ nibble*10). Nibble 11 + cable bit is the "fully charged" sentinel.
static uint8_t ds4BatteryByte(const BatteryReport& report) {
    int nibble = (report.level == BATTERY_LEVEL_UNKNOWN)
                     ? 5 // mid-scale so the host still shows something
                     : static_cast<int>(report.level) / 10;
    if (nibble > 10) nibble = 10;

    switch (report.status) {
    case BATTERY_STATUS_CHARGING:
        return static_cast<uint8_t>(0x10 | nibble); // cable connected + charging
    case BATTERY_STATUS_FULL:
    case BATTERY_STATUS_WIRED:
        return static_cast<uint8_t>(0x10 | 11);
    default: // discharging/unknown: on battery, no cable bit
        return static_cast<uint8_t>(nibble);
    }
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

bool ViGEmAdapter::submitRelativeMouse(int dx, int dy, bool leftButton) {
    // Host-global desktop injection, independent of the bus, so it works with no
    // controllers plugged.
    INPUT inputs[2] = {};
    int n = 0;
    if (dx != 0 || dy != 0) {
        inputs[n].type = INPUT_MOUSE;
        inputs[n].mi.dx = dx;
        inputs[n].mi.dy = dy;
        inputs[n].mi.dwFlags = MOUSEEVENTF_MOVE;
        ++n;
    }
    const bool was = relMouseBtnDown_.exchange(leftButton);
    if (leftButton != was) {
        inputs[n].type = INPUT_MOUSE;
        inputs[n].mi.dwFlags = leftButton ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        ++n;
    }
    if (n == 0) return true; // idle frame: nothing to inject, still "handled"
    return SendInput(static_cast<UINT>(n), inputs, sizeof(INPUT)) == static_cast<UINT>(n);
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
