#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# Builds the satellite-bin pacman package from packaging/aur/PKGBUILD around
# a given AppImage, the way publish-repos.yml's arch-publish job does for a
# release and linux-ci.yml's arch job does on every pull request. The sources
# are placed beside the PKGBUILD before makepkg runs, so makepkg downloads
# nothing: the package wraps exactly the AppImage named here, and the desktop
# file and udev rule of this checkout. updpkgsums then writes their real
# SHA-256 sums.
#
# Runs as root in an Arch container; makepkg itself runs as a throwaway user,
# because it refuses to run as root.
#
#   scripts/build-arch-package.sh <AppImage> <pkgver> <output dir>
set -euo pipefail

appimage="${1:?usage: build-arch-package.sh <AppImage> <pkgver> <output dir>}"
pkgver="${2:?usage: build-arch-package.sh <AppImage> <pkgver> <output dir>}"
out="${3:?usage: build-arch-package.sh <AppImage> <pkgver> <output dir>}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

[ -f "${appimage}" ] || { echo "build-arch-package: ${appimage} does not exist" >&2; exit 1; }
[[ "${pkgver}" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "build-arch-package: pkgver '${pkgver}' is not MAJOR.MINOR.PATCH" >&2; exit 1; }

id builder >/dev/null 2>&1 || useradd -m builder
builddir="$(mktemp -d /tmp/satellite-bin.XXXXXX)"
cp "${repo_root}/packaging/aur/PKGBUILD" "${repo_root}/packaging/aur/satellite-bin.install" "${builddir}/"
sed -i "s/^pkgver=.*/pkgver=${pkgver}/; s/^pkgrel=.*/pkgrel=1/" "${builddir}/PKGBUILD"
cp "${appimage}" "${builddir}/satellite-${pkgver}-x86_64.AppImage"
cp "${repo_root}/packaging/debian/satellite.desktop" "${builddir}/satellite.desktop"
cp "${repo_root}/packaging/debian/70-satellite-uinput.rules" "${builddir}/70-satellite-uinput.rules"
chown -R builder:builder "${builddir}"
# -d skips dependency checks: package() only copies files, and the depends
# are runtime-only.
su builder -c "cd '${builddir}' && updpkgsums && PACKAGER='satellite-release-bot <releases@tinkernorth.invalid>' makepkg -d --noconfirm"
mkdir -p "${out}"
cp "${builddir}"/*.pkg.tar.zst "${out}/"
ls -l "${out}"/*.pkg.tar.zst
