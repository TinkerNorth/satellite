#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# clang-format gate: the exact file set and invocation every CI lane runs
# (linux-ci.yml, macos-ci.yml, windows-ci.yml under git-bash). One script so
# the workflows and scripts/ci-local.sh cannot drift; scripts/ci-local.ps1
# ports the same file set to PowerShell for bash-less Windows shells.
#
# CI pins clang-format 22.1.4 (PyPI wheel / pipx / venv). Another version can
# disagree on braced-init lists; treat a surprise verdict with suspicion.
set -euo pipefail
cd "$(dirname "$0")/.."

FILES=$(find src tests -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.mm' \))
echo "$FILES" | xargs clang-format --dry-run --Werror
echo "clang-format: OK"
