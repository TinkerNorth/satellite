// SPDX-License-Identifier: LGPL-3.0-or-later
#include "single_instance.h"

#include <algorithm>

namespace lifecycle {

namespace {

// Taps the other Satellite's window, then waits for it to exit, tapping once
// any window that shows meanwhile. True once this launch owns the singleton.
// A window seen for the first time gets the tap; after that only a probe
// (WM_NULL, which does nothing), so a Satellite whose loop starts late, as at
// login, ends the launch once that loop answers, and the tap it took then opens
// its one dashboard. Only a window that never answers again is on its way out.
bool awaitPrevious(const SingleInstanceNames& names, const SingleInstanceWaits& waits, HANDLE held,
                   Tap tap) {
    const ULONGLONG deadline = GetTickCount64() + waits.exitMs;
    HWND tapped = nullptr;
    for (;;) {
        HWND window = FindWindowExA(nullptr, nullptr, names.windowClass, names.windowTitle);
        if (window != nullptr) {
            const UINT message = window == tapped ? WM_NULL : WM_SECOND_INSTANCE;
            tapped = window;
            TapAnswer answer = tap(window, message, waits.answerMs);
            if (answer == TapAnswer::Denied) answer = tap(window, WM_NULL, waits.answerMs);
            if (answer != TapAnswer::NoAnswer) return false;
        }
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) return false;
        const DWORD slice = static_cast<DWORD>(std::min<ULONGLONG>(waits.sliceMs, deadline - now));
        // The other Satellite's exit lets the mutex go (abandoned when it
        // exits holding it), and this launch owns it from then on.
        const DWORD waited = WaitForSingleObject(held, slice);
        if (waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED) return true;
        if (waited != WAIT_TIMEOUT) return false;
    }
}

} // namespace

TapAnswer tapWindow(HWND window, UINT message, DWORD waitMs) {
    DWORD_PTR result = 0;
    SetLastError(ERROR_SUCCESS);
    if (SendMessageTimeoutA(window, message, 0, 0, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, waitMs,
                            &result) != 0) {
        return TapAnswer::Answered;
    }
    return GetLastError() == ERROR_ACCESS_DENIED ? TapAnswer::Denied : TapAnswer::NoAnswer;
}

bool acquireSingleInstance(const SingleInstanceNames& names, const SingleInstanceWaits& waits,
                           HANDLE& held, Tap tap) {
    held = CreateMutexA(nullptr, TRUE, names.mutex);
    if (held == nullptr) return true; // best-effort; never block startup on it
    if (GetLastError() != ERROR_ALREADY_EXISTS) return true;

    // One launch at a time taps and waits; a later one ends at once, so extra
    // clicks never pile up hidden waiters.
    HANDLE turn = CreateMutexA(nullptr, TRUE, names.waitingMutex);
    const bool mine = turn != nullptr && GetLastError() != ERROR_ALREADY_EXISTS;
    const bool started = mine && awaitPrevious(names, waits, held, tap);
    if (turn != nullptr) {
        if (mine) ReleaseMutex(turn);
        CloseHandle(turn);
    }
    if (started) return true;
    CloseHandle(held);
    held = nullptr;
    return false;
}

} // namespace lifecycle
