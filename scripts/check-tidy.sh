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
# clang reads the build's own compile database, with two things only gcc
# understands made clang's: -flto=<jobs> (clang takes -flto), and, for a MinGW
# gcc, the target and the MSYS2 prefix it lives in, so clang parses against
# the same MinGW headers and libstdc++ the build compiled with rather than the
# runner's MSVC ones. The rewritten database goes in
# <build>/tidy; the list of first-party translation units comes back.
tidydb="${build}/tidy"
mkdir -p "${tidydb}"
files="$("${python}" - "${db}" "${tidydb}/compile_commands.json" <<'PY'
import json, os, re, shlex, sys
root = os.getcwd()
seen = set()
kept = []
for entry in json.load(open(sys.argv[1])):
    path = os.path.normpath(os.path.join(entry["directory"], entry["file"]))
    rel = os.path.relpath(path, root)
    if rel.split(os.sep)[0] not in ("src", "tests", "tools"):
        continue
    args = entry.get("arguments") or shlex.split(entry["command"])
    args = ["-flto" if re.fullmatch(r"-flto=.*", a) else a for a in args]
    compiler = args[0].replace("\\", "/")
    if "mingw" in compiler.lower():
        sysroot = os.path.normpath(os.path.join(os.path.dirname(compiler), ".."))
        args = args[:1] + ["--target=x86_64-w64-mingw32", "--sysroot=" + sysroot] + args[1:]
    kept.append({"directory": entry["directory"], "file": entry["file"], "arguments": args})
    if rel not in seen:
        seen.add(rel)
        print(rel.replace(os.sep, "/"))
json.dump(kept, open(sys.argv[2], "w"), indent=1)
PY
)"
count="$(printf '%s\n' "${files}" | grep -c .)"
jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
echo "clang-tidy ${got} over ${count} translation units (${jobs} at a time)"
# TIDY_EXPORT_FIXES=<dir> also writes each unit's suggested fixes there, as
# clang-apply-replacements reads them.
if [ -n "${TIDY_EXPORT_FIXES:-}" ]; then
    mkdir -p "${TIDY_EXPORT_FIXES}"
    export TIDY_EXPORT_FIXES TIDYDB="${tidydb}" TIDY_EXTRA="${extra[*]+"${extra[*]}"}"
    printf '%s\n' "${files}" | tr '\n' '\0' | xargs -0 -P "${jobs}" -n 1 sh -c \
        'clang-tidy -p "${TIDYDB}" --quiet --warnings-as-errors="*" --export-fixes="${TIDY_EXPORT_FIXES}/$(echo "$1" | tr / _).yaml" ${TIDY_EXTRA} "$1"' _
else
    printf '%s\n' "${files}" | tr '\n' '\0' | xargs -0 -P "${jobs}" -n 1 \
        clang-tidy -p "${tidydb}" --quiet --warnings-as-errors='*' "${extra[@]+"${extra[@]}"}"
fi
echo "clang-tidy ${got}: OK"
