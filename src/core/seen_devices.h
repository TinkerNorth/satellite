// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include "diagnostics_bundle.h"
#include "log_ring.h"
#include "tracked_mutex.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

namespace satellite {

// Every device id and name the pairing and session routes have met this run,
// paired now or not, so the diagnostics scrubbers alias a name a rejected
// requester or an unpaired device left in the log ring. Held to the ring's
// size, least recently met dropped first: the ring has let a dropped entry's
// lines go by the time it is dropped, unless that device is connected or
// paired, and those the scrubbers alias anyway.
class SeenDevices {
  public:
    static constexpr size_t CAPACITY = LOG_RING_SLOTS;

    void note(const std::string& deviceId, const std::string& deviceName) {
        if (deviceId.empty() && deviceName.empty()) return;
        std::lock_guard<TrackedMutex> lk(mtx_);
        const auto same = std::find_if(seen_.begin(), seen_.end(), [&](const HostIdentity& h) {
            return h.deviceId == deviceId && h.deviceName == deviceName;
        });
        if (same != seen_.end()) seen_.erase(same);
        if (seen_.size() >= CAPACITY) seen_.erase(seen_.begin());
        seen_.push_back({deviceId, deviceName, ""});
    }

    std::vector<HostIdentity> all() const {
        std::lock_guard<TrackedMutex> lk(mtx_);
        return seen_;
    }

  private:
    mutable TrackedMutex mtx_{"diagnostics.seen"};
    std::vector<HostIdentity> seen_;
};

} // namespace satellite
