#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# The release's two Sentry steps, run with the sentry-cli scripts/tools.sh
# pins, so the four jobs that call them carry no copy of the version, the
# digest or the organisation lookup.
#
#   scripts/sentry-cli.sh upload-symbols <file>...   debug files, with sources
#   scripts/sentry-cli.sh release <version>          release, commits, deploy
#
# Reads SENTRY_DSN, SENTRY_AUTH_TOKEN and the optional SENTRY_ORG and
# SENTRY_PROJECT from the environment. Without a token or a DSN it says so
# and exits 0: a fork or a release without crash reporting has nothing to
# send. The organisation and project fall back to the numeric ids inside the
# DSN, which the Sentry API accepts in place of slugs.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=scripts/tools.sh
source "${repo_root}/scripts/tools.sh"

usage() {
    sed -n '8,9p' "${BASH_SOURCE[0]}" >&2
    exit 2
}

[ "$#" -ge 2 ] || usage
command="$1"
shift

if [ -z "${SENTRY_AUTH_TOKEN:-}" ] || [ -z "${SENTRY_DSN:-}" ]; then
    echo "::notice::SENTRY_AUTH_TOKEN or SENTRY_DSN unset; skipping sentry-cli ${command}."
    exit 0
fi
if [ -z "${SENTRY_ORG:-}" ]; then
    SENTRY_ORG="$(sed -nE 's#.*@o([0-9]+)\..*#\1#p' <<<"${SENTRY_DSN}")"
    export SENTRY_ORG
fi
if [ -z "${SENTRY_PROJECT:-}" ]; then
    SENTRY_PROJECT="${SENTRY_DSN##*/}"
    export SENTRY_PROJECT
fi

cli="$(fetch_sentry_cli)"

case "${command}" in
    upload-symbols)
        "${cli}" debug-files upload --include-sources "$@"
        ;;
    release)
        version="$1"
        "${cli}" releases new "${version}"
        "${cli}" releases set-commits "${version}" --auto
        "${cli}" releases finalize "${version}"
        "${cli}" deploys new -e production -r "${version}"
        ;;
    *)
        usage
        ;;
esac
