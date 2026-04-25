#!/usr/bin/env bash
# ============================================================================
#  Build Satellite (macOS / Linux)
#
#  macOS:
#    Virtual-gamepad injection is unavailable (no signed DriverKit equivalent
#    to ViGEmBus ships here), so the build produces a server that still
#    pairs, authenticates, and reports status but refuses controller-add
#    requests with ACK_ERR_VIGEM_UNAVAIL.
#    Prerequisites: Xcode CLT; Homebrew: cmake, libsodium, pkg-config.
#
#  Linux:
#    Virtual gamepads are synthesized via /dev/uinput. The uinput kernel
#    module ships with every mainline kernel; the running user must have
#    write access to /dev/uinput (typically via `sudo modprobe uinput` and
#    a udev rule or group membership).
#
#    Prerequisites:
#      - cmake, g++, pkg-config
#      - libsodium development headers
#          Debian/Ubuntu: libsodium-dev
#          Fedora/RHEL:   libsodium-devel
#          Arch:          libsodium
#      - libayatana-appindicator + GTK3 development headers (optional —
#        without these the binary builds headless / no tray icon)
#          Debian/Ubuntu: libayatana-appindicator3-dev libgtk-3-dev
#          Fedora/RHEL:   libayatana-appindicator-gtk3-devel gtk3-devel
#          Arch:          libayatana-appindicator gtk3
#
#    Note for vanilla GNOME users: the desktop has no built-in tray. Install
#    the "AppIndicator and KStatusNotifierItem Support" extension from
#    https://extensions.gnome.org/extension/615/appindicator-support/ to see
#    the icon. KDE, XFCE, Cinnamon, MATE, Budgie, and Pantheon work
#    out of the box.
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

case "$(uname -s)" in
    Darwin) echo "[✓] Build complete — ./satellite.app (bundle at repo root)" ;;
    Linux)  echo "[✓] Build complete — ./satellite (binary at repo root)" ;;
    *)      echo "[✓] Build complete" ;;
esac
