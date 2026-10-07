// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include <windows.h>

#include <string>

namespace satellite::updater {

inline std::string installerLaunchError(DWORD lastError) {
    if (lastError == ERROR_CANCELLED) {
        return "The installer needs administrator permission, and the request was declined.";
    }
    return "ShellExecuteEx failed (GLE=" + std::to_string(lastError) + ")";
}

} // namespace satellite::updater
