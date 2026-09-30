// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <string.h>

#include <string>

namespace lifecycle {

// The quoting an HKCU\Run entry carries around its path.
inline std::string stripQuotes(const std::string& s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') return s.substr(1, s.size() - 2);
    return s;
}

// Whether the Run entry is (re)written for `exe`. Never for an entry pointing
// at a DIFFERENT real exe, which lets a side-loaded build coexist with an
// installed one; yes when there is none, when it already points at this exe
// (re-normalising quoting and case), or when its target is gone (self-heal).
inline bool runEntryNeedsWrite(const std::string& existing, const std::string& exe,
                               bool existingTargetExists) {
    if (existing.empty()) return true;
    const std::string existingPath = stripQuotes(existing);
    const bool pointsAtThisExe = _stricmp(existingPath.c_str(), exe.c_str()) == 0;
    return pointsAtThisExe || !existingTargetExists;
}

} // namespace lifecycle
