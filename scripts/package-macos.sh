#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# The macOS stub bundle the release ships, in three steps that release.yml's
# macos job and macos-ci.yml's bundle job both run (the release signs and
# notarizes between `build` and `stage`, with secrets a pull request has not
# got):
#
#   scripts/package-macos.sh build                  satellite.app, preset package
#   scripts/package-macos.sh stage <label> [suffix] stage/satellite-macos-stub-
#                                                   <label><suffix>.dmg and .zip
#   scripts/package-macos.sh check                  mount the staged .dmg and run
#                                                   the .app inside it
#
# `stage` honours SATELLITE_DMG_NO_FINDER=1 (see below).
# `build` reads SATELLITE_VERSION (the release passes the tag; without it the
# build is labelled <VERSION>-dev) and SATELLITE_SENTRY_DSN (empty unless the
# release exported the repository secret). `stage` needs create-dmg
# (brew install create-dmg).
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

fail() {
    echo "::error::package-macos: $*" >&2
    exit 1
}

build() {
    local release_args=()
    if [ -n "${SATELLITE_VERSION:-}" ]; then
        release_args=(-DSATELLITE_RELEASE_VERSION="${SATELLITE_VERSION#v}")
    fi
    echo "=== Configure (preset package) ==="
    cmake --preset package "${release_args[@]+"${release_args[@]}"}" \
        -DSATELLITE_SENTRY_DSN="${SATELLITE_SENTRY_DSN:-}"
    echo "=== Build ==="
    cmake --build --preset package --target satellite -j "$(sysctl -n hw.ncpu)"
    test -x satellite.app/Contents/MacOS/satellite || fail "no satellite.app/Contents/MacOS/satellite"
    test -f satellite.app/Contents/Resources/web/index.html || fail "satellite.app carries no web/index.html"
}

stage() {
    local label="${1:?usage: package-macos.sh stage <label> [suffix]}"
    local suffix="${2:-}"
    command -v create-dmg >/dev/null 2>&1 || fail "create-dmg is required (brew install create-dmg)"
    rm -rf stage
    mkdir -p stage
    # One mountable disk image with the bundle pre-placed and an
    # /Applications link: the drag-to-Applications install. A signed build's
    # .app is already signed, notarized and stapled by then, so the image
    # only wraps it; Gatekeeper checks the .app at first launch.
    # SATELLITE_DMG_NO_FINDER=1 skips create-dmg's Finder AppleScript (the
    # window layout), which times out where no one can grant Finder
    # automation; the image's contents are the same.
    local finder=()
    if [ "${SATELLITE_DMG_NO_FINDER:-0}" = 1 ]; then
        finder=(--skip-jenkins)
    fi
    echo "=== Stage the .dmg ==="
    create-dmg "${finder[@]+"${finder[@]}"}" \
        --volname "Satellite ${label}" \
        --window-size 540 360 \
        --icon-size 128 \
        --icon "satellite.app" 130 180 \
        --app-drop-link 410 180 \
        --no-internet-enable \
        "stage/satellite-macos-stub-${label}${suffix}.dmg" \
        satellite.app
    # The .zip stays beside the .dmg for downloaders that just want the .app.
    echo "=== Stage the .zip ==="
    ditto -c -k --sequesterRsrc --keepParent satellite.app \
        "stage/satellite-macos-stub-${label}${suffix}.zip"
    ls -l stage/
}

check() {
    local dmg
    dmg="$(find stage -maxdepth 1 -name 'satellite-macos-stub-*.dmg' | head -1)"
    [ -n "${dmg}" ] || fail "no staged .dmg under stage/"
    local mount
    mount="$(mktemp -d)"
    hdiutil attach -nobrowse -readonly -mountpoint "${mount}" "${dmg}" >/dev/null
    local status=0
    (
        test -x "${mount}/satellite.app/Contents/MacOS/satellite" || fail "${dmg} holds no satellite.app"
        test -L "${mount}/Applications" || fail "${dmg} has no /Applications link"
        test -f "${mount}/satellite.app/Contents/Resources/web/index.html" || fail "${dmg}'s satellite.app carries no web/"
        bash scripts/smoke-alive.sh "${mount}/satellite.app/Contents/MacOS/satellite" 45
    ) || status=$?
    # The app the smoke ran can hold the volume for a moment after it exits.
    local tries=0
    until hdiutil detach "${mount}" >/dev/null 2>&1; do
        tries=$((tries + 1))
        [ "${tries}" -lt 10 ] || fail "${dmg} stays mounted at ${mount}"
        sleep 1
    done
    [ "${status}" -eq 0 ] || exit "${status}"
    echo "package-macos: ${dmg} mounts, holds satellite.app and /Applications, and its app stays up"
}

case "${1:-}" in
    build) build ;;
    stage)
        shift
        stage "$@"
        ;;
    check) check ;;
    *)
        sed -n '2,18p' "${BASH_SOURCE[0]}" >&2
        exit 2
        ;;
esac
