// SPDX-License-Identifier: LGPL-3.0-or-later

#include "pointer_worker.h"

#include "core/tracked_mutex.h"

namespace {

constexpr int NAME_READ_ATTEMPTS = 64;

} // namespace

PointerMotion touchpadPointerMotion(const TouchpadReport& prev, bool prevValid,
                                    const TouchpadReport& report, float& remX, float& remY) {
    PointerMotion motion;
    const bool continuous = prevValid && prev.finger0.active && report.finger0.active &&
                            prev.finger0.trackingId == report.finger0.trackingId;
    if (continuous) {
        const int32_t dt_ms = static_cast<int32_t>(report.eventTimeMs - prev.eventTimeMs);
        if (dt_ms > TOUCHPAD_MOUSE_MAX_GAP_MS) {
            remX = 0.0f;
            remY = 0.0f;
        } else if (dt_ms > 0) {
            const int32_t dt_clamped =
                dt_ms < TOUCHPAD_MOUSE_MIN_DT_MS ? TOUCHPAD_MOUSE_MIN_DT_MS : dt_ms;
            const float scale =
                static_cast<float>(TOUCHPAD_MOUSE_REFERENCE_MS) / static_cast<float>(dt_clamped);
            const float fx = static_cast<float>(report.finger0.x - prev.finger0.x) *
                                 TOUCHPAD_MOUSE_SENSITIVITY * scale +
                             remX;
            const float fy = static_cast<float>(report.finger0.y - prev.finger0.y) *
                                 TOUCHPAD_MOUSE_SENSITIVITY * scale +
                             remY;
            motion.dx = static_cast<int>(fx);
            motion.dy = static_cast<int>(fy);
            remX = fx - static_cast<float>(motion.dx);
            remY = fy - static_cast<float>(motion.dy);
        }
    } else {
        remX = 0.0f;
        remY = 0.0f;
    }
    motion.buttons.left = report.buttonPressed;
    motion.buttons.right = report.rightPressed;
    motion.buttons.middle = report.middlePressed;
    motion.wheel = report.scrollV;
    if (prevValid && static_cast<int32_t>(report.eventTimeMs - prev.eventTimeMs) <= 0) {
        motion.wheel = 0;
    }
    motion.buttonsChanged =
        prevValid ? (prev.buttonPressed != report.buttonPressed ||
                     prev.rightPressed != report.rightPressed ||
                     prev.middlePressed != report.middlePressed)
                  : (report.buttonPressed || report.rightPressed || report.middlePressed);
    return motion;
}

PointerWorker::PointerWorker(IGamepadPort& backend, ILogPort& log, const HotTable& hot)
    : backend_(backend), log_(log), hot_(hot) {
    thread_ = std::thread([this] { run(); });
}

PointerWorker::~PointerWorker() {
    stopping_.store(true, std::memory_order_seq_cst);
    wake_.notify();
    if (thread_.joinable()) thread_.join();
}

bool PointerWorker::pushFrame(const PointerFrame& frame) {
    if (!frames_.push(frame)) return false;
    wake_.notify();
    return true;
}

void PointerWorker::post(const PointerCommand& command) {
    while (!commands_.push(command)) std::this_thread::yield();
    wake_.notify();
}

bool PointerWorker::retryPending() const { return retryPending_.load(std::memory_order_acquire); }

PointerCounts PointerWorker::counts() const {
    PointerCounts c;
    c.injected = injected_.load(std::memory_order_relaxed);
    c.refused = refused_.load(std::memory_order_relaxed);
    return c;
}

void PointerWorker::run() {
    ScopedThreadRole role(ThreadRole::Pointer);
    for (;;) {
        bool worked = false;
        PointerFrame frame;
        while (frames_.pop(frame)) {
            drainCommands();
            process(frame);
            framesDone_.fetch_add(1, std::memory_order_release);
            worked = true;
        }
        if (drainCommands()) worked = true;
        if (worked) continue;
        if (stopping_.load(std::memory_order_seq_cst)) return;
        wake_.sleepUnless([this] { return hasWork(); });
    }
}

bool PointerWorker::hasWork() const {
    return !frames_.empty() || commands_.depth() != 0 || stopping_.load(std::memory_order_seq_cst);
}

bool PointerWorker::drainCommands() {
    bool any = false;
    PointerCommand command;
    while (commands_.pop(command)) {
        apply(command);
        commandsDone_.fetch_add(1, std::memory_order_release);
        any = true;
    }
    return any;
}

void PointerWorker::process(const PointerFrame& frame) {
    Pad& pad = pads_[frame.serial];
    if (frame.stamp < pad.releaseStamp) {
        lastFrameOk_.store(false, std::memory_order_release);
        return;
    }
    if (pad.stamp != frame.stamp) {
        pad.stamp = frame.stamp;
        pad.prevValid = false;
        pad.remX = 0.0f;
        pad.remY = 0.0f;
    }
    const TouchpadReport prev = pad.prev;
    const bool prevValid = pad.prevValid;
    pad.prev = frame.report;
    pad.prevValid = true;
    const PointerMotion motion =
        touchpadPointerMotion(prev, prevValid, frame.report, pad.remX, pad.remY);
    pad.buttons = motion.buttons;
    const bool ok = submit(motion.dx, motion.dy, motion.wheel);
    retryPending_.store(!ok, std::memory_order_release);
    if (motion.dx != 0 || motion.dy != 0 || motion.wheel != 0 || motion.buttonsChanged) {
        record(frame.slot, frame.row, ok);
    }
    lastFrameOk_.store(ok, std::memory_order_release);
}

void PointerWorker::apply(const PointerCommand& command) {
    switch (command.kind) {
    case PointerCommand::Kind::Release: {
        Pad& pad = pads_[command.serial];
        if (command.stamp > pad.releaseStamp) pad.releaseStamp = command.stamp;
        const bool anyHeld = pad.buttons.left || pad.buttons.right || pad.buttons.middle;
        pad.buttons = MouseButtons{};
        if (!anyHeld) return;
        retryPending_.store(!submit(0, 0, 0), std::memory_order_release);
        return;
    }
    case PointerCommand::Kind::Retry:
        if (!retryPending_.load(std::memory_order_acquire)) return;
        retryPending_.store(!submit(0, 0, 0), std::memory_order_release);
        return;
    case PointerCommand::Kind::BackdateRefusalLog:
        session(command.slot, command.row).refusalLoggedAt =
            std::chrono::steady_clock::now() - std::chrono::seconds(command.seconds);
        return;
    }
}

bool PointerWorker::submit(int dx, int dy, int wheel) {
    MouseButtons held;
    for (const Pad& pad : pads_) {
        held.left = held.left || pad.buttons.left;
        held.right = held.right || pad.buttons.right;
        held.middle = held.middle || pad.buttons.middle;
    }
    return backend_.submitRelativeMouse(dx, dy, held, wheel);
}

PointerWorker::Session& PointerWorker::session(int slot, uint32_t row) {
    Session& s = sessions_[static_cast<size_t>(slot)];
    if (s.row != row) {
        s = Session{};
        s.row = row;
    }
    return s;
}

bool PointerWorker::deviceName(int slot, uint32_t row, std::string& out) const {
    const HotSlot& hot = hot_.slots[static_cast<size_t>(slot)];
    for (int attempt = 0; attempt < NAME_READ_ATTEMPTS; attempt++) {
        uint32_t seenRow = 0;
        const bool stable = hot.read([&] {
            seenRow = hot.row.load(std::memory_order_relaxed);
            out = hot.loadName();
        });
        if (stable) return seenRow == row;
        std::this_thread::yield();
    }
    return false;
}

void PointerWorker::record(int slot, uint32_t row, bool ok) {
    Session& s = session(slot, row);
    std::string name;
    if (ok) {
        injected_.fetch_add(1, std::memory_order_relaxed);
        if (s.refusalLogged && deviceName(slot, row, name)) {
            log_.logMsg(LogLevel::INFO, "service",
                        "Host is accepting pointer input for " + name + " again after " +
                            std::to_string(s.refusedRun) +
                            (s.refusedRun == 1 ? " refused frame" : " refused frames"));
        }
        s.refusedRun = 0;
        s.refusalLogged = false;
        return;
    }
    refused_.fetch_add(1, std::memory_order_relaxed);
    s.refusedRun++;
    if (s.refusedRun != 1) return;
    const auto now = std::chrono::steady_clock::now();
    const bool loggedBefore = s.refusalLoggedAt.time_since_epoch().count() != 0;
    if (loggedBefore &&
        now - s.refusalLoggedAt < std::chrono::seconds(POINTER_REFUSAL_LOG_INTERVAL_SEC)) {
        return;
    }
    if (!deviceName(slot, row, name)) return;
    s.refusalLoggedAt = now;
    s.refusalLogged = true;
    log_.logMsg(LogLevel::WARN, "service",
                "Host refused to inject pointer input for " + name +
                    "; the frames arrive but the OS drops them (on Windows this happens while "
                    "the foreground window belongs to a process with higher privileges than "
                    "Satellite)");
}

#ifdef SATELLITE_BUILD_TESTS
bool PointerWorker::drainForTest() {
    constexpr auto DRAIN_LIMIT = std::chrono::seconds(10);
    const uint64_t frames = frames_.pushed();
    const uint64_t commands = commands_.enqueued();
    wake_.notify();
    const auto deadline = std::chrono::steady_clock::now() + DRAIN_LIMIT;
    while (framesDone_.load(std::memory_order_acquire) < frames ||
           commandsDone_.load(std::memory_order_acquire) < commands) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::yield();
    }
    const uint64_t done = framesDone_.load(std::memory_order_acquire);
    const bool processed = done != framesAtLastDrain_;
    framesAtLastDrain_ = done;
    return processed && lastFrameOk_.load(std::memory_order_acquire);
}

std::thread::id PointerWorker::threadIdForTest() const { return thread_.get_id(); }
#endif
