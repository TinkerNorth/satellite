// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "fake_vigem_driver.h"

#include "vigem_adapter.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;
using Nanos = std::chrono::nanoseconds;

constexpr int64_t NANOS_PER_MICRO = 1'000;
constexpr int64_t NANOS_PER_MILLI = 1'000'000;

std::atomic<int64_t> g_submitNanos{0};
std::atomic<int64_t> g_plugNanos{0};
std::atomic<int64_t> g_unplugNanos{0};
std::atomic<bool> g_armed{false};
std::atomic<uint64_t> g_submitCalls{0};

void spinFor(Nanos span) {
    const Clock::time_point due = Clock::now() + span;
    while (Clock::now() < due) YieldProcessor();
}

void sleepIfArmed(const std::atomic<int64_t>& nanos) {
    if (!g_armed.load(std::memory_order_acquire)) return;
    std::this_thread::sleep_for(Nanos(nanos.load(std::memory_order_relaxed)));
}

bool spinSubmit() {
    g_submitCalls.fetch_add(1, std::memory_order_relaxed);
    spinFor(Nanos(g_submitNanos.load(std::memory_order_relaxed)));
    return true;
}

} // namespace

namespace fake_vigem {

void configure(const Tuning& tuning) {
    g_armed.store(false, std::memory_order_release);
    g_submitNanos.store(tuning.submitUs * NANOS_PER_MICRO, std::memory_order_relaxed);
    g_plugNanos.store(tuning.plugMs * NANOS_PER_MILLI, std::memory_order_relaxed);
    g_unplugNanos.store(tuning.unplugMs * NANOS_PER_MILLI, std::memory_order_relaxed);
    g_submitCalls.store(0, std::memory_order_relaxed);
}

void arm() {
    g_submitCalls.store(0, std::memory_order_relaxed);
    g_armed.store(true, std::memory_order_release);
}

uint64_t submitCalls() { return g_submitCalls.load(std::memory_order_relaxed); }

} // namespace fake_vigem

HANDLE openVigemBus() { return CreateEventW(nullptr, FALSE, FALSE, nullptr); }

bool pluginTarget(HANDLE, ULONG) {
    sleepIfArmed(g_plugNanos);
    return true;
}

bool pluginTargetDS4(HANDLE, ULONG) {
    sleepIfArmed(g_plugNanos);
    return true;
}

bool unplugTarget(HANDLE, ULONG) {
    sleepIfArmed(g_unplugNanos);
    return true;
}

bool submitXusbSync(HANDLE, ULONG, XUSB_SUBMIT_REPORT&, HANDLE, const void*) {
    return spinSubmit();
}

bool submitDs4Sync(HANDLE, ULONG, DS4_SUBMIT_REPORT&, HANDLE, const DS4_REPORT&) {
    return spinSubmit();
}

bool submitDs4ExSync(HANDLE, ULONG, DS4_SUBMIT_REPORT_EX&, HANDLE, const DS4_REPORT_EX&) {
    return spinSubmit();
}

bool waitNextXusbNotification(HANDLE, ULONG, HANDLE cancel, XUSB_REQUEST_NOTIFICATION&) {
    WaitForSingleObject(cancel, INFINITE);
    return false;
}

bool waitNextDS4Notification(HANDLE, ULONG, HANDLE cancel, DS4_REQUEST_NOTIFICATION&) {
    WaitForSingleObject(cancel, INFINITE);
    return false;
}
