#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# Build a Debian package (.deb) for satellite.
#
# Output: ./dist/satellite_<version>_<arch>.deb
# Install with: sudo apt install ./dist/satellite_*.deb
#
# The cpack invocation matches release.yml's deb job (`cpack -G DEB
# --config <build>/CPackConfig.cmake -B <staging>`) so the local package and
# the released one come off the same rails. One deliberate difference:
# release.yml builds inside a debian:trixie container so dpkg-shlibdeps
# computes Depends against Debian's own sonames; a package built on another
# distro proves the CPack wiring and install layout but its Depends line is
# that distro's, not Debian's.
#
# Prerequisites (Debian/Ubuntu): scripts/install-deps.sh plus dpkg-dev.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "build-deb.sh: only supported on Linux (got $(uname -s))." >&2
    exit 1
fi

if ! command -v dpkg-shlibdeps >/dev/null 2>&1; then
    echo "build-deb.sh: dpkg-dev is required (provides dpkg-shlibdeps)." >&2
    echo "             Install with: sudo apt install dpkg-dev" >&2
    exit 1
fi

BUILD_DIR="${BUILD_DIR:-build-deb}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
DIST_DIR="${DIST_DIR:-dist}"
STAGING_DIR="${BUILD_DIR}/cpack"

echo "[*] Configuring cmake (${BUILD_TYPE}) in ${BUILD_DIR}/"
cmake -S . -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"

echo "[*] Building"
cmake --build "${BUILD_DIR}" --config "${BUILD_TYPE}" --target satellite -j "$(nproc)"

echo "[*] Packaging (cpack -G DEB, CI's invocation)"
mkdir -p "${DIST_DIR}"
cpack -G DEB --config "${BUILD_DIR}/CPackConfig.cmake" -B "${STAGING_DIR}"
# CI's glob is satellite_*_amd64.deb; stay arch-neutral here so an arm64 box
# can still package itself.
cp -f "${STAGING_DIR}"/satellite_*.deb "${DIST_DIR}/"

echo ""
echo "[OK] Package built:"
ls -lh "${DIST_DIR}"/satellite_*.deb
echo ""
echo "    Install with:  sudo apt install ./${DIST_DIR}/satellite_*.deb"
echo "    Remove with:   sudo apt remove satellite"
