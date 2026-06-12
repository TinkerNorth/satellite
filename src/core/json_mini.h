// SPDX-License-Identifier: LGPL-3.0-or-later

// Minimal, dependency-free JSON value extraction shared by the request-body
// parsers (webserver) and the catalog builder. Tolerant by design: extraction
// returns "absent" rather than erroring, and every helper is pure so the
// guards can be unit tested with raw strings.
#pragma once

#include <string>
#include <vector>

namespace satellite {

// Locate the first character of the value for a top-level-or-nested *quoted*
// key, i.e. the position just after `"key" :`. False when the key is absent.
inline bool jsonValueStart(const std::string& json, const std::string& key, size_t& out) {
    std::string needle = "\"" + key + "\"";
    size_t pos = 0;
    for (;;) {
        pos = json.find(needle, pos);
        if (pos == std::string::npos) return false;
        size_t colon = json.find_first_not_of(" \t\r\n", pos + needle.size());
        if (colon == std::string::npos || json[colon] != ':') {
            pos += needle.size();
            continue; // "key" not followed by ':' — keep searching
        }
        out = colon + 1;
        return true;
    }
}

inline bool jsonGetBoolKeyed(const std::string& json, const std::string& key, bool* out) {
    size_t vs;
    if (!jsonValueStart(json, key, vs)) return false;
    size_t t = json.find_first_not_of(" \t\r\n", vs);
    if (t == std::string::npos) return false;
    if (json.compare(t, 4, "true") == 0) {
        *out = true;
        return true;
    }
    if (json.compare(t, 5, "false") == 0) {
        *out = false;
        return true;
    }
    return false; // not a boolean literal — treat as absent
}

inline bool jsonGetIntKeyed(const std::string& json, const std::string& key, long* out) {
    size_t vs;
    if (!jsonValueStart(json, key, vs)) return false;
    size_t t = json.find_first_not_of(" \t\r\n", vs);
    if (t == std::string::npos) return false;
    // Require a numeric token so non-numbers aren't silently coerced to 0.
    if (json[t] != '-' && (json[t] < '0' || json[t] > '9')) return false;
    char* end = nullptr;
    long v = strtol(json.c_str() + t, &end, 10);
    if (end == json.c_str() + t) return false; // no digits consumed
    *out = v; // strtol overflow is harmless — the caller range-checks
    return true;
}

// Span (inclusive of braces/brackets) of the structural value at `from`,
// honouring nesting and quoted strings with escapes. Empty on malformed input.
inline std::string jsonStructuralSpan(const std::string& json, size_t from, char open, char close) {
    size_t start = json.find_first_not_of(" \t\r\n", from);
    if (start == std::string::npos || json[start] != open) return "";
    int depth = 0;
    bool inString = false;
    for (size_t i = start; i < json.size(); i++) {
        char c = json[i];
        if (inString) {
            if (c == '\\') {
                i++; // skip the escaped char
            } else if (c == '"') {
                inString = false;
            }
            continue;
        }
        if (c == '"') {
            inString = true;
        } else if (c == open) {
            depth++;
        } else if (c == close) {
            depth--;
            if (depth == 0) return json.substr(start, i - start + 1);
        }
    }
    return "";
}

// The {...} object value of `key`, braces included. Empty when absent/not an object.
inline std::string jsonGetObject(const std::string& json, const std::string& key) {
    size_t vs;
    if (!jsonValueStart(json, key, vs)) return "";
    return jsonStructuralSpan(json, vs, '{', '}');
}

// Each top-level {...} element of `key`'s [...] array. Empty when absent/not
// an array. `outPresent` distinguishes "key absent" from "empty array".
inline std::vector<std::string> jsonGetArrayObjects(const std::string& json, const std::string& key,
                                                    bool* outPresent = nullptr) {
    std::vector<std::string> objs;
    if (outPresent) *outPresent = false;
    size_t vs;
    if (!jsonValueStart(json, key, vs)) return objs;
    std::string arr = jsonStructuralSpan(json, vs, '[', ']');
    if (arr.empty()) return objs;
    if (outPresent) *outPresent = true;
    size_t i = 1; // past '['
    while (i + 1 < arr.size()) {
        size_t objStart = arr.find('{', i);
        if (objStart == std::string::npos) break;
        std::string obj = jsonStructuralSpan(arr, objStart, '{', '}');
        if (obj.empty()) break;
        objs.push_back(obj);
        i = objStart + obj.size();
    }
    return objs;
}

} // namespace satellite
