// SPDX-License-Identifier: LGPL-3.0-or-later

// Host-global relative pointer injection shared by the Windows gamepad
// adapters. Independent of any driver bus, so it works with no controllers
// plugged; the caller owns the button-state atomics so repeated frames only
// inject edges.
#pragma once

#include <winsock2.h>
#include <windows.h>

#include <atomic>

#include "core/types.h"

struct RelMouseButtonState {
    std::atomic<bool> left{false};
    std::atomic<bool> right{false};
    std::atomic<bool> middle{false};
};

inline bool injectRelativeMouse(RelMouseButtonState& state, int dx, int dy,
                                const MouseButtons& buttons, int wheelV) {
    INPUT inputs[5] = {};
    int n = 0;
    if (dx != 0 || dy != 0) {
        inputs[n].type = INPUT_MOUSE;
        inputs[n].mi.dx = dx;
        inputs[n].mi.dy = dy;
        inputs[n].mi.dwFlags = MOUSEEVENTF_MOVE;
        ++n;
    }
    if (buttons.left != state.left.exchange(buttons.left)) {
        inputs[n].type = INPUT_MOUSE;
        inputs[n].mi.dwFlags = buttons.left ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        ++n;
    }
    if (buttons.right != state.right.exchange(buttons.right)) {
        inputs[n].type = INPUT_MOUSE;
        inputs[n].mi.dwFlags = buttons.right ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        ++n;
    }
    if (buttons.middle != state.middle.exchange(buttons.middle)) {
        inputs[n].type = INPUT_MOUSE;
        inputs[n].mi.dwFlags = buttons.middle ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
        ++n;
    }
    if (wheelV != 0) {
        inputs[n].type = INPUT_MOUSE;
        inputs[n].mi.mouseData = static_cast<DWORD>(wheelV);
        inputs[n].mi.dwFlags = MOUSEEVENTF_WHEEL;
        ++n;
    }
    if (n == 0) return true; // idle frame: nothing to inject, still "handled"
    return SendInput(static_cast<UINT>(n), inputs, sizeof(INPUT)) == static_cast<UINT>(n);
}
