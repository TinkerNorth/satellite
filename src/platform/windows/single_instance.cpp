// SPDX-License-Identifier: LGPL-3.0-or-later
#include "single_instance.h"

#include <algorithm>

namespace lifecycle {

namespace {

// Whether the window's thread took a tap within `answerMs`: a Satellite whose
// message loop still runs. One on its way out has left its loop, and a hung one
// does not answer either.
bool answers(HWND window, DWORD answerMs) {
    DWORD_PTR result = 0;
    return SendMessageTimeoutA(window, WM_SECOND_INSTANCE, 0, 0,
                               SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, answerMs, &result) != 0;
}

} // namespace

bool acquireSingleInstance(const SingleInstanceNames& names, const SingleInstanceWaits& waits,
                           HANDLE& held) {
    held = CreateMutexA(nullptr, TRUE, names.mutex);
    if (held == nullptr) return true; // best-effort; never block startup on it
    if (GetLastError() != ERROR_ALREADY_EXISTS) return true;

    const ULONGLONG deadline = GetTickCount64() + waits.exitMs;
    HWND tapped = nullptr;
    for (;;) {
        HWND window = FindWindowExA(nullptr, nullptr, names.windowClass, names.windowTitle);
        if (window != nullptr && window != tapped) {
            tapped = window;
            if (answers(window, waits.answerMs)) break;
        }
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) break;
        const DWORD slice = static_cast<DWORD>(std::min<ULONGLONG>(waits.sliceMs, deadline - now));
        // The other Satellite's exit lets the mutex go (abandoned when it
        // exits holding it), and this launch owns it from then on.
        const DWORD waited = WaitForSingleObject(held, slice);
        if (waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED) return true;
        if (waited != WAIT_TIMEOUT) break;
    }
    CloseHandle(held);
    held = nullptr;
    return false;
}

} // namespace lifecycle
