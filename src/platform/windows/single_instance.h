// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <winsock2.h>
#include <windows.h>

namespace lifecycle {

// What a launch taps another Satellite's tray window with. A live one answers
// at once and opens the dashboard after.
inline constexpr UINT WM_SECOND_INSTANCE = WM_USER + 100;

// How long a tap waits for the window's thread to answer.
inline constexpr DWORD SECOND_INSTANCE_ANSWER_MS = 2000;
// How long each wait for the other Satellite's exit lasts before its window is
// looked for again.
inline constexpr DWORD PREVIOUS_INSTANCE_SLICE_MS = 1000;
// How long, all told, a launch waits for a Satellite on its way out: as long as
// the SDK lets one program wait for another's removals (DeviceManager.cs
// PnpLockWaitMs, ten minutes). app_lifecycle.cpp checks that the old one's exit,
// its helper's shutdown and the helper's own exit, fits inside it.
inline constexpr DWORD PREVIOUS_INSTANCE_EXIT_WAIT_MS = 600000;

struct SingleInstanceNames {
    const char* mutex;
    const char* windowClass;
    const char* windowTitle;
};

struct SingleInstanceWaits {
    DWORD answerMs;
    DWORD sliceMs;
    DWORD exitMs;
};

// True when this process is the session's Satellite: it then owns the mutex,
// in `held`, on the calling thread until it exits, which is what a later launch
// waits for. A launch that finds another Satellite taps its tray window: one
// that answers takes the click, and this launch ends (false). One that does not
// is on its way out, still waiting for its helper to finish removing its
// controllers, or hung, or not yet showing its window: this launch waits for it
// to exit and starts once it has, or taps the window it shows, and past
// `exitMs` it ends. Each window is tapped once, so one that answers late opens
// one dashboard, not one per wait.
bool acquireSingleInstance(const SingleInstanceNames& names, const SingleInstanceWaits& waits,
                           HANDLE& held);

} // namespace lifecycle
