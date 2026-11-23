#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# Runs the gates Linux and macOS CI run, in the same order and through the
# same scripts, against the local tree, so a green run here means a green run
# there. Mirrors linux-ci.yml and macos-ci.yml (branching on uname) and
# version-consistency.yml; the Windows equivalent is scripts/ci-local.ps1.
#
#   scripts/ci-local.sh                    every gate the main CI job runs
#   scripts/ci-local.sh --quick            format, action pins, version
#                                          consistency, vendored freshness,
#                                          core purity: no build
#   scripts/ci-local.sh --with-packages    also the .deb and .rpm lanes, in
#                                          debian:trixie and fedora:44 (docker)
#   scripts/ci-local.sh --with-appimage    also the AppImage lane, in
#                                          ubuntu:22.04 (docker)
#   scripts/ci-local.sh --with-bundle      macOS: also the release bundle lane
#                                          (scripts/package-macos.sh, unsigned)
#   scripts/ci-local.sh --allow-missing    downgrade a missing tool to a notice
#
# Without --allow-missing a gate whose tool is absent FAILS rather than
# printing a notice and continuing: a green run that silently skipped a gate
# is worse than no run at all.
#
# CI-only, with the reason:
#   uinput smoke as root, AppIndicator link check   need the runner's kernel
#                                                   module and package set
#   headless build, fuzz smoke, reproducibility,    separate linux-ci.yml jobs
#   Sentry SDK build                                with their own toolchains
#   CodeQL, Security (OSV, gitleaks, dependency     need GitHub's services
#   review), measurements
#   Windows lanes                                   scripts/ci-local.ps1
set -euo pipefail
cd "$(dirname "$0")/.."

ALLOW_MISSING=0
QUICK=0
WITH_PACKAGES=0
WITH_APPIMAGE=0
WITH_BUNDLE=0
for arg in "$@"; do
  case "$arg" in
    --allow-missing|--allow-missing-tools) ALLOW_MISSING=1 ;;
    --quick) QUICK=1 ;;
    --with-packages) WITH_PACKAGES=1 ;;
    --with-appimage) WITH_APPIMAGE=1 ;;
    --with-bundle) WITH_BUNDLE=1 ;;
    -h|--help) sed -n '2,31p' "$0"; exit 0 ;;
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
  Darwin) jobs="$(sysctl -n hw.ncpu)"; os="macos" ;;
  Linux)  jobs="$(nproc)"; os="linux" ;;
  *)      echo "unsupported platform $(uname -s); use scripts/ci-local.ps1 on Windows" >&2; exit 1 ;;
esac

step "clang-format (check only)"
# scripts/check-format.sh installs the clang-format scripts/tools.sh pins
# (needs python3 with venv), the one every CI lane uses.
if have python3; then
  bash scripts/check-format.sh
fi

step "Action pin lint (40-char SHA required)"
bash scripts/check-action-pins.sh

step "Version consistency"
bash scripts/check-version-consistency.sh

step "Vendored components <= 90 days old (security.yml)"
bash scripts/check-vendored-freshness.sh
echo "vendored freshness: OK"

step "Core purity gate"
bash scripts/check_core_purity.sh

if [ "$QUICK" -eq 1 ]; then
  echo ""
  echo "Quick gates passed (no build: drop --quick for the full run)."
  exit 0
fi

if have cmake; then
  step "Configure (preset release)"
  cmake --preset release

  step "Build"
  cmake --build --preset release -j "${jobs}"

  step "Run tests"
  ctest --preset release

  if [ "$os" = "macos" ]; then
    step "Bundle layout and smoke (macos-ci.yml)"
    test -x satellite.app/Contents/MacOS/satellite
    test -f satellite.app/Contents/Resources/web/index.html
    bash scripts/smoke-alive.sh satellite.app/Contents/MacOS/satellite 45
  fi
fi

if [ "$WITH_BUNDLE" -eq 1 ]; then
  if [ "$os" != "macos" ]; then
    echo "--with-bundle builds the macOS bundle; it needs macOS" >&2
    exit 2
  fi
  step "macOS release bundle, unsigned (macos-ci.yml bundle job)"
  bash scripts/package-macos.sh build
  bash scripts/package-macos.sh stage "$(tr -d '[:space:]' < VERSION)-dev" "-unsigned"
  bash scripts/package-macos.sh check
fi

for lane in deb rpm; do
  if [ "$WITH_PACKAGES" -eq 1 ] && have docker; then
    step "Package lane ${lane} in its container (linux-ci.yml ${lane} job)"
    bash scripts/package-in-container.sh "${lane}"
  fi
done

if [ "$WITH_APPIMAGE" -eq 1 ] && have docker; then
  step "AppImage lane in ubuntu:22.04 (linux-ci.yml appimage job)"
  bash scripts/package-in-container.sh appimage
fi

echo ""
echo "All local CI gates passed."
