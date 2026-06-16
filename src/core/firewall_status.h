// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <string>

namespace fw {

inline constexpr int PROFILE_DOMAIN = 1;
inline constexpr int PROFILE_PRIVATE = 2;
inline constexpr int PROFILE_PUBLIC = 4;

enum class FirewallState { Unknown, Configured, WrongProfile, Missing };

inline int profileBit(const std::string& category) {
    if (category == "domain") return PROFILE_DOMAIN;
    if (category == "private") return PROFILE_PRIVATE;
    if (category == "public") return PROFILE_PUBLIC;
    return 0;
}

inline FirewallState evaluateFirewall(int activeBit, int ruleMask, bool haveRule) {
    if (!haveRule) return FirewallState::Missing;
    if (activeBit == 0) return FirewallState::Unknown;
    return (ruleMask & activeBit) != 0 ? FirewallState::Configured : FirewallState::WrongProfile;
}

inline std::string firewallStateString(FirewallState s) {
    switch (s) {
    case FirewallState::Configured:
        return "configured";
    case FirewallState::WrongProfile:
        return "wrong-profile";
    case FirewallState::Missing:
        return "missing";
    case FirewallState::Unknown:
        return "unknown";
    }
    return "unknown";
}

} // namespace fw
