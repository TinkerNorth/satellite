#!/usr/bin/env bash
# Thin forwarder kept for compatibility: the build logic lives in
# scripts/build.sh (which drives the CMake presets in CMakePresets.json,
# the same presets CI runs). Usage there: scripts/build.sh [debug|release] [test]
exec "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/scripts/build.sh" "$@"
