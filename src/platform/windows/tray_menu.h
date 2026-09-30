// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include "core/update_types.h"
#include "globals.h"

#include <string>

namespace satellite::tray {

struct UpdateMenuItem {
    UINT id = 0;
    UINT flags = 0;
    std::wstring label;
};

// The one update entry the tray menu shows, from the updater's state, so the
// label reflects what a click would do: a downloaded release installs, an
// available one downloads, a check or download in flight is shown greyed, and
// anything else offers a check. Without an updater wired the check is greyed
// too.
inline UpdateMenuItem updateMenuItemFor(bool haveUpdater, UpdateState state, bool available,
                                        const std::wstring& version) {
    if (!haveUpdater) return {IDM_CHECK_UPDATES, MF_STRING | MF_GRAYED, L"Check for Updates..."};
    if (state == UpdateState::Downloaded && available) {
        return {IDM_INSTALL_UPDATE, MF_STRING, L"Install Update " + version};
    }
    if (state == UpdateState::UpdateAvailable && available) {
        return {IDM_INSTALL_UPDATE, MF_STRING, L"Download Update " + version + L"..."};
    }
    if (state == UpdateState::Downloading || state == UpdateState::Verifying) {
        return {IDM_CHECK_UPDATES, MF_STRING | MF_GRAYED, L"Downloading update..."};
    }
    if (state == UpdateState::Checking) {
        return {IDM_CHECK_UPDATES, MF_STRING | MF_GRAYED, L"Checking for updates..."};
    }
    return {IDM_CHECK_UPDATES, MF_STRING, L"Check for Updates..."};
}

} // namespace satellite::tray
