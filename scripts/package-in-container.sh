#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# Runs a Linux package lane locally the way CI runs it: the same container
# image, the same dependency list (scripts/install-package-deps.sh), the same
# build (scripts/build-deb.sh or build-rpm.sh) and the same install-and-launch
# check (scripts/check-package.sh). The tree is copied into the container
# from `git ls-files`, so untracked build output never leaks in, and the
# package lands in ./dist.
#
#   scripts/package-in-container.sh deb      # debian:trixie
#   scripts/package-in-container.sh rpm      # fedora:44
#   scripts/package-in-container.sh appimage # ubuntu:22.04, the glibc floor
#
# DOCKER_FLAGS adds flags to `docker run` (default: --cpus 4 --memory 3g).
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

case "${1:-}" in
    deb) image="debian:trixie" ;;
    rpm) image="fedora:44" ;;
    appimage) image="ubuntu:22.04" ;;
    *)
        echo "usage: scripts/package-in-container.sh deb|rpm|appimage" >&2
        exit 2
        ;;
esac
kind="$1"
command -v docker >/dev/null 2>&1 || { echo "docker is required" >&2; exit 1; }

src="$(mktemp -d)"
trap 'rm -rf "${src}"' EXIT
git ls-files -z | xargs -0 tar -cf - | tar -xf - -C "${src}"
mkdir -p dist

read -r -a docker_flags <<<"${DOCKER_FLAGS:---cpus 4 --memory 3g}"
if [ "${kind}" = appimage ]; then
    # The two checks linux-ci.yml's appimage job runs: the AppImage's own
    # content (inside build-appimage.sh) and the extracted AppRun staying up.
    lane="bash scripts/install-package-deps.sh appimage
        bash scripts/build-appimage.sh
        bash scripts/smoke-alive.sh \"\${RUNNER_TEMP}/appimage-content/squashfs-root/AppRun\" 45
        cp stage/satellite-*-x86_64.AppImage /out/"
else
    lane="bash scripts/install-package-deps.sh ${kind}
        DIST_DIR=/src/dist bash scripts/build-${kind}.sh
        bash scripts/check-package.sh ${kind} \"\$(ls dist/satellite*.${kind} | head -1)\"
        cp dist/satellite*.${kind} /out/"
fi
docker run --rm "${docker_flags[@]}" -e RUNNER_TEMP=/tmp/runner \
    -v "${src}:/src" -v "${repo_root}/dist:/out" -w /src "${image}" \
    bash -c "set -euo pipefail; mkdir -p /tmp/runner; ${lane}"
ls -l dist/
