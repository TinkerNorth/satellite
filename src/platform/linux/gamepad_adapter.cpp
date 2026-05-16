// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * gamepad_adapter.cpp — IGamepadPort implementation (Linux / uinput).
 */
#include "gamepad_adapter.h"

#include "core/touchpad_codec.h"

#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <vector>

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

// Configure one absolute axis with a resolution (units per physical unit).
// For an INPUT_PROP_ACCELEROMETER device the kernel ABI defines resolution as
// units/g for ABS_X/Y/Z and units/(deg/s) for ABS_RX/RY/RZ — that lets evdev
// consumers convert the raw int16 wire values back to physical units.
bool setupAbsRes(int fd, uint16_t code, int32_t min, int32_t max, int32_t resolution) {
    struct uinput_abs_setup abs{};
    abs.code = code;
    abs.absinfo.minimum = min;
    abs.absinfo.maximum = max;
    abs.absinfo.resolution = resolution;
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
    // Stop all reader threads first while holding the lock long enough to take
    // a snapshot of the serials, then drop the lock to join (joining inside
    // the lock would deadlock against the reader trying to take it for the
    // RumbleCallback copy).
    std::vector<uint32_t> serials;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        serials.reserve(devices_.size());
        for (auto& [serial, _] : devices_) serials.push_back(serial);
    }
    for (uint32_t serial : serials) {
        std::lock_guard<std::mutex> lk(mtx_);
        stopReader(serial);
    }

    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& [serial, dev] : devices_) {
        if (dev.fd >= 0) {
            (void)::ioctl(dev.fd, UI_DEV_DESTROY);
            ::close(dev.fd);
        }
        if (dev.motionFd >= 0) {
            (void)::ioctl(dev.motionFd, UI_DEV_DESTROY);
            ::close(dev.motionFd);
        }
        if (dev.touchFd >= 0) {
            (void)::ioctl(dev.touchFd, UI_DEV_DESTROY);
            ::close(dev.touchFd);
        }
    }
    // The relative-mouse pointer node is host-global, not per-controller.
    if (relMouseFd_ >= 0) {
        (void)::ioctl(relMouseFd_, UI_DEV_DESTROY);
        ::close(relMouseFd_);
        relMouseFd_ = -1;
    }
    relMouseBtnDown_ = false;
    devices_.clear();
    busOpen_ = false;
}

bool GamepadAdapter::isBusOpen() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return busOpen_;
}

int GamepadAdapter::openUinputDevice(uint32_t serial, bool ds4) {
    // O_RDWR (not O_WRONLY): we read FF_UPLOAD/FF_ERASE/EV_FF events back from
    // the same fd in the reader thread.
    int fd = ::open("/dev/uinput", O_RDWR | O_NONBLOCK);
    if (fd < 0) return -1;

    // Enable event classes.
    if (::ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0) goto fail;
    if (::ioctl(fd, UI_SET_EVBIT, EV_ABS) < 0) goto fail;
    if (::ioctl(fd, UI_SET_EVBIT, EV_SYN) < 0) goto fail;
    // Force-feedback: enable the EV_FF event class plus the FF_RUMBLE
    // capability so games see the virtual pad as having dual-motor rumble.
    // Without these the kernel rejects every UI_FF_UPLOAD with -EOPNOTSUPP and
    // SDL/Steam Input report the pad as "no haptics".
    if (::ioctl(fd, UI_SET_EVBIT, EV_FF) < 0) goto fail;
    if (::ioctl(fd, UI_SET_FFBIT, FF_RUMBLE) < 0) goto fail;

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
        // Number of effects the kernel will let games upload before recycling
        // slots. 16 matches what the in-tree xpad driver advertises.
        usetup.ff_effects_max = 16;
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

int GamepadAdapter::openMotionUinputDevice(uint32_t serial) {
    // Output-only node — no FF readback — so O_WRONLY is sufficient.
    int fd = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) return -1;

    if (::ioctl(fd, UI_SET_EVBIT, EV_ABS) < 0) goto fail;
    if (::ioctl(fd, UI_SET_EVBIT, EV_SYN) < 0) goto fail;
    // INPUT_PROP_ACCELEROMETER tells userspace that ABS_X/Y/Z + ABS_RX/RY/RZ
    // on this node are an accelerometer + gyroscope, not stick axes — the
    // same convention the in-tree hid-playstation driver uses for the
    // DualSense's dedicated motion device.
    if (::ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_ACCELEROMETER) < 0) goto fail;

    for (int ax : {ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ}) {
        if (::ioctl(fd, UI_SET_ABSBIT, ax) < 0) goto fail;
    }

    // Accelerometer axes (ABS_X/Y/Z): full int16 range; resolution ≈ 8192
    // units/g (wire LSB = 4/32767 g). Gyro axes (ABS_RX/RY/RZ): resolution
    // ≈ 16 units/(deg/s) (wire LSB = 2000/32767 deg/s).
    if (!setupAbsRes(fd, ABS_X, -32768, 32767, 8192)) goto fail;
    if (!setupAbsRes(fd, ABS_Y, -32768, 32767, 8192)) goto fail;
    if (!setupAbsRes(fd, ABS_Z, -32768, 32767, 8192)) goto fail;
    if (!setupAbsRes(fd, ABS_RX, -32768, 32767, 16)) goto fail;
    if (!setupAbsRes(fd, ABS_RY, -32768, 32767, 16)) goto fail;
    if (!setupAbsRes(fd, ABS_RZ, -32768, 32767, 16)) goto fail;

    {
        struct uinput_setup usetup{};
        usetup.id.bustype = BUS_USB;
        usetup.id.vendor = DS4_VID;
        usetup.id.product = DS4_PID;
        usetup.id.version = 0x0100;
        std::snprintf(usetup.name, sizeof(usetup.name),
                      "Satellite Virtual DualShock 4 Motion Sensors #%u", serial);
        if (::ioctl(fd, UI_DEV_SETUP, &usetup) < 0) goto fail;
    }

    if (::ioctl(fd, UI_DEV_CREATE) < 0) goto fail;
    return fd;

fail:
    ::close(fd);
    return -1;
}

int GamepadAdapter::openTouchpadUinputDevice(uint32_t serial) {
    // Output-only node — no FF readback — so O_WRONLY is sufficient.
    int fd = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) return -1;

    if (::ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0) goto fail;
    if (::ioctl(fd, UI_SET_EVBIT, EV_ABS) < 0) goto fail;
    if (::ioctl(fd, UI_SET_EVBIT, EV_SYN) < 0) goto fail;
    // INPUT_PROP_POINTER marks this as an indirect pointing device; the DS4 /
    // DualSense trackpad is a clickpad, so INPUT_PROP_BUTTONPAD too — libinput
    // then treats the whole surface as one big button.
    if (::ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_POINTER) < 0) goto fail;
    if (::ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_BUTTONPAD) < 0) goto fail;

    for (int btn : {BTN_TOUCH, BTN_TOOL_FINGER, BTN_TOOL_DOUBLETAP, BTN_LEFT}) {
        if (::ioctl(fd, UI_SET_KEYBIT, btn) < 0) goto fail;
    }
    // ABS_X/Y carry a single-touch mirror of finger 0; the ABS_MT_* axes carry
    // the full two-finger multitouch (MT-B) stream.
    for (int ax :
         {ABS_X, ABS_Y, ABS_MT_SLOT, ABS_MT_TRACKING_ID, ABS_MT_POSITION_X, ABS_MT_POSITION_Y}) {
        if (::ioctl(fd, UI_SET_ABSBIT, ax) < 0) goto fail;
    }

    // DS4 native touchpad resolution. ABS_MT_SLOT caps at the two-contact pad.
    if (!setupAbs(fd, ABS_X, 0, DS4_TOUCHPAD_RES_X - 1, 0, 0)) goto fail;
    if (!setupAbs(fd, ABS_Y, 0, DS4_TOUCHPAD_RES_Y - 1, 0, 0)) goto fail;
    if (!setupAbs(fd, ABS_MT_SLOT, 0, 1, 0, 0)) goto fail;
    if (!setupAbs(fd, ABS_MT_TRACKING_ID, 0, 65535, 0, 0)) goto fail;
    if (!setupAbs(fd, ABS_MT_POSITION_X, 0, DS4_TOUCHPAD_RES_X - 1, 0, 0)) goto fail;
    if (!setupAbs(fd, ABS_MT_POSITION_Y, 0, DS4_TOUCHPAD_RES_Y - 1, 0, 0)) goto fail;

    {
        struct uinput_setup usetup{};
        usetup.id.bustype = BUS_USB;
        usetup.id.vendor = DS4_VID;
        usetup.id.product = DS4_PID;
        usetup.id.version = 0x0100;
        std::snprintf(usetup.name, sizeof(usetup.name),
                      "Satellite Virtual DualShock 4 Touchpad #%u", serial);
        if (::ioctl(fd, UI_DEV_SETUP, &usetup) < 0) goto fail;
    }

    if (::ioctl(fd, UI_DEV_CREATE) < 0) goto fail;
    return fd;

fail:
    ::close(fd);
    return -1;
}

int GamepadAdapter::openRelMouseUinputDevice() {
    int fd = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) return -1;

    if (::ioctl(fd, UI_SET_EVBIT, EV_REL) < 0) goto fail;
    if (::ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0) goto fail;
    if (::ioctl(fd, UI_SET_EVBIT, EV_SYN) < 0) goto fail;
    if (::ioctl(fd, UI_SET_RELBIT, REL_X) < 0) goto fail;
    if (::ioctl(fd, UI_SET_RELBIT, REL_Y) < 0) goto fail;
    if (::ioctl(fd, UI_SET_KEYBIT, BTN_LEFT) < 0) goto fail;

    {
        struct uinput_setup usetup{};
        usetup.id.bustype = BUS_VIRTUAL;
        usetup.id.vendor = DS4_VID;
        usetup.id.product = DS4_PID;
        usetup.id.version = 0x0100;
        std::snprintf(usetup.name, sizeof(usetup.name), "Satellite Virtual Pointer");
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
    if (devices_.count(serial)) return false;
    int fd = openUinputDevice(serial, /*ds4=*/false);
    if (fd < 0) return false;
    auto& dev = devices_[serial];
    dev.fd = fd;
    dev.ds4 = false;
    startReader(serial, dev);
    return true;
}

bool GamepadAdapter::pluginDeviceDS4(uint32_t serial) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!busOpen_) return false;
    if (devices_.count(serial)) return false;
    int fd = openUinputDevice(serial, /*ds4=*/true);
    if (fd < 0) return false;
    auto& dev = devices_[serial];
    dev.fd = fd;
    dev.ds4 = true;
    // Second uinput node for the DualShock 4 IMU. Best-effort: if it fails
    // the gamepad still works and motion is still cached + DSU-re-emitted by
    // the SessionService — only the local evdev motion node is missing.
    dev.motionFd = openMotionUinputDevice(serial);
    // Third uinput node for the DualShock 4 touchpad (Task 1.3). Also
    // best-effort — a failure just means TOUCHPAD_MODE_DS4 has nowhere to
    // land for this controller; the sample is still cached for the web UI.
    dev.touchFd = openTouchpadUinputDevice(serial);
    startReader(serial, dev);
    return true;
}

void GamepadAdapter::unplugDevice(uint32_t serial) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = devices_.find(serial);
    if (it == devices_.end()) return;
    stopReader(serial); // joins reader thread; safe to take the fd after
    it = devices_.find(serial);
    if (it == devices_.end()) return; // stopReader didn't erase, but be defensive
    if (it->second.fd >= 0) {
        (void)::ioctl(it->second.fd, UI_DEV_DESTROY);
        ::close(it->second.fd);
    }
    if (it->second.motionFd >= 0) {
        (void)::ioctl(it->second.motionFd, UI_DEV_DESTROY);
        ::close(it->second.motionFd);
    }
    if (it->second.touchFd >= 0) {
        (void)::ioctl(it->second.touchFd, UI_DEV_DESTROY);
        ::close(it->second.touchFd);
    }
    devices_.erase(it);
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
    auto it = devices_.find(serial);
    if (it == devices_.end() || it->second.fd < 0) return false;
    int fd = it->second.fd;

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

bool GamepadAdapter::submitMotion(uint32_t serial, const MotionReport& report) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = devices_.find(serial);
    if (it == devices_.end() || it->second.motionFd < 0) return false;
    int fd = it->second.motionFd;

    // Gyro → ABS_RX/RY/RZ, accel → ABS_X/Y/Z. The raw signed-int16 wire values
    // are emitted verbatim; the node's per-axis absinfo.resolution (set in
    // openMotionUinputDevice) describes the units so consumers can convert
    // back to deg/s and g.
    bool ok = true;
    ok &= emit(fd, EV_ABS, ABS_RX, report.gyroX);
    ok &= emit(fd, EV_ABS, ABS_RY, report.gyroY);
    ok &= emit(fd, EV_ABS, ABS_RZ, report.gyroZ);
    ok &= emit(fd, EV_ABS, ABS_X, report.accelX);
    ok &= emit(fd, EV_ABS, ABS_Y, report.accelY);
    ok &= emit(fd, EV_ABS, ABS_Z, report.accelZ);
    ok &= emit(fd, EV_SYN, SYN_REPORT, 0);
    return ok;
}

bool GamepadAdapter::submitTouchpad(uint32_t serial, const TouchpadReport& report) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = devices_.find(serial);
    if (it == devices_.end() || it->second.touchFd < 0) return false;
    Device& dev = it->second;
    const int fd = dev.touchFd;

    bool ok = true;
    const TouchpadFinger* fingers[2] = {&report.finger0, &report.finger1};
    int activeCount = 0;
    for (int slot = 0; slot < 2; ++slot) {
        const TouchpadFinger& f = *fingers[slot];
        ok &= emit(fd, EV_ABS, ABS_MT_SLOT, slot);
        if (f.active) {
            ++activeCount;
            // Allocate a fresh tracking id on a touch-down and reuse it for the
            // life of the contact so MT-B consumers see one continuous finger.
            if (dev.touchSlotId[slot] < 0) {
                dev.touchSlotId[slot] = (dev.touchTrackingId++) & 0xFFFF;
            }
            ok &= emit(fd, EV_ABS, ABS_MT_TRACKING_ID, dev.touchSlotId[slot]);
            ok &= emit(fd, EV_ABS, ABS_MT_POSITION_X, touchpadWireToRange(f.x, DS4_TOUCHPAD_RES_X));
            ok &= emit(fd, EV_ABS, ABS_MT_POSITION_Y, touchpadWireToRange(f.y, DS4_TOUCHPAD_RES_Y));
        } else if (dev.touchSlotId[slot] >= 0) {
            // Finger lifted — release the MT slot (tracking id -1).
            dev.touchSlotId[slot] = -1;
            ok &= emit(fd, EV_ABS, ABS_MT_TRACKING_ID, -1);
        }
    }

    ok &= emit(fd, EV_KEY, BTN_TOUCH, activeCount > 0 ? 1 : 0);
    ok &= emit(fd, EV_KEY, BTN_TOOL_FINGER, activeCount == 1 ? 1 : 0);
    ok &= emit(fd, EV_KEY, BTN_TOOL_DOUBLETAP, activeCount == 2 ? 1 : 0);
    ok &= emit(fd, EV_KEY, BTN_LEFT, report.buttonPressed ? 1 : 0);

    // Single-touch (ST) compatibility: mirror finger 0 onto ABS_X/Y so non-MT
    // consumers still track the primary contact.
    if (report.finger0.active) {
        ok &= emit(fd, EV_ABS, ABS_X, touchpadWireToRange(report.finger0.x, DS4_TOUCHPAD_RES_X));
        ok &= emit(fd, EV_ABS, ABS_Y, touchpadWireToRange(report.finger0.y, DS4_TOUCHPAD_RES_Y));
    }

    ok &= emit(fd, EV_SYN, SYN_REPORT, 0);
    return ok;
}

bool GamepadAdapter::submitRelativeMouse(int dx, int dy, bool leftButton) {
    std::lock_guard<std::mutex> lk(mtx_);
    // The pointer node is host-global and lazily created — the first
    // TOUCHPAD_MODE_MOUSE sample is what brings it into existence.
    if (relMouseFd_ < 0) {
        relMouseFd_ = openRelMouseUinputDevice();
        if (relMouseFd_ < 0) return false;
    }
    const int fd = relMouseFd_;

    bool ok = true;
    if (dx != 0) ok &= emit(fd, EV_REL, REL_X, dx);
    if (dy != 0) ok &= emit(fd, EV_REL, REL_Y, dy);
    // Emit BTN_LEFT only on a level change so a held click presses once.
    if (leftButton != relMouseBtnDown_) {
        relMouseBtnDown_ = leftButton;
        ok &= emit(fd, EV_KEY, BTN_LEFT, leftButton ? 1 : 0);
    }
    ok &= emit(fd, EV_SYN, SYN_REPORT, 0);
    return ok;
}

// ── Rumble callback registration ────────────────────────────────────────────

void GamepadAdapter::setRumbleCallback(RumbleCallback cb) {
    std::lock_guard<std::mutex> lk(mtx_);
    rumbleCb_ = std::move(cb);
}

// Caller holds mtx_.
void GamepadAdapter::startReader(uint32_t serial, Device& dev) {
    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0) return;
    // Non-blocking on the read side so a spurious wake doesn't stall the
    // reader if the write side closes without a byte.
    int flags = ::fcntl(fds[0], F_GETFL, 0);
    if (flags >= 0) ::fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);
    dev.wakePipeRead = fds[0];
    dev.wakePipeWrite = fds[1];
    dev.readerRunning.store(true, std::memory_order_release);
    int devFd = dev.fd;
    int wakeFd = dev.wakePipeRead;
    bool isDS4 = dev.ds4;
    dev.readerThread = std::thread(
        [this, serial, devFd, wakeFd, isDS4] { readerLoop(serial, devFd, wakeFd, isDS4); });
}

// Caller holds mtx_. Mirrors the Windows ViGEm pattern: extract under the
// lock, drop it for the join, retake it. Joining inline would deadlock with
// the reader's lock acquisition for the rumble callback copy.
void GamepadAdapter::stopReader(uint32_t serial) {
    auto it = devices_.find(serial);
    if (it == devices_.end()) return;
    auto& dev = it->second;
    if (!dev.readerRunning.exchange(false, std::memory_order_acq_rel)) return;
    int wakeWrite = dev.wakePipeWrite;
    int wakeRead = dev.wakePipeRead;
    dev.wakePipeWrite = -1;
    dev.wakePipeRead = -1;
    std::thread th = std::move(dev.readerThread);
    if (wakeWrite >= 0) {
        // One byte is enough — the reader's poll wakes immediately and exits.
        const char b = 0;
        (void)::write(wakeWrite, &b, 1);
    }
    mtx_.unlock();
    if (th.joinable()) th.join();
    if (wakeWrite >= 0) ::close(wakeWrite);
    if (wakeRead >= 0) ::close(wakeRead);
    mtx_.lock();
}

void GamepadAdapter::readerLoop(uint32_t serial, int fd, int wakeFd, bool isDS4) {
    (void)isDS4; // FF_RUMBLE shape is identical for Xbox and DS4 profiles.

    while (true) {
        struct pollfd pfds[2]{};
        pfds[0].fd = fd;
        pfds[0].events = POLLIN;
        pfds[1].fd = wakeFd;
        pfds[1].events = POLLIN;
        int rc = ::poll(pfds, 2, -1);
        if (rc < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (pfds[1].revents & POLLIN) return; // unplug / closeBus

        if (!(pfds[0].revents & POLLIN)) continue;

        struct input_event ev{};
        ssize_t n = ::read(fd, &ev, sizeof(ev));
        if (n != (ssize_t)sizeof(ev)) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
            return;
        }

        // Three event types matter to us:
        //   EV_UINPUT.UI_FF_UPLOAD — game registered a new effect; reply with
        //     the assigned effect-id and cache its strong/weak magnitudes.
        //   EV_UINPUT.UI_FF_ERASE  — game freed an effect-id; drop the cached
        //     entry. Must be acknowledged or the kernel rejects future uploads.
        //   EV_FF                  — game pressed play/stop on an effect-id.
        //     value > 0 plays, value == 0 stops; magnitudes come from the
        //     cached effect descriptor.
        if (ev.type == EV_UINPUT && ev.code == UI_FF_UPLOAD) {
            struct uinput_ff_upload upload{};
            upload.request_id = ev.value;
            if (::ioctl(fd, UI_BEGIN_FF_UPLOAD, &upload) == 0) {
                upload.retval = 0;
                int effectId = upload.effect.id;
                if (upload.effect.type == FF_RUMBLE) {
                    Device::EffectMags m{
                        upload.effect.u.rumble.strong_magnitude,
                        upload.effect.u.rumble.weak_magnitude,
                    };
                    std::lock_guard<std::mutex> lk(mtx_);
                    auto it = devices_.find(serial);
                    if (it != devices_.end()) it->second.effects[effectId] = m;
                }
                (void)::ioctl(fd, UI_END_FF_UPLOAD, &upload);
            }
        } else if (ev.type == EV_UINPUT && ev.code == UI_FF_ERASE) {
            struct uinput_ff_erase erase{};
            erase.request_id = ev.value;
            if (::ioctl(fd, UI_BEGIN_FF_ERASE, &erase) == 0) {
                erase.retval = 0;
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    auto it = devices_.find(serial);
                    if (it != devices_.end()) it->second.effects.erase(erase.effect_id);
                }
                (void)::ioctl(fd, UI_END_FF_ERASE, &erase);
            }
        } else if (ev.type == EV_FF) {
            int effectId = ev.code;
            int playValue = ev.value; // 0 = stop, >0 = play
            RumbleReport rr{};
            RumbleCallback cb;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                auto dit = devices_.find(serial);
                if (dit == devices_.end()) continue;
                auto eit = dit->second.effects.find(effectId);
                if (playValue > 0 && eit != dit->second.effects.end()) {
                    rr.strongMagnitude = eit->second.strong;
                    rr.weakMagnitude = eit->second.weak;
                }
                // playValue == 0 → keep magnitudes at 0 (stop)
                cb = rumbleCb_;
            }
            if (cb) cb(serial, rr);
        }
    }
}
