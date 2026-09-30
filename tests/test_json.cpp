// SPDX-License-Identifier: LGPL-3.0-or-later
#include "../src/core/json.h"

#include <cstddef>
#include <iostream>
#include <string>

#include "test_util.h"

using namespace satellite;

static void test_parse_tolerant() {
    TEST("jsonParse: valid object succeeds, malformed/empty/non-object discard");
    Json j;
    EXPECT(jsonParse(R"({"a":1})", j));
    EXPECT(j.is_object());
    Json bad;
    EXPECT(!jsonParse("not json", bad));
    EXPECT(!jsonParse("", bad));
    EXPECT(!jsonParse("{\"a\":", bad));
    Json arr;
    EXPECT(jsonParse("[1,2,3]", arr));
    EXPECT(arr.is_array());
}

static void test_typed_accessors_present() {
    TEST("jsonStr/jsonInt/jsonBool: extract present, correctly-typed values");
    Json j = Json::parse(R"({"s":"hi","n":42,"b":true})");
    EXPECT_EQ(jsonStr(j, "s"), std::string("hi"));
    EXPECT_EQ(jsonInt(j, "n"), 42L);
    EXPECT(jsonBool(j, "b"));
}

static void test_typed_accessors_fallback() {
    TEST("jsonStr/jsonInt/jsonBool: absent or wrong-typed yields the fallback");
    Json j = Json::parse(R"({"s":"hi","n":42,"b":true})");
    EXPECT_EQ(jsonStr(j, "missing", "def"), std::string("def"));
    EXPECT_EQ(jsonInt(j, "missing", -1), -1L);
    EXPECT(!jsonBool(j, "missing"));
    EXPECT(jsonBool(j, "missing", true));
    EXPECT_EQ(jsonInt(j, "s", 7), 7L);
    EXPECT(!jsonBool(j, "n"));
    EXPECT_EQ(jsonStr(j, "n", "x"), std::string("x"));
}

static void test_try_accessors() {
    TEST("jsonTryInt/jsonTryBool/jsonTryI64: report presence, distinguish 0/false from absent");
    Json j = Json::parse(R"({"n":0,"b":false,"big":4294967296,"s":"x"})");
    long n = -99;
    EXPECT(jsonTryInt(j, "n", n));
    EXPECT_EQ(n, 0L);
    bool b = true;
    EXPECT(jsonTryBool(j, "b", b));
    EXPECT(!b);
    int64_t big = 0;
    EXPECT(jsonTryI64(j, "big", big));
    EXPECT_EQ(big, (int64_t)4294967296LL);
    long untouched = 123;
    EXPECT(!jsonTryInt(j, "missing", untouched));
    EXPECT_EQ(untouched, 123L);
    EXPECT(!jsonTryInt(j, "s", untouched));
    EXPECT_EQ(untouched, 123L);
}

static void test_object_descend() {
    TEST("jsonObject: returns the nested object, or an empty object when absent/not-object");
    Json j = Json::parse(R"({"caps":{"rumble":true},"x":5})");
    Json caps = jsonObject(j, "caps");
    EXPECT(jsonBool(caps, "rumble"));
    EXPECT(jsonObject(j, "missing").is_object());
    EXPECT(jsonObject(j, "missing").empty());
    EXPECT(jsonObject(j, "x").empty());
}

// A PUT /api/connections body whose hostFeatures object nests `depth` levels.
static std::string hostFeaturesNested(size_t depth) {
    std::string body = "{\"hostFeatures\":";
    for (size_t level = 0; level < depth; ++level) body += "{\"a\":";
    body += "true";
    body.append(depth, '}');
    body += "}";
    return body;
}

// 100000 levels (about 600 KB) outrun any thread's stack in one recursive
// basic_json copy, and the route copies hostFeatures out of what it parsed.
static void test_deep_request_body_is_refused_before_anything_copies_it() {
    TEST("jsonParse: a body nesting hostFeatures 100000 deep is refused, never copied");
    Json body;
    const bool accepted = jsonParse(hostFeaturesNested(100000), body);
    EXPECT(!accepted);
    EXPECT(jsonObject(body, "hostFeatures").empty());
}

// `depth` arrays, each holding the next; the innermost is empty.
static std::string arraysNested(size_t depth) {
    return std::string(depth, '[') + std::string(depth, ']');
}

// `depth` objects, each holding the next under "a"; the innermost holds 0.
static std::string objectsNested(size_t depth) {
    std::string text;
    for (size_t level = 0; level < depth; ++level) text += "{\"a\":";
    text += "0";
    text.append(depth, '}');
    return text;
}

// One array holding `count` empty arrays side by side.
static std::string siblingArrays(size_t count) {
    std::string text = "[";
    for (size_t i = 0; i < count; ++i) text += (i == 0) ? "[]" : ",[]";
    text += "]";
    return text;
}

static void test_nesting_up_to_the_bound_parses() {
    TEST("jsonParse: arrays and objects nested exactly JSON_MAX_DEPTH deep parse");
    Json arrays;
    EXPECT(jsonParse(arraysNested(JSON_MAX_DEPTH), arrays));
    EXPECT(arrays.is_array());
    Json objects;
    EXPECT(jsonParse(objectsNested(JSON_MAX_DEPTH), objects));
    EXPECT(objects.is_object());
}

static void test_nesting_one_past_the_bound_is_refused_like_malformed_json() {
    TEST("jsonParse: one level past JSON_MAX_DEPTH is refused and discarded, arrays or objects");
    Json arrays;
    EXPECT(!jsonParse(arraysNested(JSON_MAX_DEPTH + 1), arrays));
    EXPECT(arrays.is_discarded());
    Json objects;
    EXPECT(!jsonParse(objectsNested(JSON_MAX_DEPTH + 1), objects));
    EXPECT(objects.is_discarded());
}

static void test_depth_counts_open_containers_not_siblings() {
    TEST("jsonParse: a thousand sibling arrays sit two levels deep, not a thousand");
    Json siblings;
    EXPECT(jsonParse(siblingArrays(1000), siblings));
    EXPECT_EQ(siblings.size(), static_cast<size_t>(1000));
}

static void test_request_body_absent_reads_as_an_empty_object() {
    TEST("jsonRequestBody: no body at all reads as {}");
    Json body;
    EXPECT(jsonRequestBody("", body));
    EXPECT(body.is_object());
    EXPECT(body.empty());
}

static void test_request_body_object_is_the_body() {
    TEST("jsonRequestBody: a JSON object is taken as the body");
    Json body;
    EXPECT(jsonRequestBody(R"({"deviceName":"Pixel 9"})", body));
    EXPECT_EQ(jsonStr(body, "deviceName"), std::string("Pixel 9"));
}

static void test_request_body_that_is_not_an_object_is_refused() {
    TEST("jsonRequestBody: malformed text, a non-object and an over-deep object are refused");
    Json body;
    EXPECT(!jsonRequestBody("{\"controllers\":[", body));
    EXPECT(!jsonRequestBody("[1,2]", body));
    EXPECT(!jsonRequestBody(objectsNested(JSON_MAX_DEPTH + 1), body));
}

static void test_dump_is_ordered_and_compact() {
    TEST("jsonDump: preserves insertion order, compact (no spaces)");
    JsonOut j;
    j["z"] = 1;
    j["a"] = 2;
    j["m"] = 3;
    EXPECT_EQ(jsonDump(j), std::string(R"({"z":1,"a":2,"m":3})"));
}

static void test_dump_escaping() {
    TEST("jsonDump: escapes quotes/backslash/newline/control, keeps forward slash + UTF-8");
    JsonOut j;
    j["q"] = "a\"b";
    j["bs"] = "a\\b";
    j["nl"] = "a\nb";
    j["ctrl"] = std::string("a\x01"
                            "b");
    j["url"] = "/api/catalog/images/ds4";
    j["utf8"] = "übersetzt";
    const std::string s = jsonDump(j);
    EXPECT(s.find("\"q\":\"a\\\"b\"") != std::string::npos);
    EXPECT(s.find("\"bs\":\"a\\\\b\"") != std::string::npos);
    EXPECT(s.find("\"nl\":\"a\\nb\"") != std::string::npos);
    EXPECT(s.find("\"ctrl\":\"a\\u0001b\"") != std::string::npos);
    EXPECT(s.find("/api/catalog/images/ds4") != std::string::npos);
    EXPECT(s.find("\xC3\xBC"
                  "bersetzt") != std::string::npos);
}

static void test_dump_invalid_utf8_does_not_throw() {
    TEST("jsonDump: invalid UTF-8 degrades to U+FFFD instead of throwing");
    JsonOut j;
    j["name"] = std::string("bad\xff\xfe");
    std::string s;
    bool threw = false;
    try {
        s = jsonDump(j);
    } catch (...) { threw = true; }
    EXPECT(!threw);
    EXPECT(!s.empty());
    Json round;
    EXPECT(jsonParse(s, round));
}

static void test_dump_pretty() {
    TEST("jsonDumpPretty: indented, newline-separated");
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
    test_deep_request_body_is_refused_before_anything_copies_it();
    test_nesting_up_to_the_bound_parses();
    test_nesting_one_past_the_bound_is_refused_like_malformed_json();
    test_depth_counts_open_containers_not_siblings();
    test_request_body_absent_reads_as_an_empty_object();
    test_request_body_object_is_the_body();
    test_request_body_that_is_not_an_object_is_refused();
    test_dump_is_ordered_and_compact();
    test_dump_escaping();
    test_dump_invalid_utf8_does_not_throw();
    test_dump_pretty();

    std::cout << "json: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
