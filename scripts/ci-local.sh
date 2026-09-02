#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# Runs the gates Linux / macOS CI runs, in the same order, against the local
# tree, so a green run here means a green run there. Mirrors linux-ci.yml /
# macos-ci.yml (branching on uname); the Windows equivalent is
# scripts/ci-local.ps1.
#
#   scripts/ci-local.sh                    every gate
#   scripts/ci-local.sh --allow-missing    downgrade a missing tool to a notice
#
# Without --allow-missing a gate whose tool is absent FAILS rather than
# printing a notice and continuing: a "green" run that silently skipped a gate
# is worse than no run at all.
#
# CI-only legs intentionally not mirrored: the root uinput smoke test, the
# tray-link assertion (needs the runner's package set), the fuzz and
# reproducibility jobs.
set -euo pipefail
cd "$(dirname "$0")/.."

ALLOW_MISSING=0
for arg in "$@"; do
  case "$arg" in
    --allow-missing|--allow-missing-tools) ALLOW_MISSING=1 ;;
    *) echo "unknown flag: $arg" >&2; exit 2 ;;
  esac
done

step() { echo ""; echo "=== $1 ==="; }

# Returns 0 when the caller should run the gate, 1 when it was skipped by
# permission, and exits when a tool CI gates on is missing.
have() {
  local tool="$1"
  if command -v "$tool" >/dev/null 2>&1; then return 0; fi
  if [ "$ALLOW_MISSING" -eq 1 ]; then
    echo "::notice:: $tool is not installed; CI gates this. Skipping (--allow-missing)."
    return 1
  fi
  echo "$tool is not installed and CI gates it. Install it (scripts/install-deps.sh), or re-run with --allow-missing." >&2
  exit 1
}

case "$(uname -s)" in
  Darwin) lane="macos"; jobs="$(sysctl -n hw.ncpu)" ;;
  Linux)  lane="linux"; jobs="$(nproc)" ;;
  *)      echo "unsupported platform $(uname -s); use scripts/ci-local.ps1 on Windows" >&2; exit 1 ;;
esac

step "clang-format (check only)"
if have clang-format; then
  # CI pins 22.1.4; other versions disagree on braced-init lists, which is why
  # the pin exists. Treat a surprise from another version with suspicion.
  want=22.1.4
  got="$(clang-format --version | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)"
  if [ "$got" != "$want" ]; then
    echo "::notice:: clang-format $got, CI pins $want; disagreements may be the version, not the code."
  fi
  bash scripts/check-format.sh
fi

step "Action pin lint (40-char SHA required)"
# The same awk _security.yml runs, over the same directory.
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
done < <(find .github/workflows -type f \( -name '*.yml' -o -name '*.yaml' \) -print0)
[ "$fail" -eq 0 ] || { echo "unpinned action reference" >&2; exit 1; }
echo "action pins: OK"

step "Core purity gate"
# linux-ci.yml runs this; it is compiler- and platform-independent (plain
# bash 3.2 + BSD/GNU grep/sed), so run it on macOS too.
bash scripts/check_core_purity.sh

step "Configure (Release, preset ${lane})"
cmake --preset "${lane}"

step "Build"
cmake --build --preset "${lane}" -j "${jobs}"

step "Run tests"
ctest --preset "${lane}"

echo ""
echo "All local CI gates passed."
