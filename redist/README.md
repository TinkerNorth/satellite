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
- Upstream: https://github.com/hifihedgehog/HIDMaestro/releases/tag/v2.1.0
- Pinned-version: `v2.1.0`
- Filename: `HIDMaestro-v2.1.0.zip`
- SHA-256: see `SHA256SUMS`
- License: MIT
- EOL: no. Actively maintained; bump deliberately, re-verifying the
  shared-memory layout pins in `src/platform/windows/hidmaestro_wire.h`
  (see `lib/VENDORED.md`) before adopting a new release.

### HIDMaestro catalog profile `dualsense`

- Component: hifihedgehog/HIDMaestro `profiles/sony/dualsense.json`
- Upstream: https://github.com/hifihedgehog/HIDMaestro/blob/v2.1.0/profiles/sony/dualsense.json
- Pinned-version: `v2.1.0` (the same tag as the SDK zip; `fetch-redist.ps1`
  refuses a file named for another release)
- Filename: `HIDMaestro-v2.1.0-dualsense.json`
- SHA-256: see `SHA256SUMS`
- License: MIT
- EOL: no. The text the SDK zip embeds; the helper cuts
  `satellite-dualsense` from it (see the profile copies below).

### HIDMaestro catalog profile `dualsense-composite`

- Component: hifihedgehog/HIDMaestro `profiles/sony/dualsense-composite.json`
- Upstream: https://github.com/hifihedgehog/HIDMaestro/blob/v2.1.0/profiles/sony/dualsense-composite.json
- Pinned-version: `v2.1.0` (the same tag as the SDK zip; `fetch-redist.ps1`
  refuses a file named for another release)
- Filename: `HIDMaestro-v2.1.0-dualsense-composite.json`
- SHA-256: see `SHA256SUMS`
- License: MIT
- EOL: no. The text the SDK zip embeds; the helper cuts
  `satellite-dualsense-composite` from it (see the profile copies below).

### HIDMaestro catalog profile `dualsense-edge`

- Component: hifihedgehog/HIDMaestro `profiles/sony/dualsense-edge.json`
- Upstream: https://github.com/hifihedgehog/HIDMaestro/blob/v2.1.0/profiles/sony/dualsense-edge.json
- Pinned-version: `v2.1.0` (the same tag as the SDK zip; `fetch-redist.ps1`
  refuses a file named for another release)
- Filename: `HIDMaestro-v2.1.0-dualsense-edge.json`
- SHA-256: see `SHA256SUMS`
- License: MIT
- EOL: no. The text the SDK zip embeds; the helper cuts
  `satellite-dualsense-edge` from it (see the profile copies below).

### HIDMaestro catalog profile `dualsense-edge-composite`

- Component: hifihedgehog/HIDMaestro `profiles/sony/dualsense-edge-composite.json`
- Upstream: https://github.com/hifihedgehog/HIDMaestro/blob/v2.1.0/profiles/sony/dualsense-edge-composite.json
- Pinned-version: `v2.1.0` (the same tag as the SDK zip; `fetch-redist.ps1`
  refuses a file named for another release)
- Filename: `HIDMaestro-v2.1.0-dualsense-edge-composite.json`
- SHA-256: see `SHA256SUMS`
- License: MIT
- EOL: no. The text the SDK zip embeds; the helper cuts
  `satellite-dualsense-edge-composite` from it (see the profile copies below).

### HIDMaestro catalog profile `dualshock-4-v2`

- Component: hifihedgehog/HIDMaestro `profiles/sony/dualshock-4-v2.json`
- Upstream: https://github.com/hifihedgehog/HIDMaestro/blob/v2.1.0/profiles/sony/dualshock-4-v2.json
- Pinned-version: `v2.1.0` (the same tag as the SDK zip; `fetch-redist.ps1`
  refuses a file named for another release)
- Filename: `HIDMaestro-v2.1.0-dualshock-4-v2.json`
- SHA-256: see `SHA256SUMS`
- License: MIT
- EOL: no. The text the SDK zip embeds; the helper cuts
  `satellite-dualshock-4-v2` from it (see the profile copies below).

### HIDMaestro catalog profile `dualshock-4-v2-composite`

- Component: hifihedgehog/HIDMaestro `profiles/sony/dualshock-4-v2-composite.json`
- Upstream: https://github.com/hifihedgehog/HIDMaestro/blob/v2.1.0/profiles/sony/dualshock-4-v2-composite.json
- Pinned-version: `v2.1.0` (the same tag as the SDK zip; `fetch-redist.ps1`
  refuses a file named for another release)
- Filename: `HIDMaestro-v2.1.0-dualshock-4-v2-composite.json`
- SHA-256: see `SHA256SUMS`
- License: MIT
- EOL: no. The text the SDK zip embeds; the helper cuts
  `satellite-dualshock-4-v2-composite` from it (see the profile copies below).

### HIDMaestro catalog profile `switch-pro`

- Component: hifihedgehog/HIDMaestro `profiles/nintendo/switch-pro.json`
- Upstream: https://github.com/hifihedgehog/HIDMaestro/blob/v2.1.0/profiles/nintendo/switch-pro.json
- Pinned-version: `v2.1.0` (the same tag as the SDK zip; `fetch-redist.ps1`
  refuses a file named for another release)
- Filename: `HIDMaestro-v2.1.0-switch-pro.json`
- SHA-256: see `SHA256SUMS`
- License: MIT
- EOL: no. The text the SDK zip embeds; the helper cuts
  `satellite-switch-pro` from it (see the profile copies below).

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
during setup (no reboot required): since HIDMaestro 2.0 that upgrades an
older driver in place, or installs this build's packages beside it while
another program's controllers still use the old one, and never downgrades a
newer one (`HMContext.LastDriverInstall` says which, and the helper prints
it). `satellite-hm-helper.exe sdk-version` prints the SDK release a built
helper carries and the `hidmaestro.inf` `DriverVer` that SDK embeds;
`scripts/verify-helper-sdk.ps1` holds that to `driver_pins.h` after every
publish. The uninstaller's `satellite-hm-helper.exe remove-driver` runs
HIDMaestro's own removal (`HMUninstall.Run`, the same code as the
`HIDMaestroUninstall.exe` in the release zip): every virtual controller, the
driver packages, the certificate and the registry keys go, the shared
Windows keys are put back, and the removal is refused with exit code 2
while another HIDMaestro program is running, which the installer reports.
Exit code 3 means removed with a file Windows deletes at the next start.
`--include-usbip` also takes what usbip-win2's own installer left (its
root-hub filter restarts every USB 3 root hub), which the installer never
passes.

The zip's own `LICENSE` and `THIRD-PARTY-NOTICES.txt` are staged beside
the assemblies and `installer.iss` installs them as `LICENSE-HIDMaestro.txt`
and `THIRD-PARTY-NOTICES-HIDMaestro.txt`. The helper is published
self-contained, so `HIDMaestro.Core.dll` ships inside it, and MIT requires
the notice to travel with the binary; upstream asked for this in
[issue #141](https://github.com/TinkerNorth/satellite/issues/141). A pin
bump carries both along by itself, because the staging takes them from the
same hash-pinned zip as the assemblies.

The publish also writes `hidmaestro-profiles\` beside the helper:
Satellite's copies of seven catalog profiles, as `satellite-<id>`, written
by `satellite-hm-helper.exe export-profiles` and loaded back by
`check-profiles`, which fails the publish on any load issue, a missing
device model or a changed descriptor; `installer.iss` installs the folder,
and the helper refuses to plug a profile whose copy is not loaded rather
than fall back to the catalog entry. All seven are cut from the catalog
files at the SDK's tag, `profiles/<vendor>/<id>.json`, which
`fetch-redist.ps1` fetches as `HIDMaestro-v<ver>-<id>.json` (pinned in
`SHA256SUMS`, inventoried above) and stages in
`redist/hidmaestro/profiles/`, and each differs from its file only in its
`id`, its `notes` and one edit; the helper's tests hold every copy to its
pinned file. The SDK embeds the repository's `profiles/` folder as it is
(`HIDMaestro.Core.csproj` at v2.1.0), so a file at the tag is the text the
SDK runs, and each of the seven is byte-identical to the resource the
2.1.0 zip's `HIDMaestro.Core.dll` carries. Every publish checks that this
still holds: `export-profiles` reads the profile `HIDMaestro.Core.dll`
embeds for each copy (`HIDMaestro.Profiles.<dir>\<id>.json`) and cuts no
copy when a staged file is not that profile as parsed JSON (line endings
and a byte order mark aside), or when the resource is missing or renamed.
The copies are not written back out of the SDK: its serializer cannot
round-trip `protocol`, `stateBlock`, `inputReports` or `liveFeatureReports`,
which a shared parser reads instead (`ControllerProfile.cs` at v2.1.0), as
HIDMaestro's author confirmed on
[issue #141](https://github.com/TinkerNorth/satellite/issues/141), and it
writes out every key a file leaves out, as null or as its default.

The six Sony profiles Satellite writes natively (`dualsense`,
`dualsense-composite`, `dualsense-edge`, `dualsense-edge-composite`,
`dualshock-4-v2`, `dualshock-4-v2-composite`) lose their `stream` key:
since 2.0 those profiles carry an idle repeat (4 ms, 1 ms on the Edge) that
the SDK runs itself, writing a frame into the controller's input section
whether or not the program submitted one, which would race Satellite's own
writer on the same seqlock. The Switch Pro is the other way round: the
SDK's protocol host answers the Switch handshake and streams report 0x30
from a 48-byte state block, so the helper hands satellite a frame mailbox
of the input section's layout and relays each block to `SubmitRawReport`.
Its copy's one edit sends the simple report (0x3F) on the 15 ms device
clock instead of on change, the stream the 1.x driver had before a
handshake, and that is a deliberate divergence from the hardware: a real
Pro Controller pushes 0x3F only when a button changes (dekuNukem's
Nintendo Switch reverse-engineering notes), and HIDMaestro keeps its
catalog faithful to the hardware by design. The hosts are why. SDL 2.26
and 2.30 reopen a Bluetooth Switch controller that has been silent for
three seconds, in every input mode (`SDL_hidapi_switch.c`, the "Bluetooth
may have disconnected" branch); SDL3 exempts simple mode; and Steam, whose
Switch driver is not public, was measured dropping an idle pad that the
catalog profile left silent. Revisit the edit once Steam's driver exempts
simple mode as SDL3 does. `helper/hidmaestro-core` holds the mailbox
reader, the export and the profile transforms, with their own
`dotnet test` suite, which reads the staged catalog files and so runs
after `fetch-redist.ps1`.
`satellite-hm-helper.exe selftest`, run as administrator on a machine with
the driver, creates each profile and checks all of this against the real
SDK and driver: that the catalog `dualsense` streams on its own, that the
Sony copies and `xbox-360-wired` do not, that the Switch Pro copy streams
0x3F before any handshake and 0x30 after a host selects full mode and 0x3F
again after it selects simple mode, that it refuses a 47-byte block and
keeps streaming after a 48-byte one, that the Sony copies answer their
calibration and pairing feature reports through the UMDF2 driver, and that
every section name the helper opens is one the SDK lists for the
controller's transport index.

`HIDMaestro.Core.dll` also embeds a **second, kernel-mode** driver payload:
usbip-win2 0.9.8.1's host-controller driver package (x64 and ARM64, each
verified against the upstream release hash at build time and again before
it is used), the WHLK-certified USB transport. Since HIDMaestro 2.0.1 the
SDK installs that host controller from its own package and never runs
usbip-win2's installer, so no USB root hub restarts; a host controller an
earlier release put on 0.9.7.5 or 0.9.7.7 is moved to 0.9.8.1 in place in a
Windows session where it has not carried a device yet. The SDK speaks the
0.9.7.x, 0.9.8.0 and 0.9.8.1 request formats and asks the installed driver
which one it has, so a usbip-win2 another program installed is used as it
is. It is not deployed at setup time and is not used by any input-only
controller. HIDMaestro installs it the first time a *composite* persona is
created, which is what Satellite asks for when the `controllerAudio`
setting is on and the identity is a DualSense or DualShock 4 v2 (those
personas carry the pad's real USB-audio function).
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

1. Update the URL + filename in the redist table (`Get-RedistTable` in
   [`scripts/redist-functions.ps1`](../scripts/redist-functions.ps1)).
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

The same recipe applies to a HIDMaestro bump, plus four extra steps: diff
the new release's `driver/driver.h` and `SharedMemoryIO.cs` against the
layout constants in `src/platform/windows/hidmaestro_wire.h`
(`test_hidmaestro_wire` pins them) — the input and output layouts carry no
version field, so the pin bump IS the compatibility review; move the seven
catalog profile entries in the table to the new tag and re-pin each
`HIDMaestro-v<ver>-<id>.json` in `SHA256SUMS` from the file at that tag,
after which the helper's `dotnet test` suite holds every copy to its new
file and fails on an edit the new file no longer needs, and the publish
refuses a file that is not the profile the new DLL embeds; read the publish's
`check-profiles` output, which is the SDK loading the seven copies; and
run `satellite-hm-helper.exe selftest` elevated on a machine with the
driver, which is the only place the idle repeat, the Switch Pro state block
and the transport index can be observed.

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
HIDMaestro (2.1.0 embeds driver `2.1.0.1716`; the two other `DriverVer`
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
