#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# Checks a built .deb or .rpm the way a user meets it: its contents and
# scriptlets, the dashboard and controller database it ships
# (scripts/check-web-payload.sh), then installs it, launches the installed
# /usr/bin/satellite headless and holds it to three things: it stays up, it
# loads the controller database it carries (the "Controller database N
# (bundled), M records" start-up line), and it exits 0 on SIGTERM. Nothing
# else can stand in for the last part: unit tests, lints and a contents
# check all pass against a build tree, and a build tree is exactly what
# hides a missing runtime dependency.
#
# Runs as root in the package's own distro (CI's debian:trixie and fedora:44
# containers, or scripts/package-in-container.sh locally).
#
#   scripts/check-package.sh deb dist/satellite_2.3.0_amd64.deb
#   scripts/check-package.sh rpm dist/satellite-2.3.0-1.x86_64.rpm
set -euo pipefail

kind="${1:?usage: check-package.sh deb|rpm <package>}"
package="${2:?usage: check-package.sh deb|rpm <package>}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

fail() {
    echo "::error::check-package: $*" >&2
    exit 1
}
step() { echo ""; echo "=== $1 ==="; }

[ -f "${package}" ] || fail "${package} does not exist"
package="$(cd "$(dirname "${package}")" && pwd)/$(basename "${package}")"
payload="$(mktemp -d)"
scriptlets=""

case "${kind}" in
    deb)
        step "Contents"
        contents="$(dpkg-deb -c "${package}")"
        for path in \
            ./usr/bin/satellite \
            ./usr/lib/udev/rules.d/70-satellite-uinput.rules \
            ./usr/lib/modules-load.d/satellite.conf \
            ./usr/share/applications/satellite.desktop \
            ./usr/share/metainfo/io.github.tinkernorth.satellite.metainfo.xml \
            ./usr/share/pixmaps/satellite.png; do
            grep -qE " ${path}\$" <<<"${contents}" || fail "${path} missing from the .deb"
        done
        dpkg-deb -x "${package}" "${payload}"
        control="$(mktemp -d)"
        dpkg-deb -e "${package}" "${control}"
        scriptlets="$(cat "${control}/postinst")"
        ;;
    rpm)
        step "Contents"
        contents="$(rpm -qlp "${package}")"
        printf '%s\n' "${contents}"
        rpm -qp --requires "${package}"
        for path in \
            /usr/bin/satellite \
            /usr/lib/udev/rules.d/70-satellite-uinput.rules \
            /usr/lib/modules-load.d/satellite.conf \
            /usr/share/applications/satellite.desktop \
            /usr/share/metainfo/io.github.tinkernorth.satellite.metainfo.xml; do
            grep -qx "${path}" <<<"${contents}" || fail "${path} missing from the .rpm"
        done
        scriptlets="$(rpm -qp --scripts "${package}")"
        rpm2cpio "${package}" | (cd "${payload}" && cpio -idm --quiet)
        ;;
    *)
        fail "unknown package kind '${kind}' (deb or rpm)"
        ;;
esac

step "Scriptlets"
# Without the reload and the modprobe a fresh install cannot open
# /dev/uinput until the next reboot.
grep -q 'udevadm control' <<<"${scriptlets}" || fail "the ${kind} carries no udev reload"
grep -q 'modprobe uinput' <<<"${scriptlets}" || fail "the ${kind} carries no modprobe uinput"
echo "udev reload and modprobe uinput present"

step "Web payload"
bash "${repo_root}/scripts/check-web-payload.sh" "${payload}/usr/share/satellite/web"

step "Install"
case "${kind}" in
    deb) apt-get install -y "${package}" ;;
    rpm) dnf -y install "${package}" ;;
esac
if ldd /usr/bin/satellite | grep -i 'not found'; then
    fail "the installed satellite has unresolved shared libraries"
fi

step "Launch the installed satellite"
bash "${repo_root}/scripts/check-launch.sh"

if [ "${kind}" = deb ]; then
    # Last, so the install and launch above have run whatever it finds.
    # Warnings fail as errors do: no override file, no tag is waved through.
    step "lintian"
    # As an unprivileged user: lintian warns when it runs as root.
    chmod a+r "${package}"
    runuser -u nobody -- env HOME=/tmp lintian --fail-on error,warning --tag-display-limit 0 "${package}"
fi
