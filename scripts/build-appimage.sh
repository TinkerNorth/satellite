#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# Build stage/satellite-<version>-x86_64.AppImage (+ .zsync + the stable-name
# copy stage/satellite-x86_64.AppImage that /releases/latest/download/ serves).
#
# This is the recipe release.yml's appimage job runs; the workflow calls this
# script so a local AppImage and the released one are the same bytes-producing
# path (the dish-linux scripts/build-appimage.sh model). Run it on the oldest
# glibc you support (CI: ubuntu-22.04, glibc 2.35): an AppImage's floor is the
# glibc it linked against.
#
#   scripts/build-appimage.sh
#   SATELLITE_VERSION=1.2.3 scripts/build-appimage.sh   # CI passes the tag
#
# Needs the Linux build deps (scripts/install-deps.sh) plus wget, file and
# ImageMagick (convert).
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "build-appimage.sh: only supported on Linux (got $(uname -s))." >&2
    exit 1
fi

version="${SATELLITE_VERSION:-}"
if [ -z "${version}" ]; then
    version="$(tr -d '[:space:]' < VERSION)"
fi
version="${version#v}"

build_dir="${BUILD_DIR:-build-appimage}"
tmp_dir="${RUNNER_TEMP:-$(mktemp -d)}"
appdir="${tmp_dir}/Satellite.AppDir"

echo "==> satellite ${version} AppImage (x86_64)"

# SATELLITE_SENTRY_DSN is empty unless release.yml exported it from the
# repository secret, so a hand-run of this script still produces a build
# that cannot transmit, even though it stamps a release version.
cmake -S . -B "${build_dir}" -DCMAKE_BUILD_TYPE=Release -DSATELLITE_RELEASE_VERSION="${version}" -DSATELLITE_SENTRY_DSN="${SATELLITE_SENTRY_DSN:-}"
cmake --build "${build_dir}" --config Release --target satellite -j "$(nproc)"

wget -q https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage -O linuxdeploy
chmod +x linuxdeploy

mkdir -p "${appdir}/usr/share/applications" "${appdir}/usr/share/icons/hicolor/256x256/apps"
convert web/img/satellite-icon.png -resize 256x256 "${tmp_dir}/satellite.png"

# Embed AppImageUpdate metadata so AppImageUpdate / Gear Lever can
# delta-update off the newest release; appimagetool emits the matching
# .zsync, and release.yml's draft-then-flip publish keeps `latest` atomic.
export LDAI_UPDATE_INFORMATION="gh-releases-zsync|TinkerNorth|satellite|latest|satellite-*-x86_64.AppImage.zsync"

mkdir -p stage
out="stage/satellite-${version}-x86_64.AppImage"
OUTPUT="${out}" ./linuxdeploy --appimage-extract-and-run --appdir "${appdir}" \
    --executable satellite \
    --desktop-file packaging/debian/satellite.desktop \
    --icon-file "${tmp_dir}/satellite.png" \
    --output appimage

# appimagetool drops the .zsync next to the AppImage or in the CWD depending
# on version; normalise into stage/.
if [ ! -f "${out}.zsync" ] && [ -f "$(basename "${out}").zsync" ]; then
    mv "$(basename "${out}").zsync" "${out}.zsync"
fi
[ -f "${out}" ] || { echo "::error::linuxdeploy produced no AppImage"; exit 1; }
if [ ! -f "${out}.zsync" ]; then
    echo "::warning::no .zsync emitted; AppImageUpdate delta updates unavailable for this build"
fi

# Stable-name copy for the /releases/latest/download/ permalink.
# No .zsync alias: AppImageUpdate matches the versioned pattern.
cp "${out}" stage/satellite-x86_64.AppImage
ls -l stage/
