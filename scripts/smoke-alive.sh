#!/usr/bin/env bash
# Runs the satellite headless, in an isolated HOME, for long enough to cross its
# updater's 30 s settle and first update check, and fails if the process dies.
#
# Why: 2.1.3 segfaulted exactly there on macOS. Its Objective-C++ sources were
# compiled without ARC, so the updater's completion handler stored reply
# pointers that were released before the worker thread woke from its
# semaphore (objc_msgSend on a freed object). Nothing in the unit suites runs
# the binary long enough to see it; this does. The update check's own outcome
# (GitHub reachable or not) is not asserted, only that the process is alive.
#
#   scripts/smoke-alive.sh <satellite-binary> [seconds, default 45]
set -euo pipefail

BIN="${1:?usage: smoke-alive.sh <satellite-binary> [seconds]}"
SECONDS_ALIVE="${2:-45}"
[ -x "${BIN}" ] || { echo "smoke-alive: not executable: ${BIN}" >&2; exit 2; }

HOME_DIR="$(mktemp -d "${TMPDIR:-/tmp}/satellite-smoke.XXXXXX")"
LOG="${HOME_DIR}/satellite.log"
cleanup() {
    if [ -n "${PID:-}" ] && kill -0 "${PID}" >/dev/null 2>&1; then
        kill -TERM "${PID}" >/dev/null 2>&1 || true
        for _ in $(seq 1 20); do
            kill -0 "${PID}" >/dev/null 2>&1 || break
            sleep 0.25
        done
        kill -KILL "${PID}" >/dev/null 2>&1 || true
        wait "${PID}" 2>/dev/null || true
    fi
    rm -rf "${HOME_DIR}"
}
trap cleanup EXIT

HOME="${HOME_DIR}" "${BIN}" >"${LOG}" 2>&1 &
PID=$!

for i in $(seq 1 "${SECONDS_ALIVE}"); do
    if ! kill -0 "${PID}" >/dev/null 2>&1; then
        set +e
        wait "${PID}"
        status=$?
        set -e
        echo "smoke-alive: the satellite died after ${i} s (exit ${status}); its log:" >&2
        sed 's/^/[satellite] /' "${LOG}" >&2 || true
        exit 1
    fi
    sleep 1
done
echo "smoke-alive: alive after ${SECONDS_ALIVE} s, through the updater's settle and first check"
