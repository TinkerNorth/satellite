// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>
#include <ostream>
#include <string>
#include <type_traits>
#include <utility>

static int g_pass = 0;
static int g_fail = 0;
static std::string g_currentTest;

namespace satellite::test {

template <typename T, typename = void> struct IsStreamable : std::false_type {};

template <typename T>
struct IsStreamable<
    T, std::void_t<decltype(std::declval<std::ostream&>() << std::declval<const T&>())>>
    : std::true_type {};

template <typename T> inline void streamValue(std::ostream& os, const T& value) {
    if constexpr (IsStreamable<T>::value) {
        using U = std::remove_cv_t<std::remove_reference_t<T>>;
        if constexpr (std::is_same_v<U, char> || std::is_same_v<U, signed char> ||
                      std::is_same_v<U, unsigned char>) {
            os << +value;
        } else {
            os << value;
        }
    } else {
        os << "?";
    }
}

// Every test binary's main is this boundary: a body that throws is a failed run, said so with
// the test that was running, rather than std::terminate with no word of where. A body that
// returns an int returns non-zero when its setup failed, and that is the exit code.
template <typename Body> int runMain(const char* name, Body&& body) noexcept {
    try {
        if constexpr (std::is_same_v<decltype(body()), int>) {
            const int setup = body();
            if (setup != 0) return setup;
        } else {
            body();
        }
    } catch (const std::exception& e) {
        g_fail++;
        std::fprintf(stderr, "  FAIL [%s] uncaught exception: %s\n", g_currentTest.c_str(),
                     e.what());
    } catch (...) {
        g_fail++;
        std::fprintf(stderr, "  FAIL [%s] uncaught exception\n", g_currentTest.c_str());
    }
    std::printf("%s: %d passed, %d failed\n", name, g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

// Removes a test's temporary directory; one that cannot be removed is a failed test. It never
// throws, so a destructor can call it: the path is built before, never here.
inline void removeTestDir(const std::filesystem::path& dir) noexcept {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    if (ec) {
        g_fail++;
#ifdef _WIN32
        std::fprintf(stderr, "  FAIL removing %ls: error %d\n", dir.c_str(), ec.value());
#else
        std::fprintf(stderr, "  FAIL removing %s: error %d\n", dir.c_str(), ec.value());
#endif
    }
}

} // namespace satellite::test

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
                      << "  " << #a << " == " << #b << "  (got ";                                  \
            satellite::test::streamValue(std::cerr, _a);                                           \
            std::cerr << " vs ";                                                                   \
            satellite::test::streamValue(std::cerr, _b);                                           \
            std::cerr << ")\n";                                                                    \
        }                                                                                          \
    } while (0)
