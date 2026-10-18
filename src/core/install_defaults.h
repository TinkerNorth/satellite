// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include "core/types.h"

#include <string>

namespace satellite {

struct InstallDefaults {
    std::string stamp;
    bool autoCheck = true;
    bool crashReporting = true;
};

inline bool applyInstallDefaults(Config& cfg, const InstallDefaults& d) {
    if (d.stamp.empty() || d.stamp == cfg.installDefaultsStamp) return false;
    cfg.autoCheck = d.autoCheck;
    cfg.crashReporting = d.crashReporting;
    cfg.installDefaultsStamp = d.stamp;
    return true;
}

} // namespace satellite
