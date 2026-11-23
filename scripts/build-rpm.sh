#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# Build an RPM package for satellite: the `package` preset, then cpack.
#
# Output: ./dist/satellite-<version>-1.<arch>.rpm
#
# The rails scripts/build-deb.sh runs, with the generator swapped. CI builds
# inside a fedora:44 container (linux-ci.yml's rpm job on every pull request,
# release.yml's on a tag) so rpmbuild's soname scanner writes the Requires
# against Fedora's packages; a run on another distro proves the CPack wiring
# and the layout, not the Requires. scripts/package-in-container.sh rpm runs
# the CI lane locally.
#
#   SATELLITE_VERSION=1.2.3 scripts/build-rpm.sh   # the release passes the tag
#
# SATELLITE_SENTRY_DSN is empty unless release.yml exported it from the
# repository secret, so a hand-run produces a build that cannot transmit.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "build-rpm.sh: only supported on Linux (got $(uname -s))." >&2
    exit 1
fi
if ! command -v rpmbuild >/dev/null 2>&1; then
    echo "build-rpm.sh: rpm-build is required (provides rpmbuild)." >&2
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

# Every target, not only satellite: cpack's install step builds `all` anyway,
# and building it here puts a warning the distro's compiler raises in the
# log, under the warnings-as-errors the preset sets.
echo "=== Build ==="
cmake --build --preset package -j "$(nproc)"

echo "=== Package (cpack -G RPM) ==="
mkdir -p "${dist_dir}"
rm -rf build-package/cpack
cpack -G RPM --config build-package/CPackConfig.cmake -B build-package/cpack
cp -f build-package/cpack/satellite-*.rpm "${dist_dir}/"
ls -l "${dist_dir}"/satellite-*.rpm
