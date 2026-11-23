#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# Action pin lint: every `uses:` in a workflow names a 40-character commit
# SHA (or is a local ./ path, or a docker:// image by digest), never a tag or
# a branch, and never the all-zero placeholder. The rule _security.yml's
# action-pin-lint job enforces, in one script scripts/ci-local.sh and
# scripts/ci-local.ps1 run.
#
#   scripts/check-action-pins.sh [directory, default .github/workflows]
set -euo pipefail
cd "$(dirname "$0")/.."

dir="${1:-.github/workflows}"
fail=0
while IFS= read -r -d '' file; do
    awk '
        /^[[:space:]]*#/ { next }
        {
            sub(/[[:space:]]+#.*$/, "", $0)
        }
        /^[[:space:]]*-?[[:space:]]*uses:[[:space:]]+[^[:space:]]+/ {
            line = $0
            sub(/^[[:space:]]*-?[[:space:]]*uses:[[:space:]]+/, "", line)
            if (line ~ /^\.\//) { next }
            if (line ~ /^docker:\/\/[^@]+@sha256:[0-9a-f]{64}$/) { next }
            if (line !~ /@[0-9a-f]{40}([[:space:]]|$)/) {
                printf "%s: %s\n", FILENAME, line
                exit 2
            }
            if (line ~ /@0{40}([[:space:]]|$)/) {
                printf "%s: %s (forbidden all-zero placeholder pin)\n", FILENAME, line
                exit 2
            }
        }
    ' "$file" || fail=1
done < <(find "$dir" -type f \( -name '*.yml' -o -name '*.yaml' \) -print0)
if [ "$fail" -ne 0 ]; then
    echo "unpinned action reference; pin the commit: gh api repos/<owner>/<repo>/git/ref/tags/<tag> -q .object.sha" >&2
    exit 1
fi
echo "action pins: OK"
