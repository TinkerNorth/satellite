// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>

namespace fw {

inline constexpr uint32_t PROFILE_DOMAIN = 1;
inline constexpr uint32_t PROFILE_PRIVATE = 2;
inline constexpr uint32_t PROFILE_PUBLIC = 4;

enum class FirewallState : uint8_t { Unknown, Configured, WrongProfile, Missing };

inline uint32_t profileBit(const std::string& category) {
    if (category == "domain") return PROFILE_DOMAIN;
    if (category == "private") return PROFILE_PRIVATE;
    if (category == "public") return PROFILE_PUBLIC;
    return 0U;
}

inline FirewallState evaluateFirewall(uint32_t activeBit, uint32_t ruleMask, bool haveRule) {
    if (!haveRule) return FirewallState::Missing;
    if (activeBit == 0U) return FirewallState::Unknown;
    return (ruleMask & activeBit) != 0U ? FirewallState::Configured : FirewallState::WrongProfile;
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
