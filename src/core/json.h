// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

namespace satellite {

using JsonOut = nlohmann::ordered_json;
using Json = nlohmann::json;

inline std::string jsonDump(const JsonOut& j) {
    return j.dump(-1, ' ', false, JsonOut::error_handler_t::replace);
}

inline std::string jsonDumpPretty(const JsonOut& j, int indent = 2) {
    return j.dump(indent, ' ', false, JsonOut::error_handler_t::replace);
}

inline bool jsonParse(const std::string& text, Json& out) {
    out = Json::parse(text, nullptr, false);
    return !out.is_discarded();
}

inline bool jsonBool(const Json& j, const char* key, bool fallback = false) {
    auto it = j.find(key);
    return (it != j.end() && it->is_boolean()) ? it->get<bool>() : fallback;
}

inline long jsonInt(const Json& j, const char* key, long fallback = 0) {
    auto it = j.find(key);
    return (it != j.end() && it->is_number_integer()) ? it->get<long>() : fallback;
}

inline std::string jsonStr(const Json& j, const char* key, const std::string& fallback = "") {
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : fallback;
}

inline bool jsonTryInt(const Json& j, const char* key, long& out) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_number_integer()) return false;
    out = it->get<long>();
    return true;
}

inline bool jsonTryBool(const Json& j, const char* key, bool& out) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_boolean()) return false;
    out = it->get<bool>();
    return true;
}

inline bool jsonTryI64(const Json& j, const char* key, int64_t& out) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_number_integer()) return false;
    out = it->get<int64_t>();
    return true;
}

inline Json jsonObject(const Json& j, const char* key) {
    auto it = j.find(key);
    return (it != j.end() && it->is_object()) ? *it : Json::object();
}

} // namespace satellite
