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
#   scripts/package-in-container.sh arch <AppImage>
#                                            archlinux, linux/amd64: the pacman
#                                            package around an x86_64 AppImage
#                                            (one the appimage lane or CI built)
#
# DOCKER_FLAGS adds flags to `docker run` (default: --cpus 4 --memory 3g), and
# SATELLITE_JOBS sets the build's parallelism inside (default 4, to match:
# nproc in a container reports the host's cores, which the memory cap does
# not cover).
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

case "${1:-}" in
    deb) image="debian:trixie" ;;
    rpm) image="fedora:44" ;;
    appimage) image="ubuntu:22.04" ;;
    arch) image="archlinux:base-devel-20261108.0.623074" ;;
    *)
        echo "usage: scripts/package-in-container.sh deb|rpm|appimage|arch <AppImage>" >&2
        exit 2
        ;;
esac
kind="$1"
appimage_src=""
if [ "${kind}" = arch ]; then
    appimage_src="${2:?arch needs the x86_64 AppImage to package}"
    appimage_src="$(cd "$(dirname "${appimage_src}")" && pwd)/$(basename "${appimage_src}")"
fi
command -v docker >/dev/null 2>&1 || { echo "docker is required" >&2; exit 1; }

src="$(mktemp -d)"
trap 'rm -rf "${src}"' EXIT
git ls-files -z | xargs -0 tar -cf - | tar -xf - -C "${src}"
mkdir -p dist

read -r -a docker_flags <<<"${DOCKER_FLAGS:---cpus 4 --memory 3g}"
platform=()
if [ "${kind}" = arch ]; then
    # Arch publishes x86_64 images only, and the AppImage is x86_64.
    platform=(--platform linux/amd64)
    cp "${appimage_src}" "${src}/satellite-x86_64.AppImage"
    lane="pacman -Syu --noconfirm git pacman-contrib
        bash scripts/build-arch-package.sh satellite-x86_64.AppImage \"\$(tr -d '[:space:]' < VERSION)\" arch-packages
        bash scripts/check-arch-package.sh arch-packages satellite-x86_64.AppImage
        cp arch-packages/*.pkg.tar.zst /out/"
elif [ "${kind}" = appimage ]; then
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
docker run --rm "${docker_flags[@]}" "${platform[@]+"${platform[@]}"}" -e RUNNER_TEMP=/tmp/runner \
    -e SATELLITE_JOBS="${SATELLITE_JOBS:-4}" \
    -v "${src}:/src" -v "${repo_root}/dist:/out" -w /src "${image}" \
    bash -c "set -euo pipefail; mkdir -p /tmp/runner; ${lane}"
ls -l dist/
