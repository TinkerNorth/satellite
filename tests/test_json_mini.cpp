// SPDX-License-Identifier: LGPL-3.0-or-later

// core/json_mini.h — the dependency-free extraction helpers the session PUT
// body parser is built on. Malformed input must read as "absent", never as a
// crash or an over-read.
#include "../src/core/json_mini.h"

#include <iostream>
#include <string>

static int g_pass = 0;
static int g_fail = 0;
static std::string g_currentTest;

#define TEST(name)                                                                                 \
    do { g_currentTest = (name); } while (0)

#define EXPECT(cond)                                                                               \
    do {                                                                                           \
        if (cond) {                                                                                \
            g_pass++;                                                                              \
        } else {                                                                                   \
            g_fail++;                                                                              \
            std::cerr << "  FAIL [" << g_currentTest << "] " << __FILE__ << ":" << __LINE__        \
                      << "  " << #cond << "\n";                                                    \
        }                                                                                          \
    } while (0)

#define EXPECT_EQ(a, b)                                                                            \
    do {                                                                                           \
        auto _a = (a);                                                                             \
        auto _b = (b);                                                                             \
        if (_a == _b) {                                                                            \
            g_pass++;                                                                              \
        } else {                                                                                   \
            g_fail++;                                                                              \
            std::cerr << "  FAIL [" << g_currentTest << "] " << __FILE__ << ":" << __LINE__        \
                      << "  " << #a << " == " << #b << "  (got " << _a << " vs " << _b << ")\n";   \
        }                                                                                          \
    } while (0)

using namespace satellite;

static void test_valueStart() {
    TEST("jsonValueStart — locates quoted keys, skips look-alikes");
    size_t pos = 0;
    EXPECT(jsonValueStart(R"({"a":1})", "a", pos));
    EXPECT(!jsonValueStart(R"({"a":1})", "b", pos));
    // A quoted key inside a string VALUE is not followed by ':' here, but a
    // later real key must still be found. (Hoisted out of the macro argument:
    // MSVC's traditional preprocessor mangles raw strings containing \" when
    // it stringizes #cond.)
    const char* escapedKeyBody = R"({"x":"not \"k\" here","k":2})";
    EXPECT(jsonValueStart(escapedKeyBody, "k", pos));
    // Key-as-substring must not match ("ab" vs "b" needs the quotes).
    EXPECT(!jsonValueStart(R"({"ab":1})", "b", pos));
    EXPECT(!jsonValueStart("", "a", pos));
}

static void test_boolKeyed() {
    TEST("jsonGetBoolKeyed — booleans only, scoped to the key");
    bool v = false;
    EXPECT(jsonGetBoolKeyed(R"({"on":true})", "on", &v) && v);
    EXPECT(jsonGetBoolKeyed(R"({"on": false })", "on", &v) && !v);
    v = true;
    EXPECT(!jsonGetBoolKeyed(R"({"on":"true"})", "on", &v)); // string, not bool
    EXPECT(!jsonGetBoolKeyed(R"({"on":1})", "on", &v));
    EXPECT(!jsonGetBoolKeyed(R"({"off":true})", "on", &v));
    // The word "true" elsewhere in the body must not bleed into the key.
    EXPECT(jsonGetBoolKeyed(R"({"note":"true","on":false})", "on", &v) && !v);
}

static void test_intKeyed() {
    TEST("jsonGetIntKeyed — numeric tokens only");
    long v = 0;
    EXPECT(jsonGetIntKeyed(R"({"n":42})", "n", &v));
    EXPECT_EQ(v, 42L);
    EXPECT(jsonGetIntKeyed(R"({"n": -7 })", "n", &v));
    EXPECT_EQ(v, -7L);
    EXPECT(!jsonGetIntKeyed(R"({"n":"42"})", "n", &v)); // quoted → absent
    EXPECT(!jsonGetIntKeyed(R"({"n":true})", "n", &v));
    EXPECT(!jsonGetIntKeyed(R"({"m":1})", "n", &v));
}

static void test_getObject() {
    TEST("jsonGetObject — nested braces and strings with braces");
    std::string obj = jsonGetObject(R"({"caps":{"rumble":true,"deep":{"x":1}},"z":2})", "caps");
    EXPECT_EQ(obj, std::string(R"({"rumble":true,"deep":{"x":1}})"));
    // Braces inside string values must not unbalance the span.
    obj = jsonGetObject(R"({"o":{"s":"a } b { c","n":1}})", "o");
    EXPECT_EQ(obj, std::string(R"({"s":"a } b { c","n":1})"));
    // Escaped quote inside a string. (Hoisted: see test_valueStart.)
    const char* escapedQuoteOuter = R"({"o":{"s":"q\"}","n":1}})";
    const char* escapedQuoteInner = R"({"s":"q\"}","n":1})";
    obj = jsonGetObject(escapedQuoteOuter, "o");
    EXPECT_EQ(obj, std::string(escapedQuoteInner));
    EXPECT_EQ(jsonGetObject(R"({"o":[1,2]})", "o"), std::string("")); // not an object
    EXPECT_EQ(jsonGetObject(R"({"o":{)", "o"), std::string(""));      // unterminated
    EXPECT_EQ(jsonGetObject("{}", "o"), std::string(""));
}

static void test_getArrayObjects() {
    TEST("jsonGetArrayObjects — splits top-level objects, honours nesting");
    bool present = false;
    auto objs = jsonGetArrayObjects(
        R"({"controllers":[{"ctrlIdx":0,"caps":{"rumble":true}},{"ctrlIdx":1}]})", "controllers",
        &present);
    EXPECT(present);
    EXPECT_EQ(objs.size(), size_t{2});
    if (objs.size() == 2) {
        EXPECT_EQ(objs[0], std::string(R"({"ctrlIdx":0,"caps":{"rumble":true}})"));
        EXPECT_EQ(objs[1], std::string(R"({"ctrlIdx":1})"));
    }

    objs = jsonGetArrayObjects(R"({"controllers":[]})", "controllers", &present);
    EXPECT(present);
    EXPECT_EQ(objs.size(), size_t{0});

    objs = jsonGetArrayObjects(R"({"other":[{}]})", "controllers", &present);
    EXPECT(!present);
    EXPECT_EQ(objs.size(), size_t{0});

    // Not an array → absent, not a crash.
    objs = jsonGetArrayObjects(R"({"controllers":{"a":1}})", "controllers", &present);
    EXPECT(!present);

    // Unterminated array → whatever parsed cleanly, no over-read.
    objs = jsonGetArrayObjects(R"({"controllers":[{"a":1})", "controllers", &present);
    EXPECT_EQ(objs.size(), size_t{0});
}

static void test_arrayObjects_withStringsContainingBrackets() {
    TEST("jsonGetArrayObjects — brackets inside string values don't split objects");
    bool present = false;
    auto objs = jsonGetArrayObjects(R"({"a":[{"s":"x ] } y"},{"t":2}]})", "a", &present);
    EXPECT(present);
    EXPECT_EQ(objs.size(), size_t{2});
}

static void test_structuralSpan_escapes() {
    TEST("jsonStructuralSpan — escape sequences inside strings");
    // (Hoisted: see test_valueStart.)
    const char* backslashBody = R"(  {"s":"\\"} )";
    const char* backslashSpan = R"({"s":"\\"})";
    std::string span = jsonStructuralSpan(backslashBody, 0, '{', '}');
    EXPECT_EQ(span, std::string(backslashSpan));
}

static void test_truncatedBodies() {
    TEST("truncated bodies — read as absent, output untouched, no over-read");
    long n = 7;
    EXPECT(!jsonGetIntKeyed(R"({"n":)", "n", &n));   // EOF right after the colon
    EXPECT(!jsonGetIntKeyed(R"({"n": })", "n", &n)); // no value token
    EXPECT_EQ(n, 7L);
    bool b = false;
    EXPECT(!jsonGetBoolKeyed(R"({"on":tru)", "on", &b)); // truncated literal
    EXPECT(!jsonGetBoolKeyed(R"({"on":)", "on", &b));
    // Escape as the LAST character: the span scanner must stop cleanly.
    // (Hoisted: see test_valueStart.)
    const char* trailingEscape = R"({"o":{"s":"\)";
    EXPECT_EQ(jsonGetObject(trailingEscape, "o"), std::string(""));
}

static void test_nestedAndUnknownKeys() {
    TEST("nested lookup + unknown keys — documented top-level-or-nested tolerance");
    long v = 0;
    // jsonValueStart matches nested keys too (documented; the request parsers
    // only feed it bodies where the key is unambiguous).
    EXPECT(jsonGetIntKeyed(R"({"outer":{"n":9}})", "n", &v));
    EXPECT_EQ(v, 9L);
    // Unknown sibling keys never disturb extraction.
    EXPECT(jsonGetIntKeyed(R"({"future":true,"n":4,"alsoNew":[1]})", "n", &v));
    EXPECT_EQ(v, 4L);
    // Whitespace between key and colon is tolerated.
    size_t pos = 0;
    EXPECT(jsonValueStart(R"({"k" : 1})", "k", pos));
}

static void test_arrayObjects_nestedArrays() {
    TEST("jsonGetArrayObjects — nested arrays inside elements don't split them");
    bool present = false;
    auto objs = jsonGetArrayObjects(R"({"a":[{"x":[1,2]},{"y":3}]})", "a", &present);
    EXPECT(present);
    EXPECT_EQ(objs.size(), size_t{2});
    if (objs.size() == 2) {
        EXPECT_EQ(objs[0], std::string(R"({"x":[1,2]})"));
        EXPECT_EQ(objs[1], std::string(R"({"y":3})"));
    }
}

int main() {
    test_valueStart();
    test_boolKeyed();
    test_intKeyed();
    test_getObject();
    test_getArrayObjects();
    test_arrayObjects_withStringsContainingBrackets();
    test_structuralSpan_escapes();
    test_truncatedBodies();
    test_nestedAndUnknownKeys();
    test_arrayObjects_nestedArrays();

    std::cout << "json_mini: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
