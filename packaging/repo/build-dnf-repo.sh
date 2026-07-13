#!/usr/bin/env bash
# build-dnf-repo.sh: regenerate the DNF / YUM repository tree under a given
# pages directory, then GPG-sign the result.
#
# Layout under $PAGES_DIR:
#
#   rpm/x86_64/satellite-<ver>-1.x86_64.rpm
#   rpm/x86_64/repodata/
#       ├── repomd.xml
#       ├── repomd.xml.asc       ← detached signature dnf checks
#       ├── repomd.xml.key       ← public key copied next to repomd for dnf
#       ├── primary.xml.gz
#       ├── filelists.xml.gz
#       └── other.xml.gz
#   rpm/satellite.repo           ← dnf config users curl into /etc/yum.repos.d/
#
# Usage:
#   build-dnf-repo.sh <pages-dir> <release-staging-dir>
#
# Requires: createrepo_c, gpg (with $GPG_KEY_ID imported), $REPO_BASE_URL.
set -euo pipefail

PAGES_DIR="${1:-}"
STAGING_DIR="${2:-}"
ARCH="${SATELLITE_RPM_ARCH:-x86_64}"

if [ -z "$PAGES_DIR" ] || [ -z "$STAGING_DIR" ]; then
    echo "Usage: $0 <pages-dir> <release-staging-dir>" >&2
    exit 2
fi
if [ -z "${GPG_KEY_ID:-}" ] || [ -z "${REPO_BASE_URL:-}" ]; then
    echo "GPG_KEY_ID and REPO_BASE_URL must be set." >&2
    exit 2
fi
if ! command -v createrepo_c >/dev/null 2>&1; then
    echo "createrepo_c not found; install it (apt: createrepo-c)." >&2
    exit 2
fi

repo_dir="$PAGES_DIR/rpm/${ARCH}"
mkdir -p "$repo_dir"

# ── 1. Drop new RPMs into the arch tree ─────────────────────────────────────
shopt -s nullglob
rpms=("$STAGING_DIR"/satellite-*."${ARCH}".rpm)
if [ "${#rpms[@]}" -eq 0 ]; then
    echo "::warning::No satellite-*.${ARCH}.rpm found in $STAGING_DIR; nothing to add"
else
    for rpm in "${rpms[@]}"; do
        echo "+ adding $(basename "$rpm")"
        cp -v "$rpm" "$repo_dir/"
    done
fi

# Regenerate metadata from scratch (cheap at our scale).
createrepo_c --update --no-database "$repo_dir"

# ── 3. Sign repomd.xml ──────────────────────────────────────────────────────
gpg --batch --yes --default-key "$GPG_KEY_ID" --detach-sign --armor \
    --output "$repo_dir/repodata/repomd.xml.asc" \
    "$repo_dir/repodata/repomd.xml"

# Export the public key next to repomd so dnf can fetch it.
gpg --batch --yes --armor --export "$GPG_KEY_ID" \
    > "$repo_dir/repodata/repomd.xml.key"

# ── 4. Drop a satellite.repo file users can curl into /etc/yum.repos.d/ ─────
cat > "$PAGES_DIR/rpm/satellite.repo" <<EOF
[satellite]
name=Satellite (TinkerNorth)
baseurl=${REPO_BASE_URL}/rpm/${ARCH}
enabled=1
gpgcheck=1
repo_gpgcheck=1
gpgkey=${REPO_BASE_URL}/gpg.key
EOF

echo "DNF repo ready at $repo_dir"
