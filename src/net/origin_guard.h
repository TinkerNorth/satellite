// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <string>

namespace satellite {

inline bool isLoopbackLiteral(const std::string& h) {
    return h == "127.0.0.1" || h == "localhost" || h == "::1";
}

inline bool isLoopbackHost(const std::string& v) {
    if (v.empty()) return false;
    if (v.front() == '[') {
        auto close = v.find(']');
        if (close == std::string::npos) return false;
        std::string rest = v.substr(close + 1);
        if (!rest.empty() && rest.front() != ':') return false;
        return isLoopbackLiteral(v.substr(1, close - 1));
    }
    if (isLoopbackLiteral(v)) return true;
    auto colon = v.find(':');
    if (colon == std::string::npos || v.find(':', colon + 1) != std::string::npos) return false;
    return isLoopbackLiteral(v.substr(0, colon));
}

inline bool isLoopbackOrigin(const std::string& origin) {
    auto schemeEnd = origin.find("//");
    if (schemeEnd == std::string::npos) return false;
    return isLoopbackHost(origin.substr(schemeEnd + 2));
}

} // namespace satellite
