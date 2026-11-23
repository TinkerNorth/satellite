#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# Fails unless a packaged web/ folder carries the dashboard and the bundled
# controller database whole: index.html, controllers/v1/manifest.json equal to
# this checkout's, and every file the manifest lists at the size and SHA-256 it
# gives. Every Linux package runs it on what it ships (build-appimage.sh on the
# AppImage's own content, release.yml on the installed .deb and .rpm), because
# an AppImage without web/ shipped for releases before anyone noticed.
#
#   scripts/check-web-payload.sh <packaged web dir>
set -euo pipefail

web="${1:?usage: check-web-payload.sh <packaged web dir>}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
db="${web}/controllers/v1"
manifest="${db}/manifest.json"

fail() { echo "::error::check-web-payload: $*" >&2; exit 1; }

[ -f "${web}/index.html" ] || fail "${web}/index.html is missing"
[ -f "${manifest}" ] || fail "${manifest} is missing"
cmp -s "${manifest}" "${repo_root}/web/controllers/v1/manifest.json" \
    || fail "${manifest} differs from the checkout's"

# The manifest is the generator's pretty-printed form: a "<path>": { line,
# then its "sha256" and "bytes". Prints "<path> <sha256> <bytes>" per file.
entries="$(awk '
    /"[^"]+": *\{/ && infiles { match($0, /"[^"]+"/); path = substr($0, RSTART + 1, RLENGTH - 2) }
    /"files": *\{/ { infiles = 1 }
    /"sha256":/ { match($0, /: *"[0-9a-f]+"/); sha = substr($0, RSTART, RLENGTH); gsub(/[^0-9a-f]/, "", sha) }
    /"bytes":/ { match($0, /[0-9]+/); print path, sha, substr($0, RSTART, RLENGTH) }
' "${manifest}")"

count=0
while read -r path sha bytes; do
    file="${db}/${path}"
    [ -f "${file}" ] || fail "${file} is listed in the manifest but missing"
    got_bytes="$(wc -c < "${file}" | tr -d '[:space:]')"
    [ "${got_bytes}" = "${bytes}" ] || fail "${file} is ${got_bytes} bytes, the manifest says ${bytes}"
    got_sha="$(sha256sum "${file}" | cut -d' ' -f1)"
    [ "${got_sha}" = "${sha}" ] || fail "${file} has SHA-256 ${got_sha}, the manifest says ${sha}"
    count=$((count + 1))
done <<<"${entries}"

# controllers.json and the six locales' strings; with the manifest, eight files.
want=7
[ "${count}" -eq "${want}" ] || fail "the manifest lists ${count} files, expected ${want}"
echo "check-web-payload: ${web} holds index.html and the manifest's ${count} files plus the manifest, every size and SHA-256 as listed"
