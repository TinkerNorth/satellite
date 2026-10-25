// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include "types.h"

#include <algorithm>
#include <array>
#include <cstddef>

namespace satellite {

enum class PointerEventKind { Move, LeftButton, RightButton, MiddleButton, Wheel };

struct PointerEvent {
    PointerEventKind kind = PointerEventKind::Move;
    int x = 0;
    int y = 0;
    bool pressed = false;
};

struct PointerFrame {
    std::array<PointerEvent, 5> events{};
    size_t count = 0;
    MouseButtons before{};
};

struct PointerButtonMapping {
    bool MouseButtons::* level;
    PointerEventKind kind;
};

inline constexpr std::array<PointerButtonMapping, 3> POINTER_BUTTONS{{
    {&MouseButtons::left, PointerEventKind::LeftButton},
    {&MouseButtons::right, PointerEventKind::RightButton},
    {&MouseButtons::middle, PointerEventKind::MiddleButton},
}};

inline PointerFrame planPointerFrame(const MouseButtons& before, const MouseButtons& desired,
                                     const int dx, const int dy, const int wheel) {
    PointerFrame frame;
    frame.before = before;
    if (dx != 0 || dy != 0) {
        frame.events[frame.count++] = {PointerEventKind::Move, dx, dy, false};
    }
    for (const auto& button : POINTER_BUTTONS) {
        const bool pressed = desired.*button.level;
        if (pressed != before.*button.level) {
            frame.events[frame.count++] = {button.kind, 0, 0, pressed};
        }
    }
    if (wheel != 0) frame.events[frame.count++] = {PointerEventKind::Wheel, 0, wheel, false};
    return frame;
}

inline MouseButtons pointerButtonsAfter(const PointerFrame& frame, const size_t accepted) {
    MouseButtons buttons = frame.before;
    const size_t count = std::min(accepted, frame.count);
    for (size_t i = 0; i < count; i++) {
        const PointerEvent& event = frame.events[i];
        for (const auto& button : POINTER_BUTTONS) {
            if (event.kind == button.kind) buttons.*button.level = event.pressed;
        }
    }
    return buttons;
}

} // namespace satellite
