#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# Installs a built satellite-bin package the way an Arch user gets it, from a
# signed pacman repository, and holds it to what it wraps: only satellite-bin
# is built (no split debug package), the installed AppImage is byte for byte
# the one that was packaged (makepkg's strip step once cut the squashfs
# payload off it, so the 2.3.0 package installed a runtime that could not
# start), and the installed `satellite` comes up far enough to load its
# bundled controller database, then exits 0 on SIGTERM.
#
# The repository is signed with a throwaway key made here and trusted only
# by this container's pacman; nothing is published. Runs as root in an Arch
# container (linux-ci.yml's arch job).
#
#   scripts/check-arch-package.sh <package dir> <the packaged AppImage>
set -euo pipefail

pkgdir="${1:?usage: check-arch-package.sh <package dir> <AppImage>}"
appimage="${2:?usage: check-arch-package.sh <package dir> <AppImage>}"

fail() {
    echo "::error::check-arch-package: $*" >&2
    exit 1
}
step() { echo ""; echo "=== $1 ==="; }

step "Packages built"
ls -l "${pkgdir}"
# Every finding is collected and reported at the end, so one run names them
# all (a split debug package and a stripped AppImage came together).
problems=()
package=""
for candidate in "${pkgdir}"/*.pkg.tar.zst; do
    case "$(basename "${candidate}")" in
        satellite-bin-debug-*) problems+=("makepkg built a split $(basename "${candidate}"); the PKGBUILD needs options=('!debug')") ;;
        satellite-bin-[0-9]*) package="${candidate}" ;;
        *) problems+=("unexpected package $(basename "${candidate}")") ;;
    esac
done
[ -n "${package}" ] || fail "no satellite-bin package in ${pkgdir}"
packages=("${package}")

step "A pacman repository signed with a throwaway key"
export GNUPGHOME
GNUPGHOME="$(mktemp -d)"
gpg --batch --pinentry-mode loopback --passphrase '' \
    --quick-generate-key 'satellite CI throwaway <ci@tinkernorth.invalid>' ed25519 sign never
key="$(gpg --batch --list-secret-keys --with-colons | awk -F: '/^fpr/ {print $10; exit}')"
repo="$(mktemp -d)"
cp "${packages[0]}" "${repo}/"
gpg --batch --yes --default-key "${key}" --detach-sign "${repo}/$(basename "${packages[0]}")"
repo-add --sign --key "${key}" "${repo}/satellite-ci.db.tar.gz" "${repo}/$(basename "${packages[0]}")"
pacman-key --init >/dev/null
gpg --batch --armor --export "${key}" >"${repo}/key.asc"
pacman-key --add "${repo}/key.asc" >/dev/null
pacman-key --lsign-key "${key}" >/dev/null
printf '\n[satellite-ci]\nSigLevel = Required\nServer = file://%s\n' "${repo}" >>/etc/pacman.conf

step "Install from it"
pacman -Sy --noconfirm satellite-bin

step "The installed AppImage is the packaged one"
want="$(sha256sum "${appimage}" | cut -d' ' -f1)"
got="$(sha256sum /opt/satellite/satellite.AppImage | cut -d' ' -f1)"
echo "packaged  ${want} ($(wc -c <"${appimage}") bytes)"
echo "installed ${got} ($(wc -c </opt/satellite/satellite.AppImage) bytes)"
[ "${got}" = "${want}" ] || problems+=("the installed AppImage is not the packaged one: makepkg stripped it, and the PKGBUILD needs options=('!strip')")

step "Launch the installed satellite"
# No FUSE in a container: the AppImage runtime extracts itself instead.
home="$(mktemp -d)"
log="${home}/launch.log"
HOME="${home}" APPIMAGE_EXTRACT_AND_RUN=1 /usr/bin/satellite >"${log}" 2>&1 &
app=$!
loaded=0
for _ in $(seq 1 30); do
    kill -0 "${app}" 2>/dev/null || break
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
for problem in "${problems[@]+"${problems[@]}"}"; do
    echo "::error::check-arch-package: ${problem}" >&2
done
[ "${#problems[@]}" -eq 0 ] || exit 1
[ "${alive}" = 1 ] || fail "the installed satellite exited on its own (${rc})"
[ "${loaded}" = 1 ] || fail "the installed satellite never logged 'Controller database N (bundled), M records'"
[ "${rc}" = 0 ] || fail "the installed satellite did not exit 0 on SIGTERM (got ${rc})"
echo "check-arch-package: satellite-bin installs from a signed repository, carries the packaged AppImage unchanged, and starts"
