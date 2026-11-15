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
// What a Satellite's exit takes besides its helper's shutdown and the helper's
// own exit: its servers and threads stopping, and its sessions' unplugs, one of
// which can wait out an unplug's 10 s before the channel goes.
inline constexpr DWORD EXIT_BESIDES_HELPER_MS = 30000;

struct SingleInstanceNames {
    const char* mutex;
    const char* windowClass;
    const char* windowTitle;
    // Held by the one launch that waits for a Satellite on its way out.
    const char* waitingMutex;
};

// What a window did with a tap or a probe.
enum class TapAnswer {
    Answered, // its message loop runs: a live Satellite
    NoAnswer, // not within the wait: on its way out, hung, or not looping yet
    Denied,   // UIPI kept the message out: a Satellite above this launch's integrity
};
using Tap = TapAnswer (*)(HWND window, UINT message, DWORD waitMs);

// SendMessageTimeout, read as a TapAnswer.
TapAnswer tapWindow(HWND window, UINT message, DWORD waitMs);

struct SingleInstanceWaits {
    DWORD answerMs;
    DWORD sliceMs;
    DWORD exitMs;
};

// True when this process is the session's Satellite: it then owns the mutex,
// in `held`, on the calling thread until it exits, which is what a later launch
// waits for. A launch that finds another Satellite ends (false) unless that one
// is on its way out. It taps the tray window once: a Satellite that answers, or
// that UIPI keeps the tap from (one running elevated without UIAccess), has the
// click. One that does not answer is probed with WM_NULL on each wait, so one
// whose message loop has not started yet, as at login, ends the launch as soon
// as it answers. Only one that never answers again is on its way out, or hung:
// this launch waits for it to exit and starts once it has, and past `exitMs` it
// ends. One launch at a time does this (`waitingMutex`); a later one ends at once.
bool acquireSingleInstance(const SingleInstanceNames& names, const SingleInstanceWaits& waits,
                           HANDLE& held, Tap tap = tapWindow);

} // namespace lifecycle
