// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

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
