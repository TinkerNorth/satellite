#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# Launches the installed /usr/bin/satellite headless and holds it to three
# things: it stays up, it loads the controller database it carries (the
# "Controller database N (bundled), M records" start-up line in its log), and
# it exits 0 on SIGTERM. scripts/check-package.sh runs it after installing a
# package file, scripts/check-repo-install.sh after installing from a
# package repository.
#
#   scripts/check-launch.sh
set -euo pipefail

fail() {
    echo "::error::check-launch: $*" >&2
    exit 1
}

# No display and no tray host here: satellite runs headless (the fallback
# linux-ci.yml's headless job builds) and exits 0 on SIGTERM. Its own HOME,
# so the start-up reads no cache a previous run left.
home="$(mktemp -d)"
log="${home}/launch.log"
XDG_STATE_HOME="${home}/state" HOME="${home}" /usr/bin/satellite >"${log}" 2>&1 &
app=$!
started=0
loaded=0
for _ in $(seq 1 20); do
    if ! kill -0 "${app}" 2>/dev/null; then break; fi
    started=1
    # Satellite logs to its file under XDG_STATE_HOME, not to stdout.
    if cat "${home}"/state/satellite/logs/* 2>/dev/null \
        | grep -qE 'Controller database [0-9]+ \(bundled\), [0-9]+ records'; then
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
cat "${home}"/state/satellite/logs/* 2>/dev/null | sed 's/^/[satellite log] /' || true
# The file sink writes in the background; read it again once the process
# has flushed it on exit.
if cat "${home}"/state/satellite/logs/* 2>/dev/null \
    | grep -qE 'Controller database [0-9]+ \(bundled\), [0-9]+ records'; then
    loaded=1
fi
if grep -qiE 'error while loading shared libraries|cannot open shared object' "${log}"; then
    fail "the installed package cannot load its shared libraries"
fi
[ "${started}" = 1 ] && [ "${alive}" = 1 ] || fail "satellite exited on its own (${rc}) instead of staying up"
[ "${loaded}" = 1 ] || fail "satellite never logged 'Controller database N (bundled), M records'"
[ "${rc}" = 0 ] || fail "satellite did not exit 0 on SIGTERM (got ${rc})"
cat "${home}"/state/satellite/logs/* | grep -E 'Controller database [0-9]+ \(bundled\), [0-9]+ records' | head -1
echo "check-launch: the installed satellite came up headless, loaded its bundled controller database, and exited 0 on SIGTERM"

