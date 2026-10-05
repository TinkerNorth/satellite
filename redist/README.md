# Redistributables

Third-party installers / binaries the Windows Inno Setup installer pulls in
at install time. These are **not** part of Satellite's source; they are
re-distributed unchanged from their upstream publishers.

The `.exe` / `.msi` files in this directory are intentionally **not committed**
to git (`*.exe` is covered by the root `.gitignore`). The build pipeline
fetches them on demand via [`scripts/fetch-redist.ps1`](../scripts/fetch-redist.ps1),
which verifies each download against `redist/SHA256SUMS` before letting
`iscc installer.iss` consume it.

## Inventory

Each entry MUST list:

- **Component**: human-readable name
- **Upstream**: canonical source URL
- **Pinned-version**: exact tag or release this hash was captured from
- **Filename**: the asset file on disk
- **SHA-256**: recorded in `SHA256SUMS` (this file is the pin)
- **License**: SPDX identifier
- **EOL**: `yes` (frozen upstream) / `no` (still updated)

---

### ViGEmBus

- Component: nefarius/ViGEmBus runtime installer
- Upstream: https://github.com/nefarius/ViGEmBus/releases/tag/v1.22.0
- Pinned-version: `v1.22.0` (final release; repo archived 2023-11-02)
- Driver version: `1.21.442.0` — the `ViGEmBus.sys` this release lays down.
  Upstream versions the setup bundle and the driver separately and did not
  rebuild the driver for 1.22.0, so the two numbers differ (`pnputil
  /enum-drivers` shows `08/30/2022 1.21.442.0` after installing v1.22.0).
- Filename: `ViGEmBus_1.22.0_x64_x86_arm64.exe`
- SHA-256: see `SHA256SUMS`
- License: BSD-3-Clause
- EOL: yes. Upstream is archived; do not expect newer releases.

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

### HIDMaestro

- Component: hifihedgehog/HIDMaestro SDK release
- Upstream: https://github.com/hifihedgehog/HIDMaestro/releases/tag/v1.9.2
- Pinned-version: `v1.9.2`
- Filename: `HIDMaestro-v1.9.2.zip`
- SHA-256: see `SHA256SUMS`
- License: MIT
- EOL: no. Actively maintained; bump deliberately, re-verifying the
  shared-memory layout pins in `src/platform/windows/hidmaestro_wire.h`
  (see `lib/VENDORED.md`) before adopting a new release.

Unlike ViGEmBus there is no standalone driver installer: the UMDF2 driver
is embedded inside `HIDMaestro.Core.dll` and deploys itself (certificate +
signing + pnputil) when invoked elevated. `fetch-redist.ps1` stages the SDK
assemblies into `redist/hidmaestro/` and stamps that directory with the
zip they came from (`staged-from.sha256`), so a bumped pin re-stages on
the next run instead of leaving the previous SDK in place; the directory
used to be left alone whenever its files existed, and a helper built from
it carried the 1.7.0 SDK through two pin bumps. `helper/hidmaestro`
builds the staged assemblies into the self-contained
`satellite-hm-helper.exe` that the installer bundles; publish it from a
clean `bin/` and `obj/` (`scripts/build-installer.ps1` and
`scripts/ci-local.ps1` do), because an incremental `dotnet publish` over a
re-staged SDK keeps the previous compile and bundle when the staged
assembly's timestamp is older than them. Then
`SatelliteSetup.exe` runs `satellite-hm-helper.exe install-driver`
during setup (no reboot required). `satellite-hm-helper.exe sdk-version`
prints the SDK release a built helper carries and the `hidmaestro.inf`
`DriverVer` that SDK embeds; `scripts/verify-helper-sdk.ps1` holds that
to `driver_pins.h` after every publish. The uninstaller's
`satellite-hm-helper.exe remove-driver` removes the virtual devices and
driver packages.

`HIDMaestro.Core.dll` also embeds a **second, kernel-mode** driver payload:
the usbip-win2 0.9.8.1 installers (x64 and ARM64, each verified against the
upstream release hash at build time and again before it runs), the
WHLK-certified USB transport, plus the transport's host-controller driver
files unpacked so a machine an earlier release put on 0.9.7.5 or 0.9.7.7 is
moved to 0.9.8.1 in place, with no window and no USB device dropping, in a
Windows session where the host controller has not carried a device yet.
The SDK speaks the 0.9.7.x, 0.9.8.0 and 0.9.8.1 request formats and asks
the installed driver which one it has, so a usbip-win2 another program
installed is used as it is. It is not
deployed at setup time and is not used by any input-only controller. HIDMaestro installs it the first time a *composite*
persona is created, which is what Satellite asks for when the
`controllerAudio` setting is on and the identity is a DualSense or
DualShock 4 v2 (those personas carry the pad's real USB-audio function).
`HMContext.IsUsbipBackendAvailable` / `HMContext.InstallUsbipBackend()` are
the SDK's probe and install entry points, and the helper calls them
explicitly so a blocked install is a reportable plug failure rather than a
surprise. Turning `controllerAudio` off means it is never installed; the
`controllerAudioMic` / `controllerAudioSpeaker` switches beside it do not,
since they gate the wire and the composite persona is still created.
Bumping the HIDMaestro pin therefore also bumps this transport; note the
new usbip version in the commit message.

## Updating the pin

ViGEmBus 1.22.0 is the final upstream release, so this is mostly a
historical recipe. If a successor (e.g. NVGE) takes over and we
adopt it as the bundled driver, the bump is:

1. Update the URL + filename in [`scripts/fetch-redist.ps1`](../scripts/fetch-redist.ps1).
2. Update the `#define ViGEmBus*` block at the top of [`installer.iss`](../installer.iss).
3. Download the new asset, `Get-FileHash <file> -Algorithm SHA256` it and
   replace the line in `redist/SHA256SUMS` (the leading `*` is intentional:
   `sha256sum`'s binary-mode marker, accepted by both `sha256sum -c` and
   PowerShell's custom verifier in `fetch-redist.ps1`), then re-run
   `scripts/fetch-redist.ps1`: the staging follows the new zip by itself.
4. Update the inventory block in this file.
5. Build the installer end-to-end (`iscc installer.iss`) and smoke-test on
   a clean VM with no prior driver install, with an older one, and with
   the same version (`scripts/test-installer-roundtrip.ps1` covers the
   silent path with both drivers skipped).

The same recipe applies to a HIDMaestro bump, plus one extra step: diff the
new release's `driver/driver.h` and `SharedMemoryIO.cs` against the layout
constants in `src/platform/windows/hidmaestro_wire.h` (`test_hidmaestro_wire`
pins them) — the shared-memory protocol carries no version field, so the
pin bump IS the compatibility review.

Either bump also updates `src/platform/windows/driver_pins.h`, which is what
the dashboard's driver banner compares the installed drivers against
(`versionState` in `/api/backend/status`). `version-consistency.yml` fails if
the `ViGEmBusVersion` / `ViGEmBusDriverVersion` / `HmVersion` defines in
`installer.iss` drift from it.

Both drivers are versioned separately from the release that carries them, so
each has two pins: the release/installer version, and the driver binary's own
version, which is the ONLY one worth comparing against a machine — it is what
the runtime probe reads (`ViGEmBus.sys` file version; driver-store INF
`DriverVer`) and what the installer's `/VIGEM=auto` decision uses. Pin the
release version to the wrong slot and every fresh install reports the driver
as outdated. For ViGEmBus, install the bundled setup on a clean VM and read
the number back:

```powershell
(Get-Item $env:SystemRoot\System32\drivers\ViGEmBus.sys).VersionInfo.FileVersion
```

and put that value in `SATELLITE_VIGEMBUS_BUNDLED_DRIVER_VERSION`. For
HIDMaestro (1.9.2 embeds driver `1.8.1.2248`; the two other `DriverVer`
lines in the assembly belong to the embedded usbip-win2 INFs),
`scripts/fetch-redist.ps1` reads the `hidmaestro.inf` `DriverVer` out of
the staged assembly, prints it, and fails when
`SATELLITE_HIDMAESTRO_BUNDLED_DRIVER_VERSION` or
`SATELLITE_HIDMAESTRO_SDK_VERSION` (against the zip's release number)
disagree, so the bump is: set the pins, run the script, and let it say
whether the zip agrees. `tests/test_fetch_redist.ps1` pins the parser,
the staging and the gate; `windows-ci.yml` and `scripts/ci-local.ps1` run
it. To read the value by hand:

```powershell
. scripts/redist-functions.ps1
Get-HmSdkDriverVersion redist/hidmaestro/HIDMaestro.Core.dll
```
