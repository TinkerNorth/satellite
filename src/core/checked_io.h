// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

// The C stdio calls whose result clang-tidy's cert-err33-c asks to be read,
// read once here with the answer each case has.

#include <cstddef>
#include <cstdio>

namespace satellite::io {

// fprintf(stderr, ...)'s result, for the lines written before a logger exists
// or about the logger itself. When stderr cannot be written there is no
// channel left to report that on, so the stream's error is cleared and the
// next line is still attempted.
inline void stderrWritten(int result) noexcept {
    if (result < 0) std::clearerr(stderr);
}

// The same answer for another stream a tool writes its report to (fputs,
// fflush): an error is cleared so the next write is still attempted.
inline void written(std::FILE* stream, int result) noexcept {
    if (result < 0) std::clearerr(stream);
}

// snprintf's result against the buffer it wrote: true when the whole text
// fits. False when it was cut at the buffer's end, or could not be encoded at
// all, in which case the buffer is emptied so nobody reads what is left in it.
inline bool formatted(int result, char* buf, std::size_t capacity) noexcept {
    if (result < 0) {
        if (capacity > 0) buf[0] = '\0';
        return false;
    }
    return static_cast<std::size_t>(result) < capacity;
}

template <std::size_t N> bool formatted(int result, char (&buf)[N]) noexcept {
    return formatted(result, buf, N);
}

} // namespace satellite::io
