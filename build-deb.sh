#!/usr/bin/env bash
# ============================================================================
#  Build a Debian package (.deb) for satellite.
#
#  Output: ./dist/satellite_<version>_<arch>.deb
#
#  Install with:
#      sudo apt install ./dist/satellite_*.deb
#
#  The package installs:
#      /usr/bin/satellite
#      /usr/share/satellite/web/...
#      /usr/share/applications/satellite.desktop
#      /usr/share/pixmaps/satellite.png
#      /usr/lib/udev/rules.d/70-satellite-uinput.rules
#      /usr/lib/modules-load.d/satellite.conf
#
#  Postinst loads uinput, reloads udev, and adds $SUDO_USER to the `input`
#  group so the user has /dev/uinput access without further setup.
#
#  Prerequisites (Debian/Ubuntu):
#      sudo apt install build-essential cmake pkg-config dpkg-dev \
#                       libsodium-dev \
#                       libayatana-appindicator3-dev libgtk-3-dev
# ============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

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

echo "[*] Configuring cmake (${BUILD_TYPE}) in ${BUILD_DIR}/"
cmake -S . -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"

echo "[*] Building"
cmake --build "${BUILD_DIR}" --config "${BUILD_TYPE}" -j

echo "[*] Packaging (cpack -G DEB)"
mkdir -p "${DIST_DIR}"
( cd "${BUILD_DIR}" && cpack -G DEB )

mv -f "${BUILD_DIR}"/satellite_*.deb "${DIST_DIR}/"

echo ""
echo "[✓] Package built:"
ls -lh "${DIST_DIR}"/satellite_*.deb
echo ""
echo "    Install with:  sudo apt install ./${DIST_DIR}/satellite_*.deb"
echo "    Remove with:   sudo apt remove satellite"
