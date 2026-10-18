// SPDX-License-Identifier: LGPL-3.0-or-later

// Host-global relative pointer injection shared by the Windows gamepad
// adapters. Independent of any driver bus, so it works with no controllers
// plugged; the caller owns the button-state atomics so repeated frames only
// inject edges.
#pragma once

#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <functional>
#include <string>

#include "core/types.h"

struct RelMouseButtonState {
    std::atomic<bool> left{false};
    std::atomic<bool> right{false};
    std::atomic<bool> middle{false};
    std::atomic<bool> refusedRun{false};
};

inline std::function<void(const std::string&)> g_pointerInjectDiagnostic;

inline std::string pointerRefusalMessage(unsigned requested, unsigned injected, unsigned long error,
                                         const std::string& foregroundOwner) {
    std::string msg = "SendInput injected " + std::to_string(injected) + " of " +
                      std::to_string(requested) + " pointer events (Win32 error " +
                      std::to_string(error) + ")";
    if (error == ERROR_ACCESS_DENIED) {
        msg += "; Windows discards injected input while the foreground window belongs to a "
               "process with higher privileges than Satellite";
    }
    if (!foregroundOwner.empty()) msg += "; foreground window: " + foregroundOwner;
    return msg;
}

inline std::string pointerInjectNarrow(const wchar_t* text, int length) {
    if (length <= 0) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, length, out.data(), n, nullptr, nullptr);
    return out;
}

inline std::string describeForegroundWindowOwner() {
    const HWND hwnd = GetForegroundWindow();
    if (hwnd == nullptr) return "none";
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    std::string owner = "pid " + std::to_string(pid);
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process != nullptr) {
        wchar_t path[MAX_PATH] = {};
        DWORD length = MAX_PATH;
        if (QueryFullProcessImageNameW(process, 0, path, &length) && length > 0) {
            const std::wstring full(path, length);
            const size_t slash = full.find_last_of(L"\\/");
            const std::wstring name = slash == std::wstring::npos ? full : full.substr(slash + 1);
            owner = pointerInjectNarrow(name.c_str(), static_cast<int>(name.size())) + " (" +
                    owner + ")";
        }
        CloseHandle(process);
    } else {
        owner += ", not queryable from Satellite's unelevated process";
    }
    wchar_t title[128] = {};
    const int titleLength = GetWindowTextW(hwnd, title, 128);
    if (titleLength > 0) owner += " titled '" + pointerInjectNarrow(title, titleLength) + "'";
    return owner;
}

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
    const UINT injected = SendInput(static_cast<UINT>(n), inputs, sizeof(INPUT));
    if (injected == static_cast<UINT>(n)) {
        state.refusedRun.store(false, std::memory_order_relaxed);
        return true;
    }
    const DWORD error = GetLastError();
    if (!state.refusedRun.exchange(true, std::memory_order_relaxed) && g_pointerInjectDiagnostic) {
        g_pointerInjectDiagnostic(pointerRefusalMessage(static_cast<unsigned>(n), injected, error,
                                                        describeForegroundWindowOwner()));
    }
    return false;
}
