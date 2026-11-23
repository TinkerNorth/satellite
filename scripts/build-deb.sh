#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# Build a Debian package for satellite: the `package` preset, then cpack.
#
# Output: ./dist/satellite_<version>_<arch>.deb
#
# CI builds inside a debian:trixie container (linux-ci.yml's deb job on every
# pull request, release.yml's on a tag) so dpkg-shlibdeps computes Depends
# against Debian's own sonames; a package built on another distro proves the
# CPack wiring and install layout, but its Depends line is that distro's.
# scripts/package-in-container.sh deb runs the CI lane locally.
#
#   SATELLITE_VERSION=1.2.3 scripts/build-deb.sh   # the release passes the tag
#
# SATELLITE_SENTRY_DSN is empty unless release.yml exported it from the
# repository secret, so a hand-run produces a build that cannot transmit.
#
# Prerequisites (Debian/Ubuntu): scripts/install-package-deps.sh deb.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "build-deb.sh: only supported on Linux (got $(uname -s))." >&2
    exit 1
fi
if ! command -v dpkg-shlibdeps >/dev/null 2>&1; then
    echo "build-deb.sh: dpkg-dev is required (provides dpkg-shlibdeps)." >&2
    exit 1
fi

dist_dir="${DIST_DIR:-dist}"

# Only the release names a version: without one the build is labelled
# <VERSION>-dev, so a pull-request or hand-built package never files itself
# under a real release.
release_args=()
if [ -n "${SATELLITE_VERSION:-}" ]; then
    release_args=(-DSATELLITE_RELEASE_VERSION="${SATELLITE_VERSION#v}")
fi

echo "=== Configure (preset package) ==="
cmake --preset package "${release_args[@]}" -DSATELLITE_SENTRY_DSN="${SATELLITE_SENTRY_DSN:-}"

echo "=== Build ==="
cmake --build --preset package --target satellite -j "$(nproc)"

echo "=== Package (cpack -G DEB) ==="
mkdir -p "${dist_dir}"
rm -rf build-package/cpack
cpack -G DEB --config build-package/CPackConfig.cmake -B build-package/cpack
cp -f build-package/cpack/satellite_*.deb "${dist_dir}/"
ls -l "${dist_dir}"/satellite_*.deb
