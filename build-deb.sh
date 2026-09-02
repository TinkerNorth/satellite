#!/usr/bin/env bash
# Thin forwarder kept for compatibility: the .deb packaging lives in
# scripts/build-deb.sh (whose cpack invocation matches release.yml's deb job).
exec "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/scripts/build-deb.sh" "$@"
