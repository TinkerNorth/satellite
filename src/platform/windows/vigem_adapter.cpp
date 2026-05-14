// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * adapters/vigem_adapter.cpp — IGamepadPort implementation (Windows/ViGEm).
 */
#include "vigem_adapter.h"

// ── Raw ViGEm driver functions (defined in vigem.cpp / infra) ───────────────
extern HANDLE openVigemBus();
extern bool pluginTarget(HANDLE bus, unsigned long serial);
extern bool pluginTargetDS4(HANDLE bus, unsigned long serial);
extern bool submitReportFast(HANDLE bus, unsigned long serial, const XUSB_REPORT& rpt,
                             HANDLE event);
extern bool submitReportDS4Fast(HANDLE bus, unsigned long serial, const DS4_REPORT& rpt,
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

    HANDLE evt = nullptr;
    auto it = submitEvents_.find(serial);
    if (it != submitEvents_.end()) evt = it->second;

    if (evt != nullptr) { return submitReportDS4Fast(busHandle_, (unsigned long)serial, ds4, evt); }
    return false;
}

// ── Rumble callback registration ────────────────────────────────────────────

void ViGEmAdapter::setRumbleCallback(RumbleCallback cb) {
    std::lock_guard<std::mutex> lk(busMtx_);
    rumbleCb_ = std::move(cb);
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
            rr.lightbarR = n.LightbarColor.Red;
            rr.lightbarG = n.LightbarColor.Green;
            rr.lightbarB = n.LightbarColor.Blue;
            rr.hasLightbar = true;
            RumbleCallback cb;
            {
                std::lock_guard<std::mutex> lk(busMtx_);
                cb = rumbleCb_;
            }
            if (cb) cb(serial, rr);
        } else {
            XUSB_REQUEST_NOTIFICATION n{};
            if (!waitNextXusbNotification(bus, (unsigned long)serial, cancel, n)) return;
            RumbleReport rr{};
            rr.strongMagnitude = static_cast<uint16_t>(n.LargeMotor) * 257;
            rr.weakMagnitude = static_cast<uint16_t>(n.SmallMotor) * 257;
            // Xbox 360 has no lightbar — leave hasLightbar=false.
            RumbleCallback cb;
            {
                std::lock_guard<std::mutex> lk(busMtx_);
                cb = rumbleCb_;
            }
            if (cb) cb(serial, rr);
        }
    }
}
