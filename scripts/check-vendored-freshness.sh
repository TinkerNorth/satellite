#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# Vendored-component freshness: every `Last-vendored: YYYY-MM-DD` in
# lib/VENDORED.md is at most 90 days old, so each component is re-checked
# against its upstream at least once a quarter even when no advisory has
# fired. security.yml's vendored-freshness job and scripts/ci-local.sh run it.
#
#   scripts/check-vendored-freshness.sh
cd "$(dirname "$0")/.."
date_epoch() {
  date -u -d "$1" +%s 2>/dev/null || date -u -j -f '%Y-%m-%d' "$1" +%s 2>/dev/null || true
}
set -euo pipefail
if [ ! -f lib/VENDORED.md ]; then
  echo "::error::lib/VENDORED.md is missing; vendored components must be inventoried."
  exit 1
fi
# Each component block must contain a `Last-vendored: YYYY-MM-DD` line.
today_epoch=$(date -u +%s)
fail=0
while IFS= read -r line; do
  d="${line#*: }"
  d="${d## }"
  d="${d%% *}"
  ts=$(date_epoch "$d")
  if [ -z "$ts" ]; then
    echo "::error::lib/VENDORED.md: unparseable date '$d' in line: $line"
    fail=1
    continue
  fi
  age_days=$(( (today_epoch - ts) / 86400 ))
  if [ "$age_days" -gt 90 ]; then
    echo "::error::lib/VENDORED.md: entry dated $d is ${age_days} days old (>90). Refresh upstream and update the timestamp."
    fail=1
  fi
done < <(grep -E '^[[:space:]]*-?[[:space:]]*Last-vendored:' lib/VENDORED.md || true)
if ! grep -qE '^[[:space:]]*-?[[:space:]]*Last-vendored:' lib/VENDORED.md; then
  echo "::error::lib/VENDORED.md must contain at least one 'Last-vendored: YYYY-MM-DD' line."
  exit 1
fi
exit "$fail"
