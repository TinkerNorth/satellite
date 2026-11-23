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
        step "lintian"
        lintian --fail-on error --tag-display-limit 0 "${package}"

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
# No display and no tray host here: satellite runs headless (the fallback
# linux-ci.yml's headless job builds) and exits 0 on SIGTERM. Its own HOME,
# so the start-up reads no cache a previous run left.
home="$(mktemp -d)"
log="${home}/launch.log"
HOME="${home}" /usr/bin/satellite >"${log}" 2>&1 &
app=$!
started=0
loaded=0
for _ in $(seq 1 20); do
    if ! kill -0 "${app}" 2>/dev/null; then break; fi
    started=1
    if grep -qE 'Controller database [0-9]+ \(bundled\), [0-9]+ records' "${log}"; then
        loaded=1
        break
    fi
    sleep 1
done
alive=0
kill -0 "${app}" 2>/dev/null && alive=1
if [ "${alive}" = 1 ]; then
    kill -TERM "${app}"
    for _ in $(seq 1 15); do
        kill -0 "${app}" 2>/dev/null || break
        sleep 1
    done
    kill -0 "${app}" 2>/dev/null && kill -KILL "${app}"
fi
set +e
wait "${app}"
rc=$?
set -e
sed 's/^/[satellite] /' "${log}"
if grep -qiE 'error while loading shared libraries|cannot open shared object' "${log}"; then
    fail "the installed package cannot load its shared libraries"
fi
[ "${started}" = 1 ] && [ "${alive}" = 1 ] || fail "satellite exited on its own (${rc}) instead of staying up"
[ "${loaded}" = 1 ] || fail "satellite never logged 'Controller database N (bundled), M records'"
[ "${rc}" = 0 ] || fail "satellite did not exit 0 on SIGTERM (got ${rc})"
grep -E 'Controller database [0-9]+ \(bundled\), [0-9]+ records' "${log}" | head -1
echo "check-package: the installed ${kind} came up headless, loaded its bundled controller database, and exited 0 on SIGTERM"
