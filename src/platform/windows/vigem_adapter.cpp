// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * adapters/vigem_adapter.cpp — IGamepadPort implementation (Windows/ViGEm).
 */
#include "vigem_adapter.h"

#include "core/touchpad_codec.h"

// ── Raw ViGEm driver functions (defined in vigem.cpp / infra) ───────────────
extern HANDLE openVigemBus();
extern bool pluginTarget(HANDLE bus, unsigned long serial);
extern bool pluginTargetDS4(HANDLE bus, unsigned long serial);
extern bool submitReportFast(HANDLE bus, unsigned long serial, const XUSB_REPORT& rpt,
                             HANDLE event);
extern bool submitReportDS4Fast(HANDLE bus, unsigned long serial, const DS4_REPORT& rpt,
                                HANDLE event);
extern bool submitReportDS4ExFast(HANDLE bus, unsigned long serial, const DS4_REPORT_EX& rpt,
                                  HANDLE event);
extern void unplugTarget(HANDLE bus, unsigned long serial);
extern bool waitNextXusbNotification(HANDLE bus, unsigned long serial, HANDLE cancel,
                                     XUSB_REQUEST_NOTIFICATION& out);
extern bool waitNextDS4Notification(HANDLE bus, unsigned long serial, HANDLE cancel,
                                    DS4_REQUEST_NOTIFICATION& out);

ViGEmAdapter::ViGEmAdapter() = default;

ViGEmAdapter::~ViGEmAdapter() { closeBus(); }

bool ViGEmAdapter::ensureBusOpen() {
    std::lock_guard<std::mutex> lk(busMtx_);
    if (busHandle_ != INVALID_HANDLE_VALUE) return true;
    busHandle_ = openVigemBus();
    return busHandle_ != INVALID_HANDLE_VALUE;
}

void ViGEmAdapter::closeBus() {
    // Stop all notification workers first — they hold pending IOCTLs against
    // busHandle_, so we must let them unwind before closing the handle.
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
    // Clean up all submit events
    for (auto& [serial, evt] : submitEvents_) {
        if (evt) CloseHandle(evt);
    }
    submitEvents_.clear();
    ds4State_.clear();

    if (busHandle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(busHandle_);
        busHandle_ = INVALID_HANDLE_VALUE;
    }
}

bool ViGEmAdapter::isBusOpen() const {
    std::lock_guard<std::mutex> lk(busMtx_);
    return busHandle_ != INVALID_HANDLE_VALUE;
}

bool ViGEmAdapter::pluginDevice(uint32_t serial) {
    std::lock_guard<std::mutex> lk(busMtx_);
    if (busHandle_ == INVALID_HANDLE_VALUE) return false;

    if (!pluginTarget(busHandle_, (unsigned long)serial)) return false;

    // Pre-allocate overlapped event for fast report submission
    HANDLE evt = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    submitEvents_[serial] = evt;

    startNotificationWorker(serial, /*isDS4=*/false);
    return true;
}

void ViGEmAdapter::unplugDevice(uint32_t serial) {
    // Stop the notification worker first; it holds a pending IOCTL keyed on
    // serial which the driver completes-with-error on unplug, but explicitly
    // cancelling here keeps the unplug path deterministic.
    {
        std::lock_guard<std::mutex> lk(busMtx_);
        stopNotificationWorker(serial);
    }

    std::lock_guard<std::mutex> lk(busMtx_);
    if (busHandle_ == INVALID_HANDLE_VALUE) return;

    unplugTarget(busHandle_, (unsigned long)serial);

    auto it = submitEvents_.find(serial);
    if (it != submitEvents_.end()) {
        if (it->second) CloseHandle(it->second);
        submitEvents_.erase(it);
    }
    ds4State_.erase(serial);
}

bool ViGEmAdapter::submitReport(uint32_t serial, const GamepadReport& report) {
    std::lock_guard<std::mutex> lk(busMtx_);
    if (busHandle_ == INVALID_HANDLE_VALUE) return false;

    // Convert GamepadReport → XUSB_REPORT (binary compatible)
    XUSB_REPORT rpt;
    static_assert(sizeof(GamepadReport) == sizeof(XUSB_REPORT),
                  "GamepadReport and XUSB_REPORT must match");
    std::memcpy(&rpt, &report, sizeof(rpt));

    HANDLE evt = nullptr;
    auto it = submitEvents_.find(serial);
    if (it != submitEvents_.end()) evt = it->second;

    if (evt != nullptr) { return submitReportFast(busHandle_, (unsigned long)serial, rpt, evt); }
    // Fallback (should not happen)
    return false;
}

bool ViGEmAdapter::pluginDeviceDS4(uint32_t serial) {
    std::lock_guard<std::mutex> lk(busMtx_);
    if (busHandle_ == INVALID_HANDLE_VALUE) return false;

    if (!pluginTargetDS4(busHandle_, (unsigned long)serial)) return false;

    HANDLE evt = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    submitEvents_[serial] = evt;

    // Seed the extended-report cache. Centre the sticks (0x80) so the pad does
    // not read as a stuck corner before the first gamepad frame. Touch packet
    // count stays 0 = no touch activity.
    DS4State st{};
    st.report.Report.bThumbLX = 0x80;
    st.report.Report.bThumbLY = 0x80;
    st.report.Report.bThumbRX = 0x80;
    st.report.Report.bThumbRY = 0x80;
    // Seed the battery byte as charged until the first MSG_BATTERY arrives
    // (Task 1.2). The sender forwards battery on connect, so the real value
    // lands within ~1 s; seeding "charged" avoids a spurious low-battery
    // toast on the host in that window.
    st.report.Report.bBatteryLvl = 0x1B; // cable connected + fully charged
    ds4State_[serial] = st;

    startNotificationWorker(serial, /*isDS4=*/true);
    return true;
}

bool ViGEmAdapter::submitDS4Report(uint32_t serial, const GamepadReport& report) {
    std::lock_guard<std::mutex> lk(busMtx_);
    if (busHandle_ == INVALID_HANDLE_VALUE) return false;

    // Convert GamepadReport (Xbox-layout) → DS4_REPORT
    DS4_REPORT ds4;
    DS4_REPORT_INIT(&ds4);

    // Sticks: Xbox uses signed int16 (-32768..32767), DS4 uses unsigned byte (0..255)
    ds4.bThumbLX = (BYTE)((((int)report.sThumbLX + 32768) * 255) / 65535);
    ds4.bThumbLY = (BYTE)(255 - (((int)report.sThumbLY + 32768) * 255) / 65535); // Y inverted
    ds4.bThumbRX = (BYTE)((((int)report.sThumbRX + 32768) * 255) / 65535);
    ds4.bThumbRY = (BYTE)(255 - (((int)report.sThumbRY + 32768) * 255) / 65535); // Y inverted

    // Triggers: Xbox uses 0..255, DS4 uses 0..255 — direct map
    ds4.bTriggerL = report.bLeftTrigger;
    ds4.bTriggerR = report.bRightTrigger;

    // Buttons mapping (Xbox → DS4)
    USHORT ds4btn = 0;
    if (report.wButtons & 0x1000) ds4btn |= DS4_BUTTON_CROSS;          // A → Cross
    if (report.wButtons & 0x2000) ds4btn |= DS4_BUTTON_CIRCLE;         // B → Circle
    if (report.wButtons & 0x4000) ds4btn |= DS4_BUTTON_SQUARE;         // X → Square
    if (report.wButtons & 0x8000) ds4btn |= DS4_BUTTON_TRIANGLE;       // Y → Triangle
    if (report.wButtons & 0x0100) ds4btn |= DS4_BUTTON_SHOULDER_LEFT;  // LB
    if (report.wButtons & 0x0200) ds4btn |= DS4_BUTTON_SHOULDER_RIGHT; // RB
    if (report.wButtons & 0x0020) ds4btn |= DS4_BUTTON_SHARE;          // Back → Share
    if (report.wButtons & 0x0010) ds4btn |= DS4_BUTTON_OPTIONS;        // Start → Options
    if (report.wButtons & 0x0040) ds4btn |= DS4_BUTTON_THUMB_LEFT;     // LS
    if (report.wButtons & 0x0080) ds4btn |= DS4_BUTTON_THUMB_RIGHT;    // RS

    // D-Pad → DS4 hat encoding
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

    // Guide → PS button (special byte bit 0)
    if (report.wButtons & 0x0400) ds4.bSpecial |= 0x01;

    // Fold the freshly-converted gamepad frame into the controller's running
    // extended report — the leading 9 fields are layout-identical to
    // DS4_REPORT — then submit the whole DS4_REPORT_EX so any cached gyro /
    // accel sample rides along on the same frame.
    auto sit = ds4State_.find(serial);
    if (sit == ds4State_.end()) return false; // serial is not a DS4 virtual device
    auto& er = sit->second.report.Report;
    er.bThumbLX = ds4.bThumbLX;
    er.bThumbLY = ds4.bThumbLY;
    er.bThumbRX = ds4.bThumbRX;
    er.bThumbRY = ds4.bThumbRY;
    er.wButtons = ds4.wButtons;
    // Preserve the touchpad clicky-button bit (bSpecial bit 1), which the
    // touchpad path owns — the gamepad frame only contributes the PS button
    // (bit 0), so a plain assignment would wipe a held trackpad click between
    // touch samples.
    er.bSpecial =
        static_cast<UCHAR>((ds4.bSpecial & ~0x02) | (sit->second.touchpadButton ? 0x02 : 0x00));
    er.bTriggerL = ds4.bTriggerL;
    er.bTriggerR = ds4.bTriggerR;
    return submitDS4Locked(serial);
}

bool ViGEmAdapter::submitMotion(uint32_t serial, const MotionReport& report) {
    std::lock_guard<std::mutex> lk(busMtx_);
    if (busHandle_ == INVALID_HANDLE_VALUE) return false;

    // Only DualShock 4 virtual devices have an IMU surface. An Xbox 360 pad
    // (no ds4State_ entry) silently drops motion — the SessionService still
    // caches it for the web UI / DSU server.
    auto sit = ds4State_.find(serial);
    if (sit == ds4State_.end()) return false;

    // The wire MotionReport is signed-int16 fixed point (±32767 ≈ ±2000 deg/s
    // for gyro, ±4 g for accel). A real DS4's gyro/accel are also int16 with a
    // near-identical full scale, so the values pass straight into the
    // DS4_REPORT_EX IMU fields — proportional and close to DS4-native. A
    // consumer applying exact DS4 calibration sees a small scale offset; the
    // DSU server path remains the precisely-calibrated motion route.
    auto& er = sit->second.report.Report;
    er.wGyroX = report.gyroX;
    er.wGyroY = report.gyroY;
    er.wGyroZ = report.gyroZ;
    er.wAccelX = report.accelX;
    er.wAccelY = report.accelY;
    er.wAccelZ = report.accelZ;

    submitDS4Locked(serial);
    // The IMU fields only actually reach the host on the extended-report path.
    return sit->second.exSupported;
}

// Map a wire BatteryReport (level 0..100 or 0xFF-unknown, status enum) onto the
// DS4 HID battery byte: bit 4 (0x10) = cable connected, low nibble = level. The
// host derives capacity ≈ nibble × 10 (clamped to 100%); nibble 11 with the
// cable bit set is the DualShock 4's "fully charged" sentinel.
static uint8_t ds4BatteryByte(const BatteryReport& report) {
    int nibble = (report.level == BATTERY_LEVEL_UNKNOWN)
                     ? 5 // unknown → mid-scale, so the host still shows something
                     : static_cast<int>(report.level) / 10;
    if (nibble > 10) nibble = 10;

    switch (report.status) {
    case BATTERY_STATUS_CHARGING:
        return static_cast<uint8_t>(0x10 | nibble); // cable connected + charging
    case BATTERY_STATUS_FULL:
    case BATTERY_STATUS_WIRED:
        // FULL = sitting on the charger at 100%. WIRED = the controller is
        // cabled and the sender fell back to host (desktop / AC) power — both
        // present to the host as a fully-charged pad.
        return static_cast<uint8_t>(0x10 | 11);
    default: // discharging / unknown — running on battery, no cable bit
        return static_cast<uint8_t>(nibble);
    }
}

bool ViGEmAdapter::submitBattery(uint32_t serial, const BatteryReport& report) {
    std::lock_guard<std::mutex> lk(busMtx_);
    if (busHandle_ == INVALID_HANDLE_VALUE) return false;

    // Only DualShock 4 virtual devices expose a battery byte. An Xbox 360 pad
    // (no ds4State_ entry) silently drops it — the SessionService still caches
    // the value for the web UI.
    auto sit = ds4State_.find(serial);
    if (sit == ds4State_.end()) return false;

    sit->second.report.Report.bBatteryLvl = ds4BatteryByte(report);

    submitDS4Locked(serial);
    // The battery byte only actually reaches the host on the extended-report
    // path — the basic DS4_REPORT has no battery field.
    return sit->second.exSupported;
}

bool ViGEmAdapter::submitTouchpad(uint32_t serial, const TouchpadReport& report) {
    std::lock_guard<std::mutex> lk(busMtx_);
    if (busHandle_ == INVALID_HANDLE_VALUE) return false;

    // Only DualShock 4 virtual devices have a touchpad surface. An Xbox 360
    // pad (no ds4State_ entry) silently drops it — the SessionService still
    // caches the sample for the web UI.
    auto sit = ds4State_.find(serial);
    if (sit == ds4State_.end()) return false;
    DS4State& st = sit->second;
    auto& er = st.report.Report;

    // Bump a finger's tracking id whenever it transitions up→down so a
    // consumer reads a brand-new contact rather than a teleporting drag.
    if (report.finger0.active && !st.fingerDown0)
        st.trackingId0 = static_cast<uint8_t>((st.trackingId0 + 1) & 0x7F);
    if (report.finger1.active && !st.fingerDown1)
        st.trackingId1 = static_cast<uint8_t>((st.trackingId1 + 1) & 0x7F);
    st.fingerDown0 = report.finger0.active;
    st.fingerDown1 = report.finger1.active;

    // One current touch frame rides along on this report. ds4PackTouchFinger
    // (core/touchpad_codec.h) owns the wire→DS4 12-bit coordinate packing.
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

    // The clicky-trackpad button is bSpecial bit 1 (bit 0 = PS button, owned
    // by the gamepad-report path). Cache it so submitDS4Report can re-apply it
    // on plain gamepad frames without clobbering it.
    st.touchpadButton = report.buttonPressed;
    if (report.buttonPressed)
        er.bSpecial |= 0x02;
    else
        er.bSpecial = static_cast<UCHAR>(er.bSpecial & ~0x02);

    submitDS4Locked(serial);
    // Touchpad fields only actually reach the host on the extended-report path.
    return st.exSupported;
}

bool ViGEmAdapter::submitRelativeMouse(int dx, int dy, bool leftButton) {
    // Host-global desktop injection — independent of the ViGEm bus, so this
    // works even with no virtual controllers plugged in.
    INPUT inputs[2] = {};
    int n = 0;
    if (dx != 0 || dy != 0) {
        inputs[n].type = INPUT_MOUSE;
        inputs[n].mi.dx = dx;
        inputs[n].mi.dy = dy;
        inputs[n].mi.dwFlags = MOUSEEVENTF_MOVE;
        ++n;
    }
    // Emit a button event only on a level change — a held click must not
    // re-press every frame.
    const bool was = relMouseBtnDown_.exchange(leftButton);
    if (leftButton != was) {
        inputs[n].type = INPUT_MOUSE;
        inputs[n].mi.dwFlags = leftButton ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        ++n;
    }
    if (n == 0) return true; // idle frame — nothing to inject, still "handled"
    return SendInput(static_cast<UINT>(n), inputs, sizeof(INPUT)) == static_cast<UINT>(n);
}

// Caller holds busMtx_; `serial` must exist in ds4State_.
bool ViGEmAdapter::submitDS4Locked(uint32_t serial) {
    auto sit = ds4State_.find(serial);
    if (sit == ds4State_.end()) return false;
    DS4State& st = sit->second;

    HANDLE evt = nullptr;
    if (auto eit = submitEvents_.find(serial); eit != submitEvents_.end()) evt = eit->second;
    if (evt == nullptr) return false;

    if (st.exSupported) {
        // Advance the free-running DS4 report timestamp (~5.33 µs per unit;
        // 16/3 ≈ 5.33). Skipped on the very first submit (no prior `lastSubmit`).
        const auto now = std::chrono::steady_clock::now();
        if (st.lastSubmit.time_since_epoch().count() != 0) {
            const auto us =
                std::chrono::duration_cast<std::chrono::microseconds>(now - st.lastSubmit).count();
            st.report.Report.wTimestamp =
                static_cast<USHORT>(st.report.Report.wTimestamp + (us * 3) / 16);
        }
        st.lastSubmit = now;

        if (submitReportDS4ExFast(busHandle_, (unsigned long)serial, st.report, evt)) {
            return true;
        }
        // The driver rejected IOCTL_DS4_SUBMIT_REPORT_EX — almost certainly a
        // ViGEmBus older than 1.17. Latch the EX path off for this serial and
        // fall through to the basic report so buttons / sticks keep working
        // (the basic report has no IMU fields, so motion is dropped).
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
    return submitReportDS4Fast(busHandle_, (unsigned long)serial, basic, evt);
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
    // Capture by value — the worker only ever reads serial/isDS4/cancelHandle.
    w.th = std::thread(
        [this, serial, isDS4, cancelHandle] { notificationLoop(serial, isDS4, cancelHandle); });
}

// Caller holds busMtx_. Extracts the worker out of the map under lock, then
// the caller is expected to join + close-handle outside the lock (joining
// inside would deadlock against the worker's own lock acquisition for the
// rumble callback copy).
void ViGEmAdapter::stopNotificationWorker(uint32_t serial) {
    auto it = notifWorkers_.find(serial);
    if (it == notifWorkers_.end()) return;
    NotificationWorker w = std::move(it->second);
    notifWorkers_.erase(it);
    if (w.cancel) SetEvent(w.cancel);
    // We need to release busMtx_ for the join; the caller's lock_guard owns
    // the lock, so drop and reacquire at the underlying-mutex level. This is
    // safe because the caller's lock_guard will re-unlock the (now re-locked)
    // mutex on scope exit.
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
        // Block until the driver has data (game called XInputSetState etc.)
        // or until cancel is signalled by stopNotificationWorker.
        if (isDS4) {
            DS4_REQUEST_NOTIFICATION n{};
            if (!waitNextDS4Notification(bus, (unsigned long)serial, cancel, n)) return;
            RumbleReport rr{};
            // Scale the DS4 motor bytes (0..255) up into the XInput-style
            // 16-bit space the wire format uses, so the dish can handle one
            // unified scale regardless of source.
            rr.strongMagnitude = static_cast<uint16_t>(n.LargeMotor) * 257;
            rr.weakMagnitude = static_cast<uint16_t>(n.SmallMotor) * 257;
            // The DS4 driver delivers rumble and lightbar in the *same*
            // notification. Fan it out to the two independent return paths:
            // motor vibration via the rumble callback, LED colour via the
            // lightbar callback. Identical-colour coalescing for the latter is
            // handled by SessionService::handleLightbarFromBackend.
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
