// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include "core/config_store.h"
#include "core/tracked_mutex.h"
#include "core/types.h"

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace satellite {

struct ConfigEffectSinks {
    std::function<void(bool)> autoStart;
    std::function<void(bool)> crashReporting;
};

struct AppliedConfigEffects {
    std::optional<bool> autoStart;
    std::optional<bool> crashReporting;
};

inline void applyConfigEffects(const Config& snapshot, AppliedConfigEffects& applied,
                               const ConfigEffectSinks& sinks) {
    if (applied.autoStart != snapshot.autoStart) {
        blockingRegion("autostart.write");
        sinks.autoStart(snapshot.autoStart);
        applied.autoStart = snapshot.autoStart;
    }
    if (applied.crashReporting != snapshot.crashReporting) {
        blockingRegion("crash.toggle");
        sinks.crashReporting(snapshot.crashReporting);
        applied.crashReporting = snapshot.crashReporting;
    }
}

class ConfigEffects {
  public:
    explicit ConfigEffects(ConfigEffectSinks sinks) : sinks_(std::move(sinks)) {}

    void apply(const ConfigStore& store) {
        std::lock_guard<TrackedMutex> lk(mtx_);
        const std::shared_ptr<const Config> snapshot = store.current();
        applyConfigEffects(*snapshot, applied_, sinks_);
    }

  private:
    const ConfigEffectSinks sinks_;
    TrackedMutex mtx_{"config.effects"};
    AppliedConfigEffects applied_;
};

} // namespace satellite
