#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Build dish_bench with the repo's ucrt64 g++ toolchain.
# Run from anywhere; paths are resolved relative to the satellite repo root.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"      # satellite/
SODIUM="$ROOT/lib/libsodium/libsodium-win64"

CXX="${CXX:-g++}"

"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -DNDEBUG -DSODIUM_STATIC=1 -D_WIN32_WINNT=0x0A00 \
    -I "$ROOT/src" \
    -I "$SODIUM/include" \
    "$HERE/dish_bench.cpp" \
    "$ROOT/src/net/session_crypto.cpp" \
    "$SODIUM/lib/libsodium.a" \
    -lwinhttp -lws2_32 -lxinput -lwinmm \
    -o "$HERE/dish_bench.exe"

echo "built: $HERE/dish_bench.exe"
