// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * gamepad_adapter.cpp — IGamepadPort implementation (Linux / uinput).
 */
#include "gamepad_adapter.h"

#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstring>

namespace {

constexpr uint16_t XBOX_VID = 0x045e;
constexpr uint16_t XBOX_PID = 0x028e;
constexpr uint16_t DS4_VID = 0x054c;
constexpr uint16_t DS4_PID = 0x05c4;

// Buttons exposed on both profiles. Code values follow evdev conventions.
constexpr int BUTTONS[] = {BTN_A,      BTN_B,     BTN_X,    BTN_Y,      BTN_TL,    BTN_TR,
                           BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR};

// Emit a single input_event to the uinput fd.
bool emit(int fd, uint16_t type, uint16_t code, int32_t value) {
    struct input_event ev{};
    ev.type = type;
    ev.code = code;
    ev.value = value;
    return ::write(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev);
}

// Configure one absolute axis via UI_ABS_SETUP (kernel ≥ 4.5).
bool setupAbs(int fd, uint16_t code, int32_t min, int32_t max, int32_t flat, int32_t fuzz) {
    struct uinput_abs_setup abs{};
    abs.code = code;
    abs.absinfo.minimum = min;
    abs.absinfo.maximum = max;
    abs.absinfo.flat = flat;
    abs.absinfo.fuzz = fuzz;
    return ::ioctl(fd, UI_ABS_SETUP, &abs) == 0;
}

} // namespace

GamepadAdapter::~GamepadAdapter() { closeBus(); }

bool GamepadAdapter::ensureBusOpen() {
    std::lock_guard<std::mutex> lk(mtx_);
    // "Bus" on Linux is just the uinput module + device node; verify we can
    // write to it. Actual device fds are opened per plugin.
    if (::access("/dev/uinput", W_OK) != 0) return false;
    busOpen_ = true;
    return true;
}

void GamepadAdapter::closeBus() {
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& [serial, fd] : fds_) {
        if (fd >= 0) {
            (void)::ioctl(fd, UI_DEV_DESTROY);
            ::close(fd);
        }
    }
    fds_.clear();
    busOpen_ = false;
}

bool GamepadAdapter::isBusOpen() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return busOpen_;
}

int GamepadAdapter::openUinputDevice(uint32_t serial, bool ds4) {
    int fd = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) return -1;

    // Enable event classes.
    if (::ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0) goto fail;
    if (::ioctl(fd, UI_SET_EVBIT, EV_ABS) < 0) goto fail;
    if (::ioctl(fd, UI_SET_EVBIT, EV_SYN) < 0) goto fail;

    for (int btn : BUTTONS) {
        if (::ioctl(fd, UI_SET_KEYBIT, btn) < 0) goto fail;
    }

    // Absolute axes used by both Xbox and DS4 profiles.
    for (int ax : {ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_Z, ABS_RZ, ABS_HAT0X, ABS_HAT0Y}) {
        if (::ioctl(fd, UI_SET_ABSBIT, ax) < 0) goto fail;
    }

    // Per-axis absinfo (range/flat/fuzz) — matches xpad driver defaults.
    if (!setupAbs(fd, ABS_X, -32768, 32767, 128, 16)) goto fail;
    if (!setupAbs(fd, ABS_Y, -32768, 32767, 128, 16)) goto fail;
    if (!setupAbs(fd, ABS_RX, -32768, 32767, 128, 16)) goto fail;
    if (!setupAbs(fd, ABS_RY, -32768, 32767, 128, 16)) goto fail;
    if (!setupAbs(fd, ABS_Z, 0, 255, 0, 0)) goto fail;
    if (!setupAbs(fd, ABS_RZ, 0, 255, 0, 0)) goto fail;
    if (!setupAbs(fd, ABS_HAT0X, -1, 1, 0, 0)) goto fail;
    if (!setupAbs(fd, ABS_HAT0Y, -1, 1, 0, 0)) goto fail;

    {
        struct uinput_setup usetup{};
        usetup.id.bustype = BUS_USB;
        usetup.id.vendor = ds4 ? DS4_VID : XBOX_VID;
        usetup.id.product = ds4 ? DS4_PID : XBOX_PID;
        usetup.id.version = ds4 ? 0x0100 : 0x0110;
        const char* name = ds4 ? "Satellite Virtual DualShock 4" : "Satellite Virtual Xbox 360 Pad";
        std::snprintf(usetup.name, sizeof(usetup.name), "%s #%u", name, serial);
        if (::ioctl(fd, UI_DEV_SETUP, &usetup) < 0) goto fail;
    }

    if (::ioctl(fd, UI_DEV_CREATE) < 0) goto fail;
    return fd;

fail:
    ::close(fd);
    return -1;
}

bool GamepadAdapter::pluginDevice(uint32_t serial) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!busOpen_) return false;
    if (fds_.count(serial)) return false;
    int fd = openUinputDevice(serial, /*ds4=*/false);
    if (fd < 0) return false;
    fds_[serial] = fd;
    return true;
}

bool GamepadAdapter::pluginDeviceDS4(uint32_t serial) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!busOpen_) return false;
    if (fds_.count(serial)) return false;
    int fd = openUinputDevice(serial, /*ds4=*/true);
    if (fd < 0) return false;
    fds_[serial] = fd;
    return true;
}

void GamepadAdapter::unplugDevice(uint32_t serial) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = fds_.find(serial);
    if (it == fds_.end()) return;
    int fd = it->second;
    if (fd >= 0) {
        (void)::ioctl(fd, UI_DEV_DESTROY);
        ::close(fd);
    }
    fds_.erase(it);
}

// ── Report submission ───────────────────────────────────────────────────────
// XUSB wButtons bit layout (matches core/types.h convention).
namespace {
constexpr uint16_t XUSB_DPAD_UP = 0x0001;
constexpr uint16_t XUSB_DPAD_DOWN = 0x0002;
constexpr uint16_t XUSB_DPAD_LEFT = 0x0004;
constexpr uint16_t XUSB_DPAD_RIGHT = 0x0008;
constexpr uint16_t XUSB_START = 0x0010;
constexpr uint16_t XUSB_BACK = 0x0020;
constexpr uint16_t XUSB_LS = 0x0040;
constexpr uint16_t XUSB_RS = 0x0080;
constexpr uint16_t XUSB_LB = 0x0100;
constexpr uint16_t XUSB_RB = 0x0200;
constexpr uint16_t XUSB_GUIDE = 0x0400;
constexpr uint16_t XUSB_A = 0x1000;
constexpr uint16_t XUSB_B = 0x2000;
constexpr uint16_t XUSB_X = 0x4000;
constexpr uint16_t XUSB_Y = 0x8000;

// Clamp a signed 16-bit sample so negating -32768 does not overflow when we
// flip the Y axis to match evdev's "down is positive" convention.
int32_t clampS16(int32_t v) {
    if (v < -32767) return -32767;
    if (v > 32767) return 32767;
    return v;
}
} // namespace

bool GamepadAdapter::submitReport(uint32_t serial, const GamepadReport& report) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = fds_.find(serial);
    if (it == fds_.end() || it->second < 0) return false;
    int fd = it->second;

    bool ok = true;
    // Sticks. XUSB Y is positive-up; evdev Y is positive-down — invert.
    ok &= emit(fd, EV_ABS, ABS_X, report.sThumbLX);
    ok &= emit(fd, EV_ABS, ABS_Y, -clampS16(report.sThumbLY));
    ok &= emit(fd, EV_ABS, ABS_RX, report.sThumbRX);
    ok &= emit(fd, EV_ABS, ABS_RY, -clampS16(report.sThumbRY));

    // Triggers (0..255).
    ok &= emit(fd, EV_ABS, ABS_Z, report.bLeftTrigger);
    ok &= emit(fd, EV_ABS, ABS_RZ, report.bRightTrigger);

    // D-pad via ABS_HAT0X/Y.
    int32_t hatX = 0, hatY = 0;
    if (report.wButtons & XUSB_DPAD_LEFT)
        hatX = -1;
    else if (report.wButtons & XUSB_DPAD_RIGHT)
        hatX = 1;
    if (report.wButtons & XUSB_DPAD_UP)
        hatY = -1;
    else if (report.wButtons & XUSB_DPAD_DOWN)
        hatY = 1;
    ok &= emit(fd, EV_ABS, ABS_HAT0X, hatX);
    ok &= emit(fd, EV_ABS, ABS_HAT0Y, hatY);

    // Buttons.
    ok &= emit(fd, EV_KEY, BTN_A, (report.wButtons & XUSB_A) ? 1 : 0);
    ok &= emit(fd, EV_KEY, BTN_B, (report.wButtons & XUSB_B) ? 1 : 0);
    ok &= emit(fd, EV_KEY, BTN_X, (report.wButtons & XUSB_X) ? 1 : 0);
    ok &= emit(fd, EV_KEY, BTN_Y, (report.wButtons & XUSB_Y) ? 1 : 0);
    ok &= emit(fd, EV_KEY, BTN_TL, (report.wButtons & XUSB_LB) ? 1 : 0);
    ok &= emit(fd, EV_KEY, BTN_TR, (report.wButtons & XUSB_RB) ? 1 : 0);
    ok &= emit(fd, EV_KEY, BTN_SELECT, (report.wButtons & XUSB_BACK) ? 1 : 0);
    ok &= emit(fd, EV_KEY, BTN_START, (report.wButtons & XUSB_START) ? 1 : 0);
    ok &= emit(fd, EV_KEY, BTN_MODE, (report.wButtons & XUSB_GUIDE) ? 1 : 0);
    ok &= emit(fd, EV_KEY, BTN_THUMBL, (report.wButtons & XUSB_LS) ? 1 : 0);
    ok &= emit(fd, EV_KEY, BTN_THUMBR, (report.wButtons & XUSB_RS) ? 1 : 0);

    // Flush the frame.
    ok &= emit(fd, EV_SYN, SYN_REPORT, 0);
    return ok;
}

bool GamepadAdapter::submitDS4Report(uint32_t serial, const GamepadReport& report) {
    // The uinput device is configured with DS4 VID/PID but exposes the same
    // evdev button/axis codes — SDL2 / Steam Input identify it as a DualShock 4
    // via the USB IDs and remap face buttons accordingly.
    return submitReport(serial, report);
}
