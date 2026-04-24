#!/usr/bin/env bash
# ============================================================================
#  Build Satellite (macOS)
#
#  Virtual-gamepad injection is unavailable on macOS (no signed DriverKit
#  equivalent to ViGEmBus ships here), so the build produces a server that
#  still pairs, authenticates, and reports status to clients but refuses
#  controller-add requests with ACK_ERR_VIGEM_UNAVAIL.
#
#  Prerequisites:
#    - Xcode Command Line Tools
#    - Homebrew: cmake, libsodium, pkg-config
# ============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

BUILD_DIR="${BUILD_DIR:-build}"
BUILD_TYPE="${BUILD_TYPE:-Release}"

echo "[*] Configuring cmake (${BUILD_TYPE}) in ${BUILD_DIR}/"
cmake -S . -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"

echo "[*] Building"
cmake --build "${BUILD_DIR}" --config "${BUILD_TYPE}" -j

echo "[✓] Build complete — ./satellite.app (bundle at repo root)"
