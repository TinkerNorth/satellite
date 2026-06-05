// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * adapters/vigem_adapter.cpp -- IGamepadPort implementation (Windows/ViGEm).
 *
 * Hot-path changes vs the pre-PR revision (see header for the full
 * rationale):
 *   * Per-serial state lives in a flat `io_[17]` array, no hash lookups.
 *   * `XUSB_REPORT` is filled by a single 12-byte memcpy from the wire
 *     bytes straight into the slot-persistent submit buffer -- the
 *     intermediate stack-local `XUSB_REPORT rpt` is gone.
 *   * `ds4State_` (formerly a map) moved into the IoSlot, so DS4
 *     submission paths also avoid the hash lookup.
 *   * `submitReport` validates + grabs the bus handle under busMtx_,
 *     then drops the lock for the actual DeviceIoControl, so the
 *     notification thread is no longer blocked on the IOCTL syscall.
 *
 * IO sequencing is SYNCHRONOUS (GetOverlappedResult bWait=TRUE) -- a
 * previous revision tried fire-and-forget here and the dish reported
 * "no input reaching the game" with no driver-side error. The sync
 * path is the documented-safe behaviour for ViGEmBus IOCTLs; see
 * vigem.h's helper comments for the full reasoning. The
 * slot-persistent submit buffer + dropped-busMtx_ wins are retained.
 */
#include "vigem_adapter.h"

#include "core/touchpad_codec.h"
#include "vigem.h"

// ── Raw ViGEm driver functions (defined in vigem.cpp / infra) ───────────────
extern HANDLE openVigemBus();
extern bool pluginTarget(HANDLE bus, unsigned long serial);
extern bool pluginTargetDS4(HANDLE bus, unsigned long serial);
extern void unplugTarget(HANDLE bus, unsigned long serial);
extern bool waitNextXusbNotification(HANDLE bus, unsigned long serial, HANDLE cancel,
                                     XUSB_REQUEST_NOTIFICATION& out);
extern bool waitNextDS4Notification(HANDLE bus, unsigned long serial, HANDLE cancel,
                                    DS4_REQUEST_NOTIFICATION& out);

namespace {

// True if `serial` is in the valid 1..MAX_BACKEND_CONTROLLERS range.
// Centralised so every public entry point can early-out the same way.
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
    // Stop all notification workers first -- they hold pending IOCTLs on
    // busHandle_, so they must unwind before we close the handle.
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

    // Submit IO is synchronous, so no submit IOCTL is ever in flight here:
    // every submitReport / submitDS4Locked has already waited on its own
    // completion before returning. Just free each slot's persistent event.
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

// Create the per-serial auto-reset event used as the OVERLAPPED hEvent for
// the slot's synchronous submits. The synchronous submit path resets it at
// IOCTL-start and the kernel signals it on completion, so the initial state
// is immaterial; created signalled only so no teardown path can ever block
// on it. Helper used by both Xbox and DS4 plug paths.
static HANDLE makeSlotEvent() {
    return CreateEventW(nullptr, /*manualReset=*/FALSE, /*initialState=*/TRUE, nullptr);
}

bool ViGEmAdapter::pluginDevice(uint32_t serial) {
    if (!isValidSerial(serial)) return false;
    std::lock_guard<std::mutex> lk(busMtx_);
    if (busHandle_ == INVALID_HANDLE_VALUE) return false;
    if (!pluginTarget(busHandle_, static_cast<unsigned long>(serial))) return false;

    IoSlot& slot = io_[serial];
    if (slot.event == nullptr) slot.event = makeSlotEvent();
    slot.isDS4 = false;
    slot.ds4 = {};
    slot.plugged.store(true, std::memory_order_release);

    startNotificationWorker(serial, /*isDS4=*/false);
    return true;
}

void ViGEmAdapter::unplugDevice(uint32_t serial) {
    if (!isValidSerial(serial)) return;

    // Stop the notification worker first; it holds a pending IOCTL keyed
    // on serial which the driver completes-with-error on unplug, but
    // explicitly cancelling here keeps the unplug path deterministic.
    {
        std::lock_guard<std::mutex> lk(busMtx_);
        stopNotificationWorker(serial);
    }

    std::lock_guard<std::mutex> lk(busMtx_);
    if (busHandle_ == INVALID_HANDLE_VALUE) return;

    // Stop accepting new submissions immediately so a concurrent receiver
    // thread can't race the unplug.
    IoSlot& slot = io_[serial];
    slot.plugged.store(false, std::memory_order_release);

    // Submit IO is synchronous, so there is never an in-flight submit
    // IOCTL holding slot.xsr / slot.event at this point -- no drain wait
    // needed. (busMtx_ is held, and the plugged=false store above blocks
    // any new submitReport / submitDS4Report from starting.)
    unplugTarget(busHandle_, static_cast<unsigned long>(serial));

    // We deliberately do NOT CloseHandle(slot.event) here -- it gets
    // reused if the serial is replugged. Final cleanup is in closeBus.
    slot.isDS4 = false;
    slot.ds4 = {};
}

bool ViGEmAdapter::submitReport(uint32_t serial, const GamepadReport& report) {
    if (!isValidSerial(serial)) return false;

    // Hot path: take the lock long enough to copy the handle + slot
    // pointer, then drop it. The slot's submit buffer + event are
    // persistent (closed only at closeBus), so we can keep using them
    // after the lock release. Sync submit means the IOCTL has returned
    // by the time closeBus could observe us -- no in-flight kernel
    // reference to the slot is possible at teardown, so the previously
    // needed drain wait is gone.
    HANDLE bus;
    IoSlot* slot;
    {
        std::lock_guard<std::mutex> lk(busMtx_);
        if (busHandle_ == INVALID_HANDLE_VALUE) return false;
        slot = &io_[serial];
        if (!slot->plugged.load(std::memory_order_acquire) || slot->isDS4 || slot->event == nullptr)
            return false;
        bus = busHandle_;
    }

    // Single memcpy from wire-layout GamepadReport into the kernel-bound
    // XUSB_REPORT (binary-compatible). No intermediate stack-local copy.
    static_assert(sizeof(GamepadReport) == sizeof(XUSB_REPORT),
                  "GamepadReport and XUSB_REPORT must match");
    // Synchronous-wait submit (GetOverlappedResult bWait=TRUE inside the
    // helper). The driver has finished consuming slot->xsr by the time
    // this returns, so the slot's buffer + event are immediately reusable
    // and no in-flight kernel reference to the slot survives the call --
    // this is the documented-safe path (see vigem.h). Fire-and-forget was
    // tried here and reverted: the dish saw "no input reaching the game".
    return submitXusbSync(bus, static_cast<unsigned long>(serial), slot->xsr, slot->event, &report);
}

bool ViGEmAdapter::pluginDeviceDS4(uint32_t serial) {
    if (!isValidSerial(serial)) return false;
    std::lock_guard<std::mutex> lk(busMtx_);
    if (busHandle_ == INVALID_HANDLE_VALUE) return false;
    if (!pluginTargetDS4(busHandle_, static_cast<unsigned long>(serial))) return false;

    IoSlot& slot = io_[serial];
    if (slot.event == nullptr) slot.event = makeSlotEvent();
    slot.isDS4 = true;
    slot.ds4 = {};

    // Seed the extended-report cache. Centre the sticks (0x80) so the pad
    // doesn't read as a stuck corner before the first gamepad frame.
    slot.ds4.report.Report.bThumbLX = 0x80;
    slot.ds4.report.Report.bThumbLY = 0x80;
    slot.ds4.report.Report.bThumbRX = 0x80;
    slot.ds4.report.Report.bThumbRY = 0x80;
    // Seed the battery byte as charged until the first MSG_BATTERY arrives.
    slot.ds4.report.Report.bBatteryLvl = 0x1B; // cable connected + fully charged
    slot.plugged.store(true, std::memory_order_release);

    startNotificationWorker(serial, /*isDS4=*/true);
    return true;
}

bool ViGEmAdapter::submitDS4Report(uint32_t serial, const GamepadReport& report) {
    if (!isValidSerial(serial)) return false;
    std::lock_guard<std::mutex> lk(busMtx_);
    if (busHandle_ == INVALID_HANDLE_VALUE) return false;
    IoSlot& slot = io_[serial];
    if (!slot.plugged.load(std::memory_order_relaxed) || !slot.isDS4) return false;

    // Convert GamepadReport (Xbox-layout) -> DS4_REPORT
    DS4_REPORT ds4;
    DS4_REPORT_INIT(&ds4);

    // Sticks: Xbox uses signed int16 (-32768..32767), DS4 uses unsigned byte (0..255)
    ds4.bThumbLX = (BYTE)((((int)report.sThumbLX + 32768) * 255) / 65535);
    ds4.bThumbLY = (BYTE)(255 - (((int)report.sThumbLY + 32768) * 255) / 65535); // Y inverted
    ds4.bThumbRX = (BYTE)((((int)report.sThumbRX + 32768) * 255) / 65535);
    ds4.bThumbRY = (BYTE)(255 - (((int)report.sThumbRY + 32768) * 255) / 65535); // Y inverted

    // Triggers: Xbox uses 0..255, DS4 uses 0..255 -- direct map
    ds4.bTriggerL = report.bLeftTrigger;
    ds4.bTriggerR = report.bRightTrigger;

    // Buttons mapping (Xbox -> DS4)
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

    // Fold the freshly-converted gamepad frame into the controller's running
    // extended report -- the leading fields are layout-identical to
    // DS4_REPORT -- then submit the whole DS4_REPORT_EX so any cached
    // gyro/accel sample rides along on the same frame.
    auto& er = slot.ds4.report.Report;
    er.bThumbLX = ds4.bThumbLX;
    er.bThumbLY = ds4.bThumbLY;
    er.bThumbRX = ds4.bThumbRX;
    er.bThumbRY = ds4.bThumbRY;
    er.wButtons = ds4.wButtons;
    // Preserve the touchpad clicky-button bit (bSpecial bit 1), owned by
    // the touchpad path -- the gamepad frame only contributes the PS
    // button (bit 0), so a plain assignment would wipe a held trackpad
    // click between touch samples.
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
    // IMU fields only actually reach the host on the extended-report path.
    return slot.ds4.exSupported;
}

// Map a wire BatteryReport (level 0..100 or 0xFF-unknown, status enum) onto
// the DS4 HID battery byte: bit 4 (0x10) = cable connected, low nibble =
// level. The host derives capacity ~ nibble * 10 (clamped to 100%); nibble
// 11 with the cable bit set is the DualShock 4's "fully charged" sentinel.
static uint8_t ds4BatteryByte(const BatteryReport& report) {
    int nibble = (report.level == BATTERY_LEVEL_UNKNOWN)
                     ? 5 // unknown -> mid-scale, so the host still shows something
                     : static_cast<int>(report.level) / 10;
    if (nibble > 10) nibble = 10;

    switch (report.status) {
    case BATTERY_STATUS_CHARGING:
        return static_cast<uint8_t>(0x10 | nibble); // cable connected + charging
    case BATTERY_STATUS_FULL:
    case BATTERY_STATUS_WIRED:
        return static_cast<uint8_t>(0x10 | 11);
    default: // discharging / unknown -- running on battery, no cable bit
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

    // Bump a finger's tracking id whenever it transitions up->down so a
    // consumer reads a brand-new contact rather than a teleporting drag.
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

    // The clicky-trackpad button is bSpecial bit 1 (bit 0 = PS button,
    // owned by the gamepad-report path). Cache it so submitDS4Report can
    // re-apply it on plain gamepad frames without clobbering it.
    st.touchpadButton = report.buttonPressed;
    if (report.buttonPressed)
        er.bSpecial |= 0x02;
    else
        er.bSpecial = static_cast<UCHAR>(er.bSpecial & ~0x02);

    submitDS4Locked(serial);
    return st.exSupported;
}

bool ViGEmAdapter::submitRelativeMouse(int dx, int dy, bool leftButton) {
    // Host-global desktop injection -- independent of the ViGEm bus, so
    // this works even with no virtual controllers plugged in.
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
    if (n == 0) return true; // idle frame -- nothing to inject, still "handled"
    return SendInput(static_cast<UINT>(n), inputs, sizeof(INPUT)) == static_cast<UINT>(n);
}

bool ViGEmAdapter::supportsMotionForType(uint8_t controllerType) const {
    return controllerTypeUsesDS4(controllerType);
}

// Caller holds busMtx_; `serial` must be a plugged DS4 slot.
bool ViGEmAdapter::submitDS4Locked(uint32_t serial) {
    IoSlot& slot = io_[serial];
    if (!slot.plugged.load(std::memory_order_relaxed) || !slot.isDS4 || slot.event == nullptr)
        return false;
    DS4State& st = slot.ds4;

    if (st.exSupported) {
        // Advance the free-running DS4 report timestamp (~5.33 us per
        // unit; 16/3 ~= 5.33). Skipped on the very first submit.
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
        // The driver rejected IOCTL_DS4_SUBMIT_REPORT_EX -- almost
        // certainly a ViGEmBus older than 1.17. Latch the EX path off
        // for this serial and fall through to the basic report so
        // buttons / sticks keep working (no IMU fields). The synchronous
        // submit above is what makes this rejection observable: a
        // fire-and-forget submit returns success the instant the IOCTL is
        // queued (ERROR_IO_PENDING), so the EX rejection would never be
        // seen and PlayStation input would silently die here.
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

// ── Rumble callback registration ────────────────────────────────────────────

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
