#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# Installs Satellite from a package repository the way a user does, with
# every signature check on: builds the APT or DNF repository around a package
# with packaging/repo/build-apt-repo.sh or build-dnf-repo.sh (the scripts
# publish-repos.yml runs), signed with a throwaway key made here, adds it as
# packaging/repo/index.html tells a user to (the key, then the source list or
# the .repo file), installs `satellite` with the package manager, and starts
# it (scripts/check-launch.sh). Nothing is published.
#
# Runs as root in a clean container of the target distro: linux-ci.yml's
# apt-repo job in debian:trixie, its dnf-repo job in fedora:44.
#
#   scripts/check-repo-install.sh apt <satellite_X.Y.Z_amd64.deb>
#   scripts/check-repo-install.sh dnf <satellite-X.Y.Z-1.x86_64.rpm>
set -euo pipefail

kind="${1:?usage: check-repo-install.sh apt|dnf <package>}"
package="${2:?usage: check-repo-install.sh apt|dnf <package>}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[ -f "${package}" ] || { echo "check-repo-install: ${package} does not exist" >&2; exit 1; }

step() { echo ""; echo "=== $1 ==="; }

step "Tools to build the repository"
case "${kind}" in
    apt)
        export DEBIAN_FRONTEND=noninteractive
        apt-get update
        apt-get install -y --no-install-recommends apt-utils gnupg ca-certificates
        ;;
    dnf)
        dnf -y install --setopt=install_weak_deps=False createrepo_c gnupg2 rpm-sign
        ;;
    *)
        echo "usage: check-repo-install.sh apt|dnf <package>" >&2
        exit 2
        ;;
esac

step "A throwaway signing key"
export GNUPGHOME
GNUPGHOME="$(mktemp -d)"
gpg --batch --pinentry-mode loopback --passphrase '' \
    --quick-generate-key 'satellite CI throwaway <ci@tinkernorth.invalid>' rsa3072 sign never
GPG_KEY_ID="$(gpg --batch --list-secret-keys --with-colons | awk -F: '/^fpr/ {print $10; exit}')"
export GPG_KEY_ID

pages="$(mktemp -d)"
staging="$(mktemp -d)"
chmod 755 "${pages}"
cp "${package}" "${staging}/"
gpg --batch --armor --export "${GPG_KEY_ID}" >"${pages}/gpg.key"
export REPO_BASE_URL="file://${pages}"

case "${kind}" in
    apt)
        step "Build the APT repository (packaging/repo/build-apt-repo.sh)"
        bash "${repo_root}/packaging/repo/build-apt-repo.sh" "${pages}" "${staging}"

        step "Add it as a user does, and install"
        gpg --dearmor <"${pages}/gpg.key" >/usr/share/keyrings/satellite-archive-keyring.gpg
        echo "deb [signed-by=/usr/share/keyrings/satellite-archive-keyring.gpg] ${REPO_BASE_URL}/debian stable main" \
            >/etc/apt/sources.list.d/satellite.list
        apt-get update
        apt-get install -y satellite
        ;;
    dnf)
        step "Build the DNF repository (packaging/repo/build-dnf-repo.sh)"
        bash "${repo_root}/packaging/repo/build-dnf-repo.sh" "${pages}" "${staging}"

        step "Add it as a user does, and install with gpgcheck on"
        cp "${pages}/rpm/satellite.repo" /etc/yum.repos.d/satellite.repo
        cat /etc/yum.repos.d/satellite.repo
        grep -qx 'gpgcheck=1' /etc/yum.repos.d/satellite.repo \
            || { echo "::error::the .repo file does not check package signatures" >&2; exit 1; }
        dnf -y install satellite
        ;;
esac

step "Launch the installed satellite"
bash "${repo_root}/scripts/check-launch.sh"
echo "check-repo-install: satellite installs from a signed ${kind} repository and starts"
