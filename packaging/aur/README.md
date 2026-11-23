# Arch User Repository: `satellite-bin`

This directory holds the [PKGBUILD](PKGBUILD) and [.SRCINFO](.SRCINFO)
for the `satellite-bin` package. Today they feed Satellite's own signed
pacman repository, not the [AUR](https://aur.archlinux.org/): AUR account
registration is closed, so `satellite-bin` is not on the AUR.
`publish-repos.yml`'s `arch-publish` job, which release.yml calls after every
stable tag publishes, builds this PKGBUILD in an Arch container against the
release's assets (it rewrites `pkgver` and the checksums at build time, which
is why the `pkgver=1.0.0` here is a placeholder) and publishes it under
`https://tinkernorth.github.io/satellite/arch/$arch`. The files stay
AUR-ready for when registration reopens.

## How users install

From the pacman repository (the steps
[`packaging/repo/index.html`](../repo/index.html) publishes):

```bash
curl -fsSL https://tinkernorth.github.io/satellite/gpg.key | sudo pacman-key --add -
sudo pacman-key --lsign-key 4371E67B00CB54E69ED34D70428695F4ED61D3A8
printf '\n[satellite]\nSigLevel = Required DatabaseOptional\nServer = https://tinkernorth.github.io/satellite/arch/$arch\n' \
  | sudo tee -a /etc/pacman.conf
sudo pacman -Syu satellite-bin
```

Or build it yourself from this directory with `makepkg -si`.

The package re-bundles the official x86_64 AppImage under `/opt/satellite/`
and ships a `satellite` shim in `/usr/bin` that forwards to it. The
in-app OTA updater detects that install lineage and shows a manual
upgrade instruction (`sudo pacman -Syu satellite-bin`) rather than trying
to swap the binary itself.

## How maintainers would bump it on the AUR

Nothing in CI pushes to the AUR: AUR pushes need a per-maintainer SSH key,
which does not belong in CI secrets. Once registration reopens and the
package exists there, after a release publishes:

1. Pull the freshly-tagged files:
   ```bash
   git clone ssh://aur@aur.archlinux.org/satellite-bin.git aur-satellite-bin
   cd aur-satellite-bin
   cp ../packaging/aur/PKGBUILD ../packaging/aur/.SRCINFO .
   ```
2. Bump `pkgver=` in `PKGBUILD` and the matching lines in `.SRCINFO` to
   the new release tag (`echo X.Y.Z > /tmp/v && ...`).
3. Pull the AppImage SHA-256 from the release's `SHA256SUMS` asset and
   replace the first `SKIP` in `sha256sums_x86_64`:
   ```bash
   curl -fsSL "https://github.com/TinkerNorth/satellite/releases/download/${VER}/SHA256SUMS" \
     | grep "satellite-${VER}-x86_64.AppImage$" | cut -d' ' -f1
   ```
   (The `.desktop` and udev-rule SHA-256s can stay `SKIP` because they
   pin to the same tag and would be embarrassing to fail-closed on for
   trivial whitespace changes.)
4. Regenerate `.SRCINFO`:
   ```bash
   makepkg --printsrcinfo > .SRCINFO
   ```
5. Verify the package builds:
   ```bash
   makepkg -fs
   ```
6. Commit + push:
   ```bash
   git add PKGBUILD .SRCINFO
   git commit -m "Update to ${VER}"
   git push
   ```

## Why `-bin` instead of building from source

An Arch source package (`satellite`) would need to pull `libcurl-dev`,
`libsodium-dev`, `opus` headers, `gtk3` headers, `libayatana-appindicator`
headers, and `cmake` at build time. The AppImage is already bundled and
code-signed upstream, so the `-bin` flavor is faster to install, exercises the same
artifact every other Linux distribution channel exercises, and matches
the conventions of similar AUR packages (e.g. `signal-desktop-bin`,
`obsidian-bin`).

Power users who want a source build can clone the upstream repository
and run `scripts/build.sh`.
