# Redistributables

Third-party installers / binaries the Windows Inno Setup installer pulls in
at install time. These are **not** part of Satellite's source — they are
re-distributed unchanged from their upstream publishers.

The `.exe` / `.msi` files in this directory are intentionally **not committed**
to git (`*.exe` is covered by the root `.gitignore`). The build pipeline
fetches them on demand via [`scripts/fetch-redist.ps1`](../scripts/fetch-redist.ps1),
which verifies each download against `redist/SHA256SUMS` before letting
`iscc installer.iss` consume it.

## Inventory

Each entry MUST list:

- **Component** — human-readable name
- **Upstream** — canonical source URL
- **Pinned-version** — exact tag or release this hash was captured from
- **Filename** — the asset file on disk
- **SHA-256** — recorded in `SHA256SUMS` (this file is the pin)
- **License** — SPDX identifier
- **EOL** — `yes` (frozen upstream) / `no` (still updated)

---

### ViGEmBus

- Component: nefarius/ViGEmBus runtime installer
- Upstream: https://github.com/nefarius/ViGEmBus/releases/tag/v1.22.0
- Pinned-version: `v1.22.0` (final release — repo archived 2023-11-02)
- Filename: `ViGEmBus_1.22.0_x64_x86_arm64.exe`
- SHA-256: see `SHA256SUMS`
- License: BSD-3-Clause
- EOL: yes — upstream is archived; do not expect newer releases.

The installer's silent-install switches are the standard WixSharp Burn set
(`/quiet`, `/passive`, `/norestart`, `/uninstall`). Documented exit codes:

| Code | Meaning |
|------|---------|
| `0` | success |
| `1602` | user cancelled UAC / install |
| `1603` | fatal error during install |
| `1638` / `3011` | same-or-newer already installed (treated as success) |
| `1641` | success, restart was initiated |
| `3010` | success, reboot required |

`installer.iss` maps these into either silent success, a deferred-reboot
prompt, or a non-blocking warning that surfaces on the final wizard page.

## Updating the pin

ViGEmBus 1.22.0 is the final upstream release, so this is mostly a
historical recipe — but if a successor (e.g. NVGE) takes over and we
adopt it as the bundled driver, the bump is:

1. Update the URL + filename in [`scripts/fetch-redist.ps1`](../scripts/fetch-redist.ps1).
2. Update the `#define ViGEmBus*` block at the top of [`installer.iss`](../installer.iss).
3. Re-run `scripts/fetch-redist.ps1 -Force` to download the new asset, then
   `Get-FileHash <file> -Algorithm SHA256` and replace the line in
   `redist/SHA256SUMS` (the leading `*` is intentional — `sha256sum`'s
   binary-mode marker, accepted by both `sha256sum -c` and PowerShell's
   custom verifier in `fetch-redist.ps1`).
4. Update the inventory block in this file.
5. Build the installer end-to-end (`iscc installer.iss`) and smoke-test on
   a clean VM with no prior ViGEmBus install, with an older one, and with
   the same version — see the test matrix at the bottom of `installer.iss`.
