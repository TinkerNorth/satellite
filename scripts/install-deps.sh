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
#     plus clang-format 22.1.4 via pipx, the same pin linux-ci.yml uses.
#
#   macOS (brew; the macos-ci.yml package list):
#     cmake pkg-config libsodium opus
#     plus clang-format 22.1.4 in a venv, the same mechanism macos-ci.yml uses.
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
        echo "  and clang-format 22.1.4 (pipx install clang-format==22.1.4)." >&2
        exit 1
    fi

    step "apt packages (the linux-ci.yml list)"
    sudo apt-get update
    sudo apt-get install -y --no-install-recommends \
        build-essential cmake pkg-config \
        libsodium-dev libssl-dev libcurl4-openssl-dev libopus-dev \
        libayatana-appindicator3-dev libgtk-3-dev libnotify-dev \
        pipx

    step "clang-format 22.1.4 (pipx; the pin every CI lane uses)"
    pipx install clang-format==22.1.4 || pipx upgrade clang-format || true
    pipx ensurepath
    if ! command -v clang-format >/dev/null 2>&1; then
        echo "[NOTE] clang-format installed via pipx; open a new shell (pipx ensurepath) to pick it up."
    fi
    ;;

Darwin)
    if ! command -v brew >/dev/null 2>&1; then
        echo "Homebrew is required (https://brew.sh), plus the Xcode CLT (xcode-select --install)." >&2
        exit 1
    fi

    step "brew packages (the macos-ci.yml list)"
    brew install cmake pkg-config libsodium opus

    step "clang-format 22.1.4 (venv; the mechanism macos-ci.yml uses)"
    venv="$HOME/.clang-format-venv"
    python3 -m venv "$venv"
    "$venv/bin/pip" install --quiet 'clang-format==22.1.4'
    echo "[NOTE] add $venv/bin to PATH (e.g. in ~/.zshrc) so the pinned clang-format wins:"
    echo "       export PATH=\"$venv/bin:\$PATH\""
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
