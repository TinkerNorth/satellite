// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

namespace satellite {

inline std::vector<std::string> semverSplitDots(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        size_t dot = s.find('.', start);
        if (dot == std::string::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, dot - start));
        start = dot + 1;
    }
    return out;
}

inline bool semverIdentIsNumeric(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

inline int semverCompareNumericIdent(const std::string& a, const std::string& b) {
    size_t ai = 0, bi = 0;
    while (ai + 1 < a.size() && a[ai] == '0') ai++;
    while (bi + 1 < b.size() && b[bi] == '0') bi++;
    size_t alen = a.size() - ai, blen = b.size() - bi;
    if (alen != blen) return alen < blen ? -1 : 1;
    int c = a.compare(ai, alen, b, bi, blen);
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

inline int semverComparePrerelease(const std::string& a, const std::string& b) {
    if (a.empty() && b.empty()) return 0;
    if (a.empty()) return 1;
    if (b.empty()) return -1;

    std::vector<std::string> av = semverSplitDots(a);
    std::vector<std::string> bv = semverSplitDots(b);
    size_t n = av.size() < bv.size() ? av.size() : bv.size();
    for (size_t i = 0; i < n; i++) {
        bool an = semverIdentIsNumeric(av[i]);
        bool bn = semverIdentIsNumeric(bv[i]);
        if (an && bn) {
            int c = semverCompareNumericIdent(av[i], bv[i]);
            if (c != 0) return c;
        } else if (an != bn) {
            return an ? -1 : 1;
        } else {
            if (av[i] < bv[i]) return -1;
            if (av[i] > bv[i]) return 1;
        }
    }
    if (av.size() != bv.size()) return av.size() < bv.size() ? -1 : 1;
    return 0;
}

inline int compareSemver(const std::string& a, const std::string& b) {
    std::string aCore = a, aPre, bCore = b, bPre;
    size_t aDash = a.find('-');
    if (aDash != std::string::npos) {
        aCore = a.substr(0, aDash);
        aPre = a.substr(aDash + 1);
    }
    size_t bDash = b.find('-');
    if (bDash != std::string::npos) {
        bCore = b.substr(0, bDash);
        bPre = b.substr(bDash + 1);
    }

    std::vector<std::string> ac = semverSplitDots(aCore);
    std::vector<std::string> bc = semverSplitDots(bCore);
    for (size_t i = 0; i < 3; i++) {
        std::string ax = i < ac.size() && semverIdentIsNumeric(ac[i]) ? ac[i] : "0";
        std::string bx = i < bc.size() && semverIdentIsNumeric(bc[i]) ? bc[i] : "0";
        int c = semverCompareNumericIdent(ax, bx);
        if (c != 0) return c;
    }
    return semverComparePrerelease(aPre, bPre);
}

} // namespace satellite
