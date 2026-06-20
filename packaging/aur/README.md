# Arch User Repository: `satellite-bin`

This directory holds the [PKGBUILD](PKGBUILD) and [.SRCINFO](.SRCINFO)
files that mirror Satellite to the [AUR](https://aur.archlinux.org/) as
the `satellite-bin` package. The AUR repository itself is a separate
git remote: the files here are the source of truth, and a release-time
job syncs them across.

## How users install

```bash
# Via an AUR helper
yay -S satellite-bin
# or
paru -S satellite-bin

# Or manually with makepkg
git clone https://aur.archlinux.org/satellite-bin.git
cd satellite-bin
makepkg -si
```

The package re-bundles the official x86_64 AppImage under `/opt/satellite/`
and ships a `satellite` shim in `/usr/bin` that forwards to it. The
in-app OTA updater detects the AUR install lineage and surfaces the
right manual upgrade instruction (`yay -Syu satellite-bin`) rather than
trying to swap the binary itself.

## How maintainers bump it

The `release.yml` workflow regenerates the SHA-256 sums on tag, but does
**not** push to the AUR: AUR pushes require a per-maintainer SSH key
which we don't want sitting in CI secrets. After a release publishes:

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
   curl -fsSL "https://github.com/TinkerNorth/satellite/releases/download/v${VER}/SHA256SUMS" \
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
   git commit -m "Update to v${VER}"
   git push
   ```

## Why `-bin` instead of building from source

An Arch source package (`satellite`) would need to pull `libcurl-dev`,
`libsodium-dev`, `gtk3` headers, `libayatana-appindicator` headers, and
`cmake` at build time. The AppImage is already bundled and code-signed
upstream, so the `-bin` flavor is faster to install, exercises the same
artifact every other Linux distribution channel exercises, and matches
the conventions of similar AUR packages (e.g. `signal-desktop-bin`,
`obsidian-bin`).

Power users who want a source build can clone the upstream repository
and run the standard `./build-satellite.sh` flow.
