// SPDX-License-Identifier: LGPL-3.0-or-later

// core/json — the shared nlohmann/json facade: tolerant typed accessors used by
// request-body and config parsing, plus the ordered, UTF-8-safe serializer used
// by every response builder.
#include "../src/core/json.h"

#include <iostream>
#include <string>

#include "test_util.h"

using namespace satellite;

static void test_parse_tolerant() {
    TEST("jsonParse — valid object succeeds, malformed/empty/non-object discard");
    Json j;
    EXPECT(jsonParse(R"({"a":1})", j));
    EXPECT(j.is_object());
    Json bad;
    EXPECT(!jsonParse("not json", bad));
    EXPECT(!jsonParse("", bad));
    EXPECT(!jsonParse("{\"a\":", bad)); // truncated
    Json arr;
    EXPECT(jsonParse("[1,2,3]", arr)); // valid JSON, just not an object
    EXPECT(arr.is_array());
}

static void test_typed_accessors_present() {
    TEST("jsonStr/jsonInt/jsonBool — extract present, correctly-typed values");
    Json j = Json::parse(R"({"s":"hi","n":42,"b":true})");
    EXPECT_EQ(jsonStr(j, "s"), std::string("hi"));
    EXPECT_EQ(jsonInt(j, "n"), 42L);
    EXPECT(jsonBool(j, "b"));
}

static void test_typed_accessors_fallback() {
    TEST("jsonStr/jsonInt/jsonBool — absent or wrong-typed yields the fallback");
    Json j = Json::parse(R"({"s":"hi","n":42,"b":true})");
    // Absent keys fall back.
    EXPECT_EQ(jsonStr(j, "missing", "def"), std::string("def"));
    EXPECT_EQ(jsonInt(j, "missing", -1), -1L);
    EXPECT(!jsonBool(j, "missing"));
    EXPECT(jsonBool(j, "missing", true));
    // Wrong-typed values fall back (never throw): "s" is a string, not int/bool.
    EXPECT_EQ(jsonInt(j, "s", 7), 7L);
    EXPECT(!jsonBool(j, "n"));
    EXPECT_EQ(jsonStr(j, "n", "x"), std::string("x"));
}

static void test_try_accessors() {
    TEST("jsonTryInt/jsonTryBool/jsonTryI64 — report presence, distinguish 0/false from absent");
    Json j = Json::parse(R"({"n":0,"b":false,"big":4294967296,"s":"x"})");
    long n = -99;
    EXPECT(jsonTryInt(j, "n", n));
    EXPECT_EQ(n, 0L); // present-and-zero is still "present"
    bool b = true;
    EXPECT(jsonTryBool(j, "b", b));
    EXPECT(!b);
    int64_t big = 0;
    EXPECT(jsonTryI64(j, "big", big));
    EXPECT_EQ(big, (int64_t)4294967296LL); // 64-bit, not truncated to 32
    // Absent / wrong-type report false and leave the out-param untouched.
    long untouched = 123;
    EXPECT(!jsonTryInt(j, "missing", untouched));
    EXPECT_EQ(untouched, 123L);
    EXPECT(!jsonTryInt(j, "s", untouched)); // present but a string
    EXPECT_EQ(untouched, 123L);
}

static void test_object_descend() {
    TEST("jsonObject — returns the nested object, or an empty object when absent/not-object");
    Json j = Json::parse(R"({"caps":{"rumble":true},"x":5})");
    Json caps = jsonObject(j, "caps");
    EXPECT(jsonBool(caps, "rumble"));
    EXPECT(jsonObject(j, "missing").is_object());
    EXPECT(jsonObject(j, "missing").empty());
    EXPECT(jsonObject(j, "x").empty()); // "x" is a number, not an object
}

static void test_dump_is_ordered_and_compact() {
    TEST("jsonDump — preserves insertion order, compact (no spaces)");
    JsonOut j;
    j["z"] = 1;
    j["a"] = 2;
    j["m"] = 3;
    EXPECT_EQ(jsonDump(j), std::string(R"({"z":1,"a":2,"m":3})"));
}

static void test_dump_escaping() {
    TEST("jsonDump — escapes quotes/backslash/newline/control, keeps forward slash + UTF-8");
    JsonOut j;
    j["q"] = "a\"b";
    j["bs"] = "a\\b";
    j["nl"] = "a\nb";
    j["ctrl"] = std::string("a\x01"
                            "b");
    j["url"] = "/api/catalog/images/ds4";
    j["utf8"] = "übersetzt"; // non-ASCII stays raw UTF-8 (ensure_ascii=false)
    const std::string s = jsonDump(j);
    EXPECT(s.find("\"q\":\"a\\\"b\"") != std::string::npos);
    EXPECT(s.find("\"bs\":\"a\\\\b\"") != std::string::npos);
    EXPECT(s.find("\"nl\":\"a\\nb\"") != std::string::npos);
    EXPECT(s.find("\"ctrl\":\"a\\u0001b\"") != std::string::npos);
    EXPECT(s.find("/api/catalog/images/ds4") != std::string::npos); // '/' not escaped
    EXPECT(s.find("\xC3\xBC"
                  "bersetzt") != std::string::npos); // ü kept as UTF-8
}

static void test_dump_invalid_utf8_does_not_throw() {
    TEST("jsonDump — invalid UTF-8 degrades to U+FFFD instead of throwing");
    JsonOut j;
    j["name"] = std::string("bad\xff\xfe"); // lone high bytes — not valid UTF-8
    std::string s;
    bool threw = false;
    try {
        s = jsonDump(j);
    } catch (...) { threw = true; }
    EXPECT(!threw);
    EXPECT(!s.empty());
    // The result is still parseable JSON.
    Json round;
    EXPECT(jsonParse(s, round));
}

static void test_dump_pretty() {
    TEST("jsonDumpPretty — indented, newline-separated");
    JsonOut j;
    j["a"] = 1;
    const std::string s = jsonDumpPretty(j);
    EXPECT(s.find("\n") != std::string::npos);
    EXPECT(s.find("    \"a\": 1") != std::string::npos ||
           s.find("  \"a\": 1") != std::string::npos);
}

int main() {
    test_parse_tolerant();
    test_typed_accessors_present();
    test_typed_accessors_fallback();
    test_try_accessors();
    test_object_descend();
    test_dump_is_ordered_and_compact();
    test_dump_escaping();
    test_dump_invalid_utf8_does_not_throw();
    test_dump_pretty();

    std::cout << "json: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
