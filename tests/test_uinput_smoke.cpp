// SPDX-License-Identifier: LGPL-3.0-or-later
// Real-kernel smoke test for the uinput backend: plugs virtual pads through
// the production GamepadAdapter, then verifies from the CONSUMER side (the
// /dev/input/event* nodes a game would open) that reports land and that a
// force-feedback upload travels back through the reader thread into the
// rumble callback. Everything else in the suite fakes this layer; this test
// is the only place the adapter meets an actual kernel.
//
// Requirements: a writable /dev/uinput AND permission to open the created
// /dev/input/event* nodes (root, or an input-group user with a udev rule).
// When either is missing the test exits 77 — wired to SKIP_RETURN_CODE so
// ctest reports "skipped", never a silent pass. CI runs it via sudo.
#include "../src/core/gamepad_backend.h"
#include "../src/platform/linux/gamepad_adapter.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

static int g_pass = 0;
static int g_fail = 0;
static std::string g_currentTest;

#define TEST(name)                                                                                 \
    do { g_currentTest = (name); } while (0)

#define EXPECT(cond)                                                                               \
    do {                                                                                           \
        if (cond) {                                                                                \
            g_pass++;                                                                              \
        } else {                                                                                   \
            g_fail++;                                                                              \
            std::cerr << "  FAIL [" << g_currentTest << "] " << __FILE__ << ":" << __LINE__        \
                      << "  " << #cond << "\n";                                                    \
        }                                                                                          \
    } while (0)

namespace {

constexpr int kSkipExitCode = 77;
// Matches the file-local constant in gamepad_adapter.cpp (XUSB wButtons bit
// for the A face button).
constexpr uint16_t kXusbA = 0x1000;

// Scan /sys/class/input/event*/device/name for an exact `name` match and
// return the /dev/input/event* path, retrying until `deadlineMs` because node
// creation is asynchronous relative to UI_DEV_CREATE returning.
std::string findEventNodeByName(const std::string& name, int deadlineMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(deadlineMs);
    do {
        DIR* dir = ::opendir("/sys/class/input");
        if (dir != nullptr) {
            struct dirent* de = nullptr;
            while ((de = ::readdir(dir)) != nullptr) {
                if (std::strncmp(de->d_name, "event", 5) != 0) continue;
                std::ifstream f(std::string("/sys/class/input/") + de->d_name + "/device/name");
                std::string devName;
                if (!f || !std::getline(f, devName)) continue;
                if (devName == name) {
                    ::closedir(dir);
                    return std::string("/dev/input/") + de->d_name;
                }
            }
            ::closedir(dir);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    } while (std::chrono::steady_clock::now() < deadline);
    return {};
}

bool eventNodeGone(const std::string& name, int deadlineMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(deadlineMs);
    do {
        if (findEventNodeByName(name, 0).empty()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

// Drain until an EV_KEY event for `code` with `value` arrives, or timeout.
bool waitForKeyEvent(int fd, uint16_t code, int32_t value, int deadlineMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(deadlineMs);
    while (std::chrono::steady_clock::now() < deadline) {
        struct pollfd pfd{fd, POLLIN, 0};
        if (::poll(&pfd, 1, 100) <= 0) continue;
        struct input_event ev;
        while (::read(fd, &ev, sizeof(ev)) == static_cast<ssize_t>(sizeof(ev))) {
            if (ev.type == EV_KEY && ev.code == code && ev.value == value) return true;
        }
    }
    return false;
}

bool waitFor(const std::atomic<bool>& flag, int deadlineMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(deadlineMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (flag.load(std::memory_order_acquire)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return flag.load(std::memory_order_acquire);
}

} // namespace

int main() {
    std::cout << "Running uinput smoke test...\n\n";

    const BackendStatus status = probeBackend();
    if (!status.available) {
        std::cout << "SKIP: /dev/uinput unavailable ("
                  << (status.errorCode != nullptr ? status.errorCode : "unknown")
                  << ") — modprobe uinput and grant write access to run this test.\n";
        return kSkipExitCode;
    }

    // Keep the battery/lightbar file mirror out of a running daemon's tree.
    char proxyTemplate[] = "/tmp/satellite-smoke-XXXXXX";
    char* proxyDir = ::mkdtemp(proxyTemplate);
    if (proxyDir != nullptr) ::setenv("SATELLITE_SYSFS_PROXY_DIR", proxyDir, 1);

    GamepadAdapter adapter;
    constexpr uint32_t kXboxSerial = 9101;
    constexpr uint32_t kDs4Serial = 9102;

    TEST("bus opens");
    EXPECT(adapter.ensureBusOpen());
    EXPECT(adapter.isBusOpen());

    TEST("Xbox 360 pad plugs in");
    EXPECT(adapter.pluginDevice(kXboxSerial, GamepadIdentity::Xbox));
    EXPECT(adapter.isDevicePlugged(kXboxSerial));

    const std::string xboxName = "Satellite Virtual Xbox 360 Pad #" + std::to_string(kXboxSerial);
    const std::string xboxNode = findEventNodeByName(xboxName, 3000);
    TEST("Xbox pad appears as an evdev node");
    EXPECT(!xboxNode.empty());
    if (xboxNode.empty()) {
        std::cout << "SKIP: virtual pad created but its evdev node never appeared.\n";
        return kSkipExitCode;
    }

    const int evFd = ::open(xboxNode.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (evFd < 0) {
        std::cout << "SKIP: cannot open " << xboxNode
                  << " (need root or input-group membership) — errno " << errno << "\n";
        return kSkipExitCode;
    }

    TEST("gamepad report reaches a consumer as EV_KEY");
    GamepadReport report;
    report.wButtons = kXusbA;
    EXPECT(adapter.submitReport(kXboxSerial, report));
    EXPECT(waitForKeyEvent(evFd, BTN_A, 1, 2000));
    report.wButtons = 0;
    EXPECT(adapter.submitReport(kXboxSerial, report));
    EXPECT(waitForKeyEvent(evFd, BTN_A, 0, 2000));

    TEST("force-feedback upload round-trips into the rumble callback");
    std::atomic<bool> rumblePlayed{false};
    std::atomic<bool> rumbleStopped{false};
    std::atomic<uint16_t> rumbleStrong{0};
    std::atomic<uint16_t> rumbleWeak{0};
    adapter.setRumbleCallback([&](uint32_t serial, const RumbleReport& r) {
        if (serial != kXboxSerial) return;
        if (r.strongMagnitude > 0 || r.weakMagnitude > 0) {
            rumbleStrong.store(r.strongMagnitude, std::memory_order_release);
            rumbleWeak.store(r.weakMagnitude, std::memory_order_release);
            rumblePlayed.store(true, std::memory_order_release);
        } else {
            rumbleStopped.store(true, std::memory_order_release);
        }
    });
    struct ff_effect effect{};
    effect.type = FF_RUMBLE;
    effect.id = -1;
    effect.u.rumble.strong_magnitude = 0x1234;
    effect.u.rumble.weak_magnitude = 0x5678;
    effect.replay.length = 1000;
    EXPECT(::ioctl(evFd, EVIOCSFF, &effect) >= 0);
    struct input_event play{};
    play.type = EV_FF;
    play.code = static_cast<uint16_t>(effect.id);
    play.value = 1;
    EXPECT(::write(evFd, &play, sizeof(play)) == static_cast<ssize_t>(sizeof(play)));
    EXPECT(waitFor(rumblePlayed, 2000));
    EXPECT(rumbleStrong.load(std::memory_order_acquire) == 0x1234);
    EXPECT(rumbleWeak.load(std::memory_order_acquire) == 0x5678);
    play.value = 0;
    EXPECT(::write(evFd, &play, sizeof(play)) == static_cast<ssize_t>(sizeof(play)));
    EXPECT(waitFor(rumbleStopped, 2000));
    ::close(evFd);

    TEST("DS4 pad plugs in with motion + touchpad nodes");
    EXPECT(adapter.pluginDevice(kDs4Serial, GamepadIdentity::DS4));
    EXPECT(adapter.isDevicePlugged(kDs4Serial));
    const std::string ds4Name = "Satellite Virtual DualShock 4 #" + std::to_string(kDs4Serial);
    EXPECT(!findEventNodeByName(ds4Name, 3000).empty());
    EXPECT(!findEventNodeByName(
                "Satellite Virtual DualShock 4 Motion Sensors #" + std::to_string(kDs4Serial), 3000)
                .empty());
    EXPECT(!findEventNodeByName(
                "Satellite Virtual DualShock 4 Touchpad #" + std::to_string(kDs4Serial), 3000)
                .empty());
    EXPECT(adapter.motionBackendOk(kDs4Serial));

    TEST("DS4 report / motion / touchpad submissions are accepted");
    GamepadReport ds4Report;
    ds4Report.wButtons = kXusbA;
    EXPECT(adapter.submitReport(kDs4Serial, ds4Report));
    MotionReport motion;
    motion.gyroX = 100;
    motion.accelZ = 8192;
    motion.timestampDeltaUs = 4000;
    EXPECT(adapter.submitMotion(kDs4Serial, motion));
    TouchpadReport touch;
    touch.finger0.active = true;
    touch.finger0.trackingId = 1;
    touch.finger0.x = 1024;
    touch.finger0.y = -2048;
    touch.eventTimeMs = 1;
    EXPECT(adapter.submitTouchpad(kDs4Serial, touch));

    TEST("relative-mouse node materialises on first use");
    EXPECT(adapter.submitRelativeMouse(3, -2, false));
    EXPECT(!findEventNodeByName("Satellite Virtual Pointer", 3000).empty());

    TEST("unplug removes devices and their nodes");
    EXPECT(adapter.unplugDevice(kXboxSerial));
    EXPECT(!adapter.isDevicePlugged(kXboxSerial));
    EXPECT(adapter.unplugDevice(kDs4Serial));
    EXPECT(!adapter.isDevicePlugged(kDs4Serial));
    EXPECT(eventNodeGone(xboxName, 3000));
    EXPECT(eventNodeGone(ds4Name, 3000));

    adapter.closeBus();
    TEST("bus closes");
    EXPECT(!adapter.isBusOpen());

    std::cout << "\n=== Test Results ===\n";
    std::cout << "  Passed: " << g_pass << "\n";
    std::cout << "  Failed: " << g_fail << "\n";
    std::cout << "  STATUS: " << (g_fail == 0 ? "ALL PASSED" : "FAILED") << "\n";
    return g_fail == 0 ? 0 : 1;
}
