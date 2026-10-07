// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include "core/update_types.h"

namespace satellite::tray {

struct UpdateToastState {
    bool shown = false;
    bool rearmAtNextCheck = false;
};

inline bool updateToastDue(UpdateToastState& state, const UpdateStatusSnapshot& snap) {
    const bool settled = snap.state == UpdateState::Idle || snap.state == UpdateState::UpToDate;
    const bool checkRearms = snap.state == UpdateState::Checking && state.rearmAtNextCheck;
    if (settled || checkRearms) {
        state.shown = false;
        state.rearmAtNextCheck = false;
        return false;
    }
    if (snap.dismissed) {
        state.shown = true;
        state.rearmAtNextCheck = true;
        return false;
    }
    const bool offered = snap.state == UpdateState::UpdateAvailable && snap.info.available;
    const bool due = offered && !state.shown;
    if (due) state.shown = true;
    return due;
}

} // namespace satellite::tray
