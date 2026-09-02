#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# Build Satellite on Linux / macOS: scripts/build.sh [debug|release] [test|clean]
#
# Thin wrapper over the CMake presets in CMakePresets.json, which are the
# single source of configure truth (the same presets linux-ci.yml and
# macos-ci.yml run, warnings-as-errors included). Same argument shape as the
# dish repos' scripts/build.sh.
#
#   scripts/build.sh                # Release (linux/macos preset, by uname)
#   scripts/build.sh debug          # Debug into build-debug/
#   scripts/build.sh release test   # Release build, then ctest
#   scripts/build.sh clean          # wipe the preset build directories
#
# Run scripts/install-deps.sh once to install the toolchain CI uses.
# Output: ./satellite (Linux) or ./satellite.app (macOS) at the repo root.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

config="release"
run_tests=0

for arg in "$@"; do
    case "${arg}" in
        debug|Debug)     config="debug" ;;
        release|Release) config="release" ;;
        test|tests)      run_tests=1 ;;
        clean)
            rm -rf build build-debug
            echo "removed build directories"
            exit 0
            ;;
        -h|--help)
            sed -n '2,18p' "${BASH_SOURCE[0]}"
            exit 0
            ;;
        *)
            echo "unknown argument: ${arg}" >&2
            exit 1
            ;;
    esac
done

case "$(uname -s)" in
    Darwin) lane="macos" ;;
    Linux)  lane="linux" ;;
    *)      echo "unsupported platform $(uname -s); on Windows use scripts/build.ps1" >&2; exit 1 ;;
esac
preset="${lane}"
if [ "${config}" = "debug" ]; then
    preset="${lane}-debug"
fi

if [ "$(uname -s)" = "Darwin" ]; then
    jobs="$(sysctl -n hw.ncpu)"
else
    jobs="$(nproc)"
fi

cmake --preset "${preset}"
cmake --build --preset "${preset}" -j "${jobs}"

if [ "${run_tests}" -eq 1 ]; then
    ctest --preset "${preset}"
fi

echo ""
case "$(uname -s)" in
    Darwin) echo "Build complete (preset ${preset}): ./satellite.app at the repo root" ;;
    *)      echo "Build complete (preset ${preset}): ./satellite at the repo root" ;;
esac
