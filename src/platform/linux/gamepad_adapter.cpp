// SPDX-License-Identifier: LGPL-3.0-or-later
#include "gamepad_adapter.h"

#include "core/touchpad_codec.h"
#include "core/types.h"

#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// Forward-declared to avoid pulling app/app_state.h (httplib + net_compat.h)
// into this TU. Defined in platform/linux/globals.cpp.
void logMsg(LogLevel level, const std::string& source, const std::string& message);

namespace {

constexpr uint16_t XBOX_VID = 0x045e;
constexpr uint16_t XBOX_PID = 0x028e;
constexpr uint16_t DS4_VID = 0x054c;
constexpr uint16_t DS4_PID = 0x05c4;
constexpr uint16_t DUALSENSE_VID = 0x054c;
constexpr uint16_t DUALSENSE_PID = 0x0ce6;
constexpr uint16_t SWITCHPRO_VID = 0x057e;
constexpr uint16_t SWITCHPRO_PID = 0x2009;

// Buttons exposed on the Xbox/DS4/DualSense profiles (evdev codes).
constexpr int BUTTONS[] = {BTN_A,      BTN_B,     BTN_X,    BTN_Y,      BTN_TL,    BTN_TR,
                           BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR};

// Switch Pro: 14 CONTIGUOUS evdev codes 0x130-0x13d so SDL's built-in Nintendo
// mapping (which keys on js index = ascending-code position) lines up exactly.
// Order + the submit remap were derived empirically from SDL's gamecontrollerdb.
constexpr int SWITCH_BUTTONS[] = {BTN_A,      BTN_B,     BTN_C,    BTN_X,     BTN_Y,
                                  BTN_Z,      BTN_TL,    BTN_TR,   BTN_TL2,   BTN_TR2,
                                  BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL};
constexpr int SWITCH_TRIGGER_ON = 30; // ZL/ZR are digital; press past the XInput threshold.

bool emit(int fd, uint16_t type, uint16_t code, int32_t value) {
    struct input_event ev{};
    ev.type = type;
    ev.code = code;
    ev.value = value;
    return ::write(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev);
}

// Configure one absolute axis via UI_ABS_SETUP (kernel >= 4.5).
bool setupAbs(int fd, uint16_t code, int32_t min, int32_t max, int32_t flat, int32_t fuzz) {
    struct uinput_abs_setup abs{};
    abs.code = code;
    abs.absinfo.minimum = min;
    abs.absinfo.maximum = max;
    abs.absinfo.flat = flat;
    abs.absinfo.fuzz = fuzz;
    return ::ioctl(fd, UI_ABS_SETUP, &abs) == 0;
}

// For INPUT_PROP_ACCELEROMETER the kernel ABI reads resolution as units/g
// (ABS_X/Y/Z) and units/(deg/s) (ABS_RX/RY/RZ), letting consumers convert raw
// int16 wire values back to physical units.
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
    // "Bus" here is just the uinput node; per-device fds open per plugin.
    if (::access("/dev/uinput", W_OK) != 0) return false;
    busOpen_ = true;
    return true;
}

void GamepadAdapter::closeBus() {
    // Snapshot serials under the lock, then join outside it: joining while
    // holding mtx_ deadlocks against the reader taking it for the rumble copy.
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
    // Pointer node is host-global, not per-controller.
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

int GamepadAdapter::openUinputDevice(uint32_t serial, GamepadIdentity identity) {
    // O_RDWR (not O_WRONLY): the reader thread reads FF_UPLOAD/FF_ERASE/EV_FF back.
    int fd = ::open("/dev/uinput", O_RDWR | O_NONBLOCK);
    if (fd < 0) return -1;
    const bool sw = identity == GamepadIdentity::SwitchPro;

    if (::ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0) goto fail;
    if (::ioctl(fd, UI_SET_EVBIT, EV_ABS) < 0) goto fail;
    if (::ioctl(fd, UI_SET_EVBIT, EV_SYN) < 0) goto fail;
    // Without EV_FF + FF_RUMBLE the kernel rejects every UI_FF_UPLOAD with
    // -EOPNOTSUPP and SDL/Steam Input report the pad as "no haptics".
    if (::ioctl(fd, UI_SET_EVBIT, EV_FF) < 0) goto fail;
    if (::ioctl(fd, UI_SET_FFBIT, FF_RUMBLE) < 0) goto fail;

    if (sw) {
        for (int btn : SWITCH_BUTTONS) {
            if (::ioctl(fd, UI_SET_KEYBIT, btn) < 0) goto fail;
        }
    } else {
        for (int btn : BUTTONS) {
            if (::ioctl(fd, UI_SET_KEYBIT, btn) < 0) goto fail;
        }
    }

    for (int ax : {ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_HAT0X, ABS_HAT0Y}) {
        if (::ioctl(fd, UI_SET_ABSBIT, ax) < 0) goto fail;
    }
    // Switch Pro's ZL/ZR are digital buttons, not analog axes.
    if (!sw) {
        for (int ax : {ABS_Z, ABS_RZ}) {
            if (::ioctl(fd, UI_SET_ABSBIT, ax) < 0) goto fail;
        }
    }

    // range/flat/fuzz matches xpad driver defaults.
    if (!setupAbs(fd, ABS_X, -32768, 32767, 128, 16)) goto fail;
    if (!setupAbs(fd, ABS_Y, -32768, 32767, 128, 16)) goto fail;
    if (!setupAbs(fd, ABS_RX, -32768, 32767, 128, 16)) goto fail;
    if (!setupAbs(fd, ABS_RY, -32768, 32767, 128, 16)) goto fail;
    if (!sw) {
        if (!setupAbs(fd, ABS_Z, 0, 255, 0, 0)) goto fail;
        if (!setupAbs(fd, ABS_RZ, 0, 255, 0, 0)) goto fail;
    }
    if (!setupAbs(fd, ABS_HAT0X, -1, 1, 0, 0)) goto fail;
    if (!setupAbs(fd, ABS_HAT0Y, -1, 1, 0, 0)) goto fail;

    {
        struct uinput_setup usetup{};
        usetup.id.bustype = BUS_USB;
        // VID/PID + name pick the identity SDL2/Steam Input recognize; the evdev
        // button/axis set is shared (they remap face buttons per their own DB).
        const char* name;
        switch (identity) {
        case GamepadIdentity::DS4:
            usetup.id.vendor = DS4_VID;
            usetup.id.product = DS4_PID;
            usetup.id.version = 0x0100;
            name = "Satellite Virtual DualShock 4";
            break;
        case GamepadIdentity::DualSense:
            usetup.id.vendor = DUALSENSE_VID;
            usetup.id.product = DUALSENSE_PID;
            usetup.id.version = 0x0100;
            name = "Satellite Virtual DualSense";
            break;
        case GamepadIdentity::SwitchPro:
            usetup.id.vendor = SWITCHPRO_VID;
            usetup.id.product = SWITCHPRO_PID;
            usetup.id.version = 0x0001;
            name = "Satellite Virtual Switch Pro Controller";
            break;
        default:
            usetup.id.vendor = XBOX_VID;
            usetup.id.product = XBOX_PID;
            usetup.id.version = 0x0110;
            name = "Satellite Virtual Xbox 360 Pad";
            break;
        }
        usetup.ff_effects_max = 16; // matches the in-tree xpad driver
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
    // Output-only node (no FF readback), so O_WRONLY suffices.
    int fd = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) return -1;

    if (::ioctl(fd, UI_SET_EVBIT, EV_ABS) < 0) goto fail;
    if (::ioctl(fd, UI_SET_EVBIT, EV_SYN) < 0) goto fail;
    // INPUT_PROP_ACCELEROMETER marks ABS_X/Y/Z + RX/RY/RZ as accel+gyro, not
    // stick axes; same convention as hid-playstation's DualSense motion node.
    if (::ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_ACCELEROMETER) < 0) goto fail;

    for (int ax : {ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ}) {
        if (::ioctl(fd, UI_SET_ABSBIT, ax) < 0) goto fail;
    }

    // Accel ABS_X/Y/Z: resolution ~8192 units/g (wire LSB = 4/32767 g).
    // Gyro ABS_RX/RY/RZ: ~16 units/(deg/s) (wire LSB = 2000/32767 deg/s).
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
    // Output-only node (no FF readback), so O_WRONLY suffices.
    int fd = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) return -1;

    if (::ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0) goto fail;
    if (::ioctl(fd, UI_SET_EVBIT, EV_ABS) < 0) goto fail;
    if (::ioctl(fd, UI_SET_EVBIT, EV_SYN) < 0) goto fail;
    // POINTER = indirect pointing device; BUTTONPAD because the DS4 trackpad is
    // a clickpad, so libinput treats the whole surface as one button.
    if (::ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_POINTER) < 0) goto fail;
    if (::ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_BUTTONPAD) < 0) goto fail;

    for (int btn : {BTN_TOUCH, BTN_TOOL_FINGER, BTN_TOOL_DOUBLETAP, BTN_LEFT}) {
        if (::ioctl(fd, UI_SET_KEYBIT, btn) < 0) goto fail;
    }
    // ABS_X/Y mirror finger 0 (single-touch); ABS_MT_* carry the two-finger MT-B stream.
    for (int ax :
         {ABS_X, ABS_Y, ABS_MT_SLOT, ABS_MT_TRACKING_ID, ABS_MT_POSITION_X, ABS_MT_POSITION_Y}) {
        if (::ioctl(fd, UI_SET_ABSBIT, ax) < 0) goto fail;
    }

    // DS4 native touchpad resolution; ABS_MT_SLOT caps at the two-contact pad.
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

bool GamepadAdapter::pluginDevice(uint32_t serial, GamepadIdentity identity) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!busOpen_) return false;
    if (devices_.count(serial)) return false;
    int fd = openUinputDevice(serial, identity);
    if (fd < 0) return false;
    auto& dev = devices_[serial];
    dev.fd = fd;
    dev.identity = identity;

    // DS4/DualSense have an IMU + touchpad; Switch Pro has an IMU, no touchpad.
    const bool ds4Family =
        identity == GamepadIdentity::DS4 || identity == GamepadIdentity::DualSense;
    const bool hasMotion = ds4Family || identity == GamepadIdentity::SwitchPro;
    if (hasMotion) {
        // Best-effort: on failure the gamepad still works and the sample is
        // still cached by SessionService; only the evdev node is missing.
        dev.motionFd = openMotionUinputDevice(serial);
        if (dev.motionFd < 0) {
            // Log so a too-old kernel / missing permission / exhausted /tmp
            // doesn't look like "no game subscribed". Pairs with motionBackendOk().
            std::fprintf(stderr,
                         "satellite: failed to open motion uinput device for serial=%u; "
                         "motion samples will be cached but not forwarded\n",
                         serial);
        }
    }
    if (ds4Family) { dev.touchFd = openTouchpadUinputDevice(serial); }
    startReader(serial, dev);
    return true;
}

bool GamepadAdapter::unplugDevice(uint32_t serial) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = devices_.find(serial);
    if (it == devices_.end()) return true; // never plugged, already gone
    stopReader(serial);                    // joins reader thread; safe to take the fd after
    it = devices_.find(serial);
    if (it == devices_.end()) return true; // defensive: stopReader doesn't erase
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
    // close(fd) destroys a uinput device synchronously even if UI_DEV_DESTROY
    // failed, so removal is confirmed by reaching here.
    return true;
}

bool GamepadAdapter::isDevicePlugged(uint32_t serial) const {
    std::lock_guard<std::mutex> lk(mtx_);
    return devices_.find(serial) != devices_.end();
}

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

// Clamp so negating -32768 doesn't overflow when we flip Y for evdev's
// "down is positive" convention.
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
    if (it->second.identity == GamepadIdentity::SwitchPro) return submitSwitchLocked(fd, report);

    bool ok = true;
    // XUSB Y is positive-up; evdev Y is positive-down, so invert.
    ok &= emit(fd, EV_ABS, ABS_X, report.sThumbLX);
    ok &= emit(fd, EV_ABS, ABS_Y, -clampS16(report.sThumbLY));
    ok &= emit(fd, EV_ABS, ABS_RX, report.sThumbRX);
    ok &= emit(fd, EV_ABS, ABS_RY, -clampS16(report.sThumbRY));

    ok &= emit(fd, EV_ABS, ABS_Z, report.bLeftTrigger);
    ok &= emit(fd, EV_ABS, ABS_RZ, report.bRightTrigger);

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

    ok &= emit(fd, EV_SYN, SYN_REPORT, 0);
    return ok;
}

bool GamepadAdapter::submitSwitchLocked(int fd, const GamepadReport& report) {
    bool ok = true;
    ok &= emit(fd, EV_ABS, ABS_X, report.sThumbLX);
    ok &= emit(fd, EV_ABS, ABS_Y, -clampS16(report.sThumbLY));
    ok &= emit(fd, EV_ABS, ABS_RX, report.sThumbRX);
    ok &= emit(fd, EV_ABS, ABS_RY, -clampS16(report.sThumbRY));

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

    // SWITCH_BUTTONS[i] is SDL js index i; the wire source per index follows SDL's
    // Nintendo mapping (A/B + X/Y swapped; ZL/ZR digital from the triggers; capture
    // at js4 has no wire source).
    const uint16_t b = report.wButtons;
    ok &= emit(fd, EV_KEY, BTN_A, (b & XUSB_B) ? 1 : 0);                               // js0  B
    ok &= emit(fd, EV_KEY, BTN_B, (b & XUSB_A) ? 1 : 0);                               // js1  A
    ok &= emit(fd, EV_KEY, BTN_C, (b & XUSB_X) ? 1 : 0);                               // js2  X
    ok &= emit(fd, EV_KEY, BTN_X, (b & XUSB_Y) ? 1 : 0);                               // js3  Y
    ok &= emit(fd, EV_KEY, BTN_Z, (b & XUSB_LB) ? 1 : 0);                              // js5  L
    ok &= emit(fd, EV_KEY, BTN_TL, (b & XUSB_RB) ? 1 : 0);                             // js6  R
    ok &= emit(fd, EV_KEY, BTN_TR, report.bLeftTrigger > SWITCH_TRIGGER_ON ? 1 : 0);   // js7  ZL
    ok &= emit(fd, EV_KEY, BTN_TL2, report.bRightTrigger > SWITCH_TRIGGER_ON ? 1 : 0); // js8  ZR
    ok &= emit(fd, EV_KEY, BTN_TR2, (b & XUSB_BACK) ? 1 : 0);                          // js9  minus
    ok &= emit(fd, EV_KEY, BTN_SELECT, (b & XUSB_START) ? 1 : 0);                      // js10 plus
    ok &= emit(fd, EV_KEY, BTN_START, (b & XUSB_GUIDE) ? 1 : 0);                       // js11 home
    ok &= emit(fd, EV_KEY, BTN_MODE, (b & XUSB_LS) ? 1 : 0);   // js12 Lstick
    ok &= emit(fd, EV_KEY, BTN_THUMBL, (b & XUSB_RS) ? 1 : 0); // js13 Rstick

    ok &= emit(fd, EV_SYN, SYN_REPORT, 0);
    return ok;
}

bool GamepadAdapter::submitMotion(uint32_t serial, const MotionReport& report) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = devices_.find(serial);
    if (it == devices_.end() || it->second.motionFd < 0) return false;
    int fd = it->second.motionFd;

    // Raw int16 wire values emitted verbatim; the node's absinfo.resolution
    // carries the units (see openMotionUinputDevice).
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

bool GamepadAdapter::supportsIdentity(GamepadIdentity identity) const {
    // uinput can stamp any VID/PID; all four are wired (Switch Pro via a distinct
    // evdev layout + submit remap, empirically fitted to SDL's Nintendo mapping).
    return identity == GamepadIdentity::Xbox || identity == GamepadIdentity::DS4 ||
           identity == GamepadIdentity::DualSense || identity == GamepadIdentity::SwitchPro;
}

bool GamepadAdapter::supportsMotionForType(uint8_t controllerType) const {
    // Backend-shape, not per-serial: the motion-capable types get a motion node.
    return supportsIdentity(controllerIdentity(controllerType)) &&
           controllerTypeHasMotion(controllerType);
}

bool GamepadAdapter::motionBackendOk(uint32_t serial) const {
    // An unknown serial (unplug/query race) reads true: false would surface as a
    // phantom "broken backend" badge in the web UI when there's nothing to report.
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = devices_.find(serial);
    if (it == devices_.end()) return true;
    return it->second.motionFd >= 0;
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
            // Fresh tracking id on touch-down, reused for the contact's life so
            // MT-B consumers see one continuous finger.
            if (dev.touchSlotId[slot] < 0) {
                dev.touchSlotId[slot] = (dev.touchTrackingId++) & 0xFFFF;
            }
            ok &= emit(fd, EV_ABS, ABS_MT_TRACKING_ID, dev.touchSlotId[slot]);
            ok &= emit(fd, EV_ABS, ABS_MT_POSITION_X, touchpadWireToRange(f.x, DS4_TOUCHPAD_RES_X));
            ok &= emit(fd, EV_ABS, ABS_MT_POSITION_Y, touchpadWireToRange(f.y, DS4_TOUCHPAD_RES_Y));
        } else if (dev.touchSlotId[slot] >= 0) {
            // Finger lifted: release the MT slot (tracking id -1).
            dev.touchSlotId[slot] = -1;
            ok &= emit(fd, EV_ABS, ABS_MT_TRACKING_ID, -1);
        }
    }

    ok &= emit(fd, EV_KEY, BTN_TOUCH, activeCount > 0 ? 1 : 0);
    ok &= emit(fd, EV_KEY, BTN_TOOL_FINGER, activeCount == 1 ? 1 : 0);
    ok &= emit(fd, EV_KEY, BTN_TOOL_DOUBLETAP, activeCount == 2 ? 1 : 0);
    ok &= emit(fd, EV_KEY, BTN_LEFT, report.buttonPressed ? 1 : 0);

    // Mirror finger 0 onto ABS_X/Y so non-MT consumers track the primary contact.
    if (report.finger0.active) {
        ok &= emit(fd, EV_ABS, ABS_X, touchpadWireToRange(report.finger0.x, DS4_TOUCHPAD_RES_X));
        ok &= emit(fd, EV_ABS, ABS_Y, touchpadWireToRange(report.finger0.y, DS4_TOUCHPAD_RES_Y));
    }

    ok &= emit(fd, EV_SYN, SYN_REPORT, 0);
    return ok;
}

bool GamepadAdapter::submitRelativeMouse(int dx, int dy, bool leftButton) {
    std::lock_guard<std::mutex> lk(mtx_);
    // Pointer node is host-global, created lazily by the first sample.
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

// uinput has no battery or RGB channel (no UI_SET_BATTERY; EV_LED is single-bit),
// so mirror each update to /tmp/satellite/controller<serial>/ for operator
// userspace (OBS overlays, LED daemons). SessionService still caches the sample.

std::string GamepadAdapter::sysfsProxyDir() {
    // SATELLITE_SYSFS_PROXY_DIR lets tests redirect to a hermetic temp dir.
    if (const char* env = std::getenv("SATELLITE_SYSFS_PROXY_DIR")) {
        if (env[0] != '\0') return std::string(env);
    }
    return "/tmp/satellite";
}

bool GamepadAdapter::writeSysfsProxyFile(uint32_t serial, const char* leaf,
                                         const std::string& contents) {
    const std::string base = sysfsProxyDir();
    if (::mkdir(base.c_str(), 0755) != 0 && errno != EEXIST) return false;
    char ctrlDir[64];
    std::snprintf(ctrlDir, sizeof(ctrlDir), "/controller%u", serial);
    const std::string dir = base + ctrlDir;
    if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) return false;

    const std::string path = dir + "/" + leaf;
    // Plain O_TRUNC (no rename): telemetry only, so a torn read yields stale
    // text, not corruption, and no rename-tmps leak on crash.
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    ssize_t want = static_cast<ssize_t>(contents.size());
    ssize_t got = ::write(fd, contents.data(), contents.size());
    ::close(fd);
    return got == want;
}

bool GamepadAdapter::submitBattery(uint32_t serial, const BatteryReport& report) {
    // "key=value" lines mirror the wire encoding: level 0..100 or 255 unknown;
    // status 0..4 per BATTERY_STATUS_*.
    char buf[64];
    int n =
        std::snprintf(buf, sizeof(buf), "level=%u\nstatus=%u\n",
                      static_cast<unsigned>(report.level), static_cast<unsigned>(report.status));
    if (n <= 0) return false;
    bool ok = writeSysfsProxyFile(serial, "battery", std::string(buf, static_cast<size_t>(n)));

    char logbuf[96];
    std::snprintf(logbuf, sizeof(logbuf), "controller %u battery level=%u status=%u (%s)", serial,
                  static_cast<unsigned>(report.level), static_cast<unsigned>(report.status),
                  ok ? "proxy ok" : "proxy write failed");
    logMsg(ok ? LogLevel::INFO : LogLevel::WARN, "uinput", logbuf);
    return ok;
}

#ifdef SATELLITE_BUILD_TESTS
void GamepadAdapter::invokeLightbarForTest(uint32_t serial, uint8_t r, uint8_t g, uint8_t b) {
    LightbarCallback cb;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        cb = lightbarCb_;
    }
    if (cb) cb(serial, r, g, b);
}
#endif

void GamepadAdapter::setLightbarCallback(LightbarCallback cb) {
    // Rarely fires on uinput (no RGB readback) but keeps parity with ViGEm.
    std::lock_guard<std::mutex> lk(mtx_);
    LightbarCallback inner = std::move(cb);
    lightbarCb_ = [inner](uint32_t serial, uint8_t r, uint8_t g, uint8_t b) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%02X%02X%02X\n", r, g, b);
        bool ok = writeSysfsProxyFile(serial, "lightbar", std::string(buf));

        char logbuf[96];
        std::snprintf(logbuf, sizeof(logbuf), "controller %u lightbar #%02X%02X%02X (%s)", serial,
                      r, g, b, ok ? "proxy ok" : "proxy write failed");
        logMsg(ok ? LogLevel::INFO : LogLevel::WARN, "uinput", logbuf);

        if (inner) inner(serial, r, g, b);
    };
}

void GamepadAdapter::setRumbleCallback(RumbleCallback cb) {
    std::lock_guard<std::mutex> lk(mtx_);
    rumbleCb_ = std::move(cb);
}

// Caller holds mtx_.
void GamepadAdapter::startReader(uint32_t serial, Device& dev) {
    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0) return;
    // Non-blocking read side so a spurious wake (write side closed without a
    // byte) doesn't stall the reader.
    int flags = ::fcntl(fds[0], F_GETFL, 0);
    if (flags >= 0) ::fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);
    dev.wakePipeRead = fds[0];
    dev.wakePipeWrite = fds[1];
    dev.readerRunning.store(true, std::memory_order_release);
    int devFd = dev.fd;
    int wakeFd = dev.wakePipeRead;
    bool isDS4 = dev.identity == GamepadIdentity::DS4 || dev.identity == GamepadIdentity::DualSense;
    dev.readerThread = std::thread(
        [this, serial, devFd, wakeFd, isDS4] { readerLoop(serial, devFd, wakeFd, isDS4); });
}

// Caller holds mtx_. Extract under the lock, drop it for the join, retake it:
// joining inline deadlocks with the reader taking the lock for the rumble copy.
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
        // EAGAIN: a wake byte is already queued, so the reader will still exit.
        const char b = 0;
        ssize_t n;
        do { n = ::write(wakeWrite, &b, 1); } while (n < 0 && errno == EINTR);
        if (n < 0 && errno != EAGAIN) {
            std::fprintf(stderr, "satellite: reader wake-pipe write failed for serial=%u: %s\n",
                         serial, std::strerror(errno));
        }
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

        // FF handshake: UI_FF_UPLOAD registers an effect (cache its magnitudes);
        // UI_FF_ERASE frees one (must be acked or the kernel rejects future
        // uploads); EV_FF plays/stops it (value>0 play, ==0 stop).
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
            int playValue = ev.value;
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
                // playValue == 0 leaves magnitudes at 0 (stop).
                cb = rumbleCb_;
            }
            if (cb) cb(serial, rr);
        }
    }
}
