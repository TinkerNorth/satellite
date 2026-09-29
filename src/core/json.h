// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

namespace satellite {

using JsonOut = nlohmann::ordered_json;
using Json = nlohmann::json;

// Deeper than any document Satellite reads (a GitHub releases list nests 5, a
// Dish request body 4), and shallow enough for nlohmann 3.12's copy, comparison
// and dump, which recurse once per level (its parser and destructor do not).
constexpr size_t JSON_MAX_DEPTH = 32;

// SAX events that build nothing: the walk stops at the first container opening
// past JSON_MAX_DEPTH, before a DOM of any depth exists.
class JsonDepthGuard {
  public:
    bool null() { return true; }
    bool boolean(bool) { return true; }
    bool number_integer(Json::number_integer_t) { return true; }
    bool number_unsigned(Json::number_unsigned_t) { return true; }
    bool number_float(Json::number_float_t, const Json::string_t&) { return true; }
    bool string(Json::string_t&) { return true; }
    bool binary(Json::binary_t&) { return true; }
    bool key(Json::string_t&) { return true; }
    bool start_object(size_t) { return open(); }
    bool start_array(size_t) { return open(); }
    bool end_object() { return close(); }
    bool end_array() { return close(); }
    bool parse_error(size_t, const std::string&, const Json::exception&) { return false; }

  private:
    bool open() {
        ++depth_;
        return depth_ <= JSON_MAX_DEPTH;
    }

    bool close() {
        --depth_;
        return true;
    }

    size_t depth_ = 0;
};

inline bool jsonWellFormedWithinDepth(const std::string& text) {
    JsonDepthGuard guard;
    return Json::sax_parse(text, &guard);
}

inline std::string jsonDump(const JsonOut& j) {
    return j.dump(-1, ' ', false, JsonOut::error_handler_t::replace);
}

inline std::string jsonDumpPretty(const JsonOut& j, int indent = 2) {
    return j.dump(indent, ' ', false, JsonOut::error_handler_t::replace);
}

// Nesting past JSON_MAX_DEPTH is refused exactly as malformed text is.
inline bool jsonParse(const std::string& text, Json& out) {
    if (!jsonWellFormedWithinDepth(text)) {
        out = Json(Json::value_t::discarded);
        return false;
    }
    out = Json::parse(text, nullptr, false);
    return !out.is_discarded();
}

// A request body is a JSON object, or nothing at all, which reads as {}.
inline bool jsonRequestBody(const std::string& text, Json& out) {
    if (text.empty()) {
        out = Json::object();
        return true;
    }
    return jsonParse(text, out) && out.is_object();
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
