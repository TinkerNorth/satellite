// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <cstdint>
#include <cstring>

namespace mdns {

struct RejoinDecision {
    bool rejoin;
    bool ipChanged;
};

inline RejoinDecision evaluateRejoin(bool force, bool haveOld, const uint8_t oldIp[4], bool haveNew,
                                     const uint8_t newIp[4]) {
    const bool ipChanged = (haveNew != haveOld) || (haveNew && std::memcmp(newIp, oldIp, 4) != 0);
    return RejoinDecision{force || ipChanged, ipChanged};
}

} // namespace mdns
