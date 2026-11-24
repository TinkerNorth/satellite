#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# clang-format gate: the exact file set and invocation every CI lane runs
# (linux-ci.yml, macos-ci.yml, windows-ci.yml under git-bash) and both local
# scripts (scripts/ci-local.sh, and scripts/ci-local.ps1 through bash). It
# formats with the clang-format scripts/tools.sh pins, fetched on first use,
# so a verdict never depends on which clang-format a machine has on PATH.
#
#   scripts/check-format.sh          check only (what CI runs)
#   scripts/check-format.sh --fix    rewrite the files in place
set -euo pipefail
cd "$(dirname "$0")/.."

mode=(--dry-run --Werror)
case "${1:-}" in
    "") ;;
    --fix) mode=(-i) ;;
    *)
        echo "usage: scripts/check-format.sh [--fix]" >&2
        exit 2
        ;;
esac

# shellcheck source=scripts/tools.sh
source scripts/tools.sh
fetch_clang_format
got="$(clang-format --version | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)"
if [ "${got}" != "${CLANG_FORMAT_VERSION}" ]; then
    echo "clang-format on PATH is ${got}, scripts/tools.sh pins ${CLANG_FORMAT_VERSION}" >&2
    exit 1
fi

find src tests -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.mm' \) -print0 \
    | xargs -0 clang-format "${mode[@]}"
echo "clang-format ${got}: OK"
