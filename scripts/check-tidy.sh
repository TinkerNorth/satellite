#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# clang-tidy gate: the pinned clang-tidy (scripts/tools.sh) with the shared
# .clang-tidy over every first-party translation unit a build configured,
# every finding an error. linux-ci.yml, macos-ci.yml and windows-ci.yml run it
# over their own configure, so each platform's sources are checked by the
# lane that compiles them; scripts/ci-local.sh runs it over this machine's.
#
#   scripts/check-tidy.sh <build dir with compile_commands.json>
set -euo pipefail
cd "$(dirname "$0")/.."

build="${1:?usage: check-tidy.sh <build dir with compile_commands.json>}"
db="${build}/compile_commands.json"
[ -f "${db}" ] || { echo "check-tidy: ${db} is missing; configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON" >&2; exit 1; }

# shellcheck source=scripts/tools.sh
source scripts/tools.sh
fetch_clang_tidy
got="$(clang-tidy --version | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)"
if [ "${got}" != "${CLANG_TIDY_VERSION}" ]; then
    echo "clang-tidy on PATH is ${got}, scripts/tools.sh pins ${CLANG_TIDY_VERSION}" >&2
    exit 1
fi

extra=()
if [ "$(uname -s)" = Darwin ]; then
    # The pip wheel's clang does not know where Xcode keeps the SDK.
    extra=(--extra-arg=-isysroot --extra-arg="$(xcrun --show-sdk-path)")
fi

python=python3
command -v python3 >/dev/null 2>&1 || python=python
files="$("${python}" - "${db}" <<'PY'
import json, os, sys
root = os.getcwd()
seen = set()
for entry in json.load(open(sys.argv[1])):
    path = os.path.normpath(os.path.join(entry["directory"], entry["file"]))
    rel = os.path.relpath(path, root)
    if rel.split(os.sep)[0] in ("src", "tests", "tools") and rel not in seen:
        seen.add(rel)
        print(rel)
PY
)"
count="$(printf '%s\n' "${files}" | grep -c .)"
jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
echo "clang-tidy ${got} over ${count} translation units (${jobs} at a time)"
printf '%s\n' "${files}" | xargs -P "${jobs}" -n 1 \
    clang-tidy -p "${build}" --quiet --warnings-as-errors='*' "${extra[@]+"${extra[@]}"}"
echo "clang-tidy ${got}: OK"
