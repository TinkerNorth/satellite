// SPDX-License-Identifier: LGPL-3.0-or-later

// Shared JSON facade over nlohmann/json (vendored at lib/nlohmann/json.hpp).
// Centralizes the two canonical types and the tolerant accessors every builder
// and parser in the project uses, so call sites stay strongly typed instead of
// concatenating strings by hand.
//
//   JsonOut  — ordered_json for RESPONSE/FILE building. Field insertion order is
//              preserved, so the serialized bytes stay stable and reviewable
//              (external clients are order-independent, but our own golden tests
//              and diffs are not).
//   Json     — the regular (node-ordered) value for PARSING untrusted input,
//              where key order is irrelevant.
#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

namespace satellite {

using JsonOut = nlohmann::ordered_json;
using Json = nlohmann::json;

// Serialize compactly. error_handler_t::replace swaps any invalid UTF-8 byte for
// U+FFFD rather than throwing — OS-derived strings (interface names, device
// names) are not guaranteed well-formed UTF-8, and a control-plane response must
// never abort on one.
inline std::string jsonDump(const JsonOut& j) {
    return j.dump(/*indent=*/-1, /*indent_char=*/' ', /*ensure_ascii=*/false,
                  JsonOut::error_handler_t::replace);
}

// Pretty-print for human-facing files (config.json). Same UTF-8 safety.
inline std::string jsonDumpPretty(const JsonOut& j, int indent = 2) {
    return j.dump(indent, ' ', /*ensure_ascii=*/false, JsonOut::error_handler_t::replace);
}

// Non-throwing parse. Returns false and leaves `out` discarded on malformed
// input; callers fall back to defaults exactly as the old tolerant helpers did.
inline bool jsonParse(const std::string& text, Json& out) {
    out = Json::parse(text, /*cb=*/nullptr, /*allow_exceptions=*/false);
    return !out.is_discarded();
}

// ── Tolerant typed accessors ────────────────────────────────────────────────
// A missing key OR a wrong-typed value yields the fallback (never throws),
// mirroring the pre-nlohmann json_mini contract that request-body and config
// parsing depend on.

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

// Presence-reporting variants — the equivalent of the old jsonGetIntKeyed /
// jsonGetBoolKeyed: write `out` and return true only when the key exists with
// the right type, so callers can distinguish "absent" from "present and 0/false".
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

// 64-bit variant — `long` is 32-bit on LLP64 (Windows), so epoch-style values
// must not round-trip through jsonTryInt.
inline bool jsonTryI64(const Json& j, const char* key, int64_t& out) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_number_integer()) return false;
    out = it->get<int64_t>();
    return true;
}

// The object value of `key`, or an empty object when absent/not-an-object — lets
// callers descend into nested blocks (e.g. `caps`, `hostFeatures`) tolerantly.
inline Json jsonObject(const Json& j, const char* key) {
    auto it = j.find(key);
    return (it != j.end() && it->is_object()) ? *it : Json::object();
}

} // namespace satellite
