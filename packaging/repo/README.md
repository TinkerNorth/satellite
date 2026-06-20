# Linux package repositories on GitHub Pages

This directory holds everything needed to publish the `satellite` `.deb`
and `.rpm` releases as proper APT and DNF/YUM repositories on
`https://tinkernorth.github.io/satellite/`.

## End-user install (Debian / Ubuntu)

```bash
# Add the signing key (one-time):
curl -fsSL https://tinkernorth.github.io/satellite/gpg.key \
  | sudo gpg --dearmor -o /usr/share/keyrings/satellite-archive-keyring.gpg

# Add the repo (one-time):
echo "deb [signed-by=/usr/share/keyrings/satellite-archive-keyring.gpg] \
  https://tinkernorth.github.io/satellite/debian stable main" \
  | sudo tee /etc/apt/sources.list.d/satellite.list

# Install + future upgrades via apt:
sudo apt update
sudo apt install satellite
```

## End-user install (Fedora / RHEL / Rocky / Alma)

```bash
# Drop the .repo file (it already references the gpg key). One-time:
sudo curl -fsSL -o /etc/yum.repos.d/satellite.repo \
  https://tinkernorth.github.io/satellite/rpm/satellite.repo

# Install + future upgrades via dnf:
sudo dnf install satellite
```

## How publishing works in CI

The `release.yml` workflow runs an `apt-publish` and `rpm-publish` job
after the main `publish` job succeeds. Both:

1. Check out the `gh-pages` branch into a working directory.
2. Run [`build-apt-repo.sh`](build-apt-repo.sh) or [`build-dnf-repo.sh`](build-dnf-repo.sh)
   against the artifacts in `release/`, which:
   - Drops the new `.deb` / `.rpm` into the pool / arch tree.
   - Regenerates the index metadata (`apt-ftparchive` / `createrepo_c`).
   - GPG-signs the result with the repo signing key.
3. Commit the resulting tree on the `gh-pages` branch and push.

The Pages site rebuilds automatically; users on `apt update` /
`dnf check-update` pick up the new version within seconds.

## Maintainer setup (one-time)

The CI workflow needs two repository secrets:

| Secret name        | Value |
|---|---|
| `GPG_PRIVATE_KEY`  | ASCII-armored private key (`gpg --export-secret-keys --armor "$KEYID"`) |
| `GPG_KEY_ID`       | The key's long ID or fingerprint (e.g. `B22AC1E1FE8B4A6F`) |

### Generating the signing key

Run this locally (not in CI):

```bash
gpg --batch --gen-key <<'EOF'
%echo Generating Satellite package signing key
Key-Type: EDDSA
Key-Curve: ed25519
Name-Real: Satellite Releases
Name-Email: releases@tinkernorth.invalid
Expire-Date: 0
%no-protection
%commit
%echo done
EOF

# Find the key ID:
gpg --list-secret-keys --keyid-format=long

# Export the private key (set as GPG_PRIVATE_KEY in repo secrets):
gpg --export-secret-keys --armor "$KEYID"

# Export the public key (commit as packaging/repo/gpg.key OR rely on
# the workflow to publish it from the imported keyring at sign time):
gpg --export --armor "$KEYID" > packaging/repo/gpg.key
```

### Enabling GitHub Pages

In repository **Settings → Pages**, set:

- **Source**: *Deploy from a branch*
- **Branch**: `gh-pages` / `/ (root)`

The first release after enabling will create the `gh-pages` branch
automatically and populate `https://tinkernorth.github.io/satellite/`.

## Local testing

The publishing scripts run fine standalone. Give them a scratch
`pages-dir` and a directory holding the latest `.deb` / `.rpm`:

```bash
mkdir -p /tmp/satellite-pages
export GPG_KEY_ID=$(gpg --list-secret-keys --with-colons | awk -F: '/^sec/ {print $5; exit}')
export REPO_BASE_URL=file:///tmp/satellite-pages

packaging/repo/build-apt-repo.sh /tmp/satellite-pages dist/
packaging/repo/build-dnf-repo.sh /tmp/satellite-pages dist/

# Verify the InRelease signature roundtrips:
gpg --verify /tmp/satellite-pages/debian/dists/stable/InRelease
gpg --verify /tmp/satellite-pages/rpm/x86_64/repodata/repomd.xml.asc \
            /tmp/satellite-pages/rpm/x86_64/repodata/repomd.xml
```
