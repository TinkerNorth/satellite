// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string>

namespace satellite {

inline std::string infTextToNarrow(const std::string& bytes) {
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
        static_cast<unsigned char>(bytes[1]) == 0xFE) {
        std::string out;
        out.reserve(bytes.size() / 2);
        for (size_t i = 2; i + 1 < bytes.size(); i += 2) {
            out.push_back(bytes[i + 1] == 0 ? bytes[i] : '?');
        }
        return out;
    }
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF) {
        return bytes.substr(3);
    }
    return bytes;
}

inline std::string parseInfDriverVersion(const std::string& infBytes) {
    const std::string text = infTextToNarrow(infBytes);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;

        size_t i = 0;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) i++;
        if (line.compare(i, 9, "DriverVer") != 0) continue;
        i += 9;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) i++;
        if (i >= line.size() || line[i] != '=') continue;
        i++;
        size_t comma = line.find(',', i);
        if (comma == std::string::npos) continue;
        size_t v = comma + 1;
        while (v < line.size() && (line[v] == ' ' || line[v] == '\t')) v++;
        size_t e = v;
        while (e < line.size() && ((line[e] >= '0' && line[e] <= '9') || line[e] == '.')) e++;
        if (e == v || line[v] == '.') continue;
        return line.substr(v, e - v);
    }
    return "";
}

} // namespace satellite
