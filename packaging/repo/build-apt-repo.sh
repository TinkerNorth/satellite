#!/usr/bin/env bash
# build-apt-repo.sh: regenerate the APT repository tree under a given
# pages directory, then GPG-sign the result.
#
# The repo layout we produce (rooted at $PAGES_DIR):
#
#   debian/pool/main/s/satellite/satellite_<ver>_amd64.deb
#   debian/dists/stable/Release
#   debian/dists/stable/Release.gpg
#   debian/dists/stable/InRelease
#   debian/dists/stable/main/binary-amd64/{Packages,Packages.gz}
#
# Usage (from CI):
#   build-apt-repo.sh <pages-dir> <release-staging-dir>
#
# The release-staging-dir is the directory the workflow already
# downloaded artifacts into. Any *.deb file matching the satellite
# naming pattern is copied into the pool.
#
# Requires: apt-utils (apt-ftparchive), gpg (with $GPG_KEY_ID imported).
set -euo pipefail

PAGES_DIR="${1:-}"
STAGING_DIR="${2:-}"
SUITE="${SATELLITE_APT_SUITE:-stable}"
COMPONENT="main"
ARCH="amd64"
ORIGIN="${SATELLITE_APT_ORIGIN:-TinkerNorth}"
LABEL="${SATELLITE_APT_LABEL:-Satellite}"
DESCRIPTION="Satellite Linux package repository"

if [ -z "$PAGES_DIR" ] || [ -z "$STAGING_DIR" ]; then
    echo "Usage: $0 <pages-dir> <release-staging-dir>" >&2
    exit 2
fi
if [ -z "${GPG_KEY_ID:-}" ]; then
    echo "GPG_KEY_ID must be set (the public key fingerprint or short ID)." >&2
    exit 2
fi
if ! command -v apt-ftparchive >/dev/null 2>&1; then
    echo "apt-ftparchive not found; install apt-utils." >&2
    exit 2
fi

# ── 1. Stage the new .deb into the pool ─────────────────────────────────────
pool_dir="$PAGES_DIR/debian/pool/${COMPONENT}/s/satellite"
mkdir -p "$pool_dir"
shopt -s nullglob
debs=("$STAGING_DIR"/satellite_*_${ARCH}.deb)
if [ "${#debs[@]}" -eq 0 ]; then
    echo "::warning::No satellite_*_${ARCH}.deb found in $STAGING_DIR; nothing to add"
else
    for deb in "${debs[@]}"; do
        echo "+ adding $(basename "$deb") to pool"
        cp -v "$deb" "$pool_dir/"
    done
fi

# ── 2. Generate the Packages index from everything in the pool ──────────────
arch_dir="$PAGES_DIR/debian/dists/${SUITE}/${COMPONENT}/binary-${ARCH}"
mkdir -p "$arch_dir"
(
    cd "$PAGES_DIR/debian"
    apt-ftparchive --arch "${ARCH}" packages "pool/${COMPONENT}" \
        > "dists/${SUITE}/${COMPONENT}/binary-${ARCH}/Packages"
    gzip -kf "dists/${SUITE}/${COMPONENT}/binary-${ARCH}/Packages"
)

# ── 3. Generate the per-suite Release manifest ──────────────────────────────
# `apt-ftparchive release` walks the suite dir and emits checksums for every
# Packages / Packages.gz / Contents file under it. The override block below
# fills in the human-meta fields that apt's authenticity check requires.
suite_dir="$PAGES_DIR/debian/dists/${SUITE}"
release_conf="$(mktemp)"
trap 'rm -f "$release_conf"' EXIT
cat > "$release_conf" <<EOF
APT::FTPArchive::Release::Origin "${ORIGIN}";
APT::FTPArchive::Release::Label "${LABEL}";
APT::FTPArchive::Release::Suite "${SUITE}";
APT::FTPArchive::Release::Codename "${SUITE}";
APT::FTPArchive::Release::Architectures "${ARCH}";
APT::FTPArchive::Release::Components "${COMPONENT}";
APT::FTPArchive::Release::Description "${DESCRIPTION}";
EOF

(
    cd "$suite_dir"
    apt-ftparchive -c "$release_conf" release . > Release.new
    mv Release.new Release
)

# ── 4. Sign Release → Release.gpg (detached) + InRelease (clearsigned) ──────
# apt will accept either; modern apt prefers InRelease (one file, one fetch).
(
    cd "$suite_dir"
    rm -f Release.gpg InRelease
    gpg --batch --yes --default-key "$GPG_KEY_ID" --detach-sign --armor \
        --output Release.gpg Release
    gpg --batch --yes --default-key "$GPG_KEY_ID" --clearsign \
        --output InRelease Release
)

echo "APT repo ready at $PAGES_DIR/debian/dists/${SUITE}"
