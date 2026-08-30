// SPDX-License-Identifier: LGPL-3.0-or-later

// Host-global relative pointer injection shared by the Windows gamepad
// adapters. Independent of any driver bus, so it works with no controllers
// plugged; the caller owns the button-state atomic so repeated frames only
// inject edges.
#pragma once

#include <winsock2.h>
#include <windows.h>

#include <atomic>

inline bool injectRelativeMouse(std::atomic<bool>& btnDown, int dx, int dy, bool leftButton) {
    INPUT inputs[2] = {};
    int n = 0;
    if (dx != 0 || dy != 0) {
        inputs[n].type = INPUT_MOUSE;
        inputs[n].mi.dx = dx;
        inputs[n].mi.dy = dy;
        inputs[n].mi.dwFlags = MOUSEEVENTF_MOVE;
        ++n;
    }
    const bool was = btnDown.exchange(leftButton);
    if (leftButton != was) {
        inputs[n].type = INPUT_MOUSE;
        inputs[n].mi.dwFlags = leftButton ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        ++n;
    }
    if (n == 0) return true; // idle frame: nothing to inject, still "handled"
    return SendInput(static_cast<UINT>(n), inputs, sizeof(INPUT)) == static_cast<UINT>(n);
}
