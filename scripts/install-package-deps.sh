#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# Installs what a Linux package lane needs inside its container: the build
# dependencies, the packaging tools, and the checks' tools. One list per
# package type, used by linux-ci.yml's and release.yml's deb and rpm jobs and
# by scripts/package-in-container.sh, so a local container run and CI install
# the same packages. The AppImage list is what linux-ci.yml's and
# release.yml's appimage jobs install on the ubuntu-22.04 runner.
#
#   scripts/install-package-deps.sh deb    # debian:trixie, as root
#   scripts/install-package-deps.sh rpm    # fedora:44, as root
#   scripts/install-package-deps.sh appimage   # ubuntu-22.04 (sudo) or ubuntu:22.04 (root)
set -euo pipefail

case "${1:-}" in
    deb)
        export DEBIAN_FRONTEND=noninteractive
        apt-get update
        apt-get install -y --no-install-recommends \
            git ca-certificates \
            build-essential cmake pkg-config curl \
            libsodium-dev libssl-dev libcurl4-openssl-dev libopus-dev \
            libayatana-appindicator3-dev libgtk-3-dev libnotify-dev \
            dpkg-dev fakeroot file gzip lintian python3
        ;;
    rpm)
        dnf -y install --setopt=install_weak_deps=False \
            git-core ca-certificates \
            gcc-c++ cmake make pkgconf-pkg-config rpm-build \
            libsodium-devel openssl-devel libcurl-devel opus-devel \
            gtk3-devel libayatana-appindicator-gtk3-devel libnotify-devel \
            gzip cpio gawk diffutils procps-ng python3
        dnf -y install --allowerasing curl
        ;;
    appimage)
        # The glibc floor is the build host's: ubuntu 22.04, glibc 2.35.
        sudo_cmd=()
        [ "$(id -u)" -eq 0 ] || sudo_cmd=(sudo)
        export DEBIAN_FRONTEND=noninteractive
        "${sudo_cmd[@]}" apt-get update
        "${sudo_cmd[@]}" apt-get install -y --no-install-recommends \
            git ca-certificates \
            build-essential cmake pkg-config curl \
            libsodium-dev libssl-dev libcurl4-openssl-dev libopus-dev \
            libayatana-appindicator3-dev libgtk-3-dev libnotify-dev \
            file imagemagick python3
        ;;
    *)
        echo "usage: scripts/install-package-deps.sh deb|rpm|appimage" >&2
        exit 2
        ;;
esac
