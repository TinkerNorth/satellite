#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# Prints an osv-scanner custom lockfile (the JSON shape of `osv-scanner --format json`) that
# names every component in lib/VENDORED.md by the upstream commit its pin resolves to, so
# OSV-Scanner can match the advisories that carry git ranges:
#
#   bash scripts/vendored-osv-lockfile.sh [inventory.md] > vendored-osv.json
#   osv-scanner --config=osv-scanner.toml --lockfile osv-scanner:vendored-osv.json
#
# A block without an Upstream or Pinned-commit line, or a pin that names no commit in its
# upstream (a missing tag, a mistyped SHA), fails the script, so no component can drop out of
# the scan unnoticed.
set -euo pipefail

inventory="${1:-lib/VENDORED.md}"

fail() {
    echo "vendored-osv-lockfile: $*" >&2
    exit 1
}

# One "upstream pin" line per "## " block; a block missing either field prints "MISSING <heading>".
component_pins() {
    tr -d '\r' < "$inventory" | awk '
        function flush() {
            if (heading == "") return
            if (upstream == "" || pin == "") print "MISSING " heading
            else print upstream " " pin
        }
        /^## / { flush(); heading = substr($0, 4); upstream = ""; pin = ""; next }
        /^- Upstream: / { upstream = $3 }
        /^- Pinned-commit: / { pin = $3 }
        END { flush() }
    '
}

# Fetches just that commit object (no trees, no blobs): GitHub answers "not our ref" for a SHA
# the repository does not have, which a pin typo or a SHA from another repository would be.
commit_in_upstream() {
    local url="$1" sha="$2" scratch found=false
    scratch=$(mktemp -d)
    git -C "$scratch" init -q
    if git -C "$scratch" fetch -q --depth=1 --filter=tree:0 "$url" "$sha" 2>/dev/null; then
        found=true
    fi
    rm -rf "$scratch"
    [ "$found" = true ]
}

# A tag's commit is the peeled ^{} line when the tag is annotated, else the tag's own line.
resolve_commit() {
    local url="$1" pin="$2" refs peeled direct
    if [[ "$pin" =~ ^[0-9a-f]{40}$ ]]; then
        if commit_in_upstream "$url" "$pin"; then echo "$pin"; fi
        return
    fi
    refs=$(git ls-remote --tags "$url")
    peeled=$(awk -v ref="refs/tags/${pin}^{}" '$2 == ref { print $1 }' <<< "$refs")
    direct=$(awk -v ref="refs/tags/${pin}" '$2 == ref { print $1 }' <<< "$refs")
    echo "${peeled:-$direct}"
}

main() {
    local line url pin commit entries=()
    while IFS= read -r line; do
        [[ "$line" != MISSING* ]] || fail "'${line#MISSING }' has no Upstream or Pinned-commit line"
        url="${line%% *}"
        pin="${line#* }"
        [[ "$url" =~ ^https://github\.com/[A-Za-z0-9._-]+/[A-Za-z0-9._-]+$ ]] ||
            fail "upstream '$url' is not a GitHub repository URL"
        commit=$(resolve_commit "$url" "$pin")
        [[ "$commit" =~ ^[0-9a-f]{40}$ ]] || fail "pin '$pin' does not resolve in $url"
        entries+=("{\"package\":{\"name\":\"$url\",\"commit\":\"$commit\"}}")
    done < <(component_pins)
    [ "${#entries[@]}" -gt 0 ] || fail "no component blocks in $inventory"
    local joined
    joined=$(IFS=,; echo "${entries[*]}")
    printf '{"results":[{"packages":[%s]}]}\n' "$joined"
}

main
