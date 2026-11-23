#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# Install the Linux / macOS build toolchain for Satellite, matching what CI
# installs so a local build is the build CI runs.
#
#   Linux (apt; the linux-ci.yml package list):
#     build-essential cmake pkg-config
#     libsodium-dev libssl-dev libcurl4-openssl-dev libopus-dev
#     libayatana-appindicator3-dev libgtk-3-dev libnotify-dev   (tray; optional
#     at build time, CMake falls back to a headless build without them)
#     plus python3-venv, which scripts/tools.sh uses to install the pinned
#     clang-format the first time a gate needs it.
#
#   macOS (brew; the macos-ci.yml package list):
#     cmake pkg-config libsodium opus
#     (the pinned clang-format comes from scripts/tools.sh on first use)
#
# On Windows use scripts/install-deps.ps1 instead.
set -euo pipefail

step() { echo ""; echo "=== $1 ==="; }

case "$(uname -s)" in
Linux)
    if ! command -v apt-get >/dev/null 2>&1; then
        echo "This script drives apt (Debian/Ubuntu, what CI runs). On another distro install:" >&2
        echo "  cmake, g++, pkg-config, libsodium, openssl, libcurl and opus development headers," >&2
        echo "  optionally libayatana-appindicator + GTK3 + libnotify for the tray," >&2
        echo "  and python3 with venv (scripts/tools.sh installs the pinned clang-format)." >&2
        exit 1
    fi

    step "apt packages (the linux-ci.yml list)"
    sudo apt-get update
    sudo apt-get install -y --no-install-recommends \
        build-essential cmake pkg-config \
        libsodium-dev libssl-dev libcurl4-openssl-dev libopus-dev \
        libayatana-appindicator3-dev libgtk-3-dev libnotify-dev \
        python3-venv

    ;;

Darwin)
    if ! command -v brew >/dev/null 2>&1; then
        echo "Homebrew is required (https://brew.sh), plus the Xcode CLT (xcode-select --install)." >&2
        exit 1
    fi

    step "brew packages (the macos-ci.yml list)"
    brew install cmake pkg-config libsodium opus

    ;;

*)
    echo "unsupported platform $(uname -s); on Windows use scripts/install-deps.ps1" >&2
    exit 1
    ;;
esac

echo ""
echo "=== Done ==="
echo ""
echo "Next steps:"
echo "  1. Build:   scripts/build.sh"
echo "  2. Test:    scripts/build.sh release test"
echo "  3. CI parity before pushing:  scripts/ci-local.sh"
