// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include "../src/core/checked_io.h"

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
        satellite::io::stderrWritten(std::fprintf(stderr, "  FAIL [%s] uncaught exception: %s\n",
                                                  g_currentTest.c_str(), e.what()));
    } catch (...) {
        g_fail++;
        satellite::io::stderrWritten(
            std::fprintf(stderr, "  FAIL [%s] uncaught exception\n", g_currentTest.c_str()));
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
        satellite::io::stderrWritten(
            std::fprintf(stderr, "  FAIL removing %ls: error %d\n", dir.c_str(), ec.value()));
#else
        satellite::io::stderrWritten(
            std::fprintf(stderr, "  FAIL removing %s: error %d\n", dir.c_str(), ec.value()));
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

// Both sides bind to the parameters for the whole expression, so a temporary (or a member of
// one) is compared without a copy and without dangling.
namespace satellite::test {
template <typename A, typename B>
void expectEq(const A& a, const B& b, const char* aText, const char* bText, const char* file,
              int line) {
    if (a == b) {
        g_pass++;
        return;
    }
    g_fail++;
    std::cerr << "  FAIL [" << g_currentTest << "] " << file << ":" << line << "  " << aText
              << " == " << bText << "  (got ";
    streamValue(std::cerr, a);
    std::cerr << " vs ";
    streamValue(std::cerr, b);
    std::cerr << ")\n";
}
} // namespace satellite::test

#define EXPECT_EQ(a, b) satellite::test::expectEq((a), (b), #a, #b, __FILE__, __LINE__)
