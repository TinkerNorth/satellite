# Third-party notices

Satellite is licensed [LGPL-3.0-or-later](LICENSE). This file lists every
third-party component that is compiled into a Satellite build, linked by it,
shipped beside it in a release, or used only to build and test it, and states
what someone redistributing the binary has to do.

The exact pin of each vendored component, its upstream commit and the review
of every bump live in [`lib/VENDORED.md`](lib/VENDORED.md); the Windows
redistributables are inventoried in [`redist/README.md`](redist/README.md).
`scripts/check-version-consistency.sh` holds the versions in the table below
to the files that pin them.

---

## 1. Summary

| Component | Version | SPDX | How it reaches the user | Attribution obligation |
|---|---|---|---|---|
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | 0.67.0 | `MIT` | Compiled in on every platform from the vendored single header `lib/httplib.h` (the admin and client HTTP servers). | Keep the copyright and permission notice below |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.12.0 | `MIT` | Compiled in on every platform from `lib/nlohmann/json.hpp`. | Keep the copyright and permission notice below |
| [libsodium](https://github.com/jedisct1/libsodium) | 1.0.22 | `ISC` | Windows: linked statically, the vendored MinGW archive under `lib/libsodium/` (MinGW lane) or vcpkg at the baseline (the shipped MSVC build). Linux: the distribution's package, dynamically. macOS: Homebrew's dylib from `/opt/homebrew` (the stub does not bundle it). The Flatpak template builds the same version. | Keep the copyright and permission notice below |
| [opus](https://opus-codec.org/) | 1.6.1 | `BSD-3-Clause` | Windows MSVC: linked statically from vcpkg at the baseline. MinGW lane: MSYS2's package. Linux: the distribution's package. macOS: Homebrew's dylib. | Keep the copyright notice and conditions below |
| [OpenSSL](https://www.openssl.org/) | 3.6.5 (vcpkg baseline, the shipped Windows build) | `Apache-2.0` | Windows MSVC: linked statically from vcpkg. Linux and macOS: the system's or Homebrew's libssl/libcrypto, dynamically. The MinGW CI lane links MSYS2's rolling package (4.x today), which ships nothing. | Apache-2.0: keep the license and NOTICE text with a redistributed binary |
| [sentry-native](https://github.com/getsentry/sentry-native) | 0.17.6 | `MIT` | Compiled in only when a release build carries a reporting address (`SATELLITE_SENTRY_DSN`): the vcpkg overlay port on Windows (with Crashpad, `crashpad_handler.exe` and `crashpad_wer.dll` beside `satellite.exe`), FetchContent on Linux (inproc backend). See [`PRIVACY.md`](PRIVACY.md). The SDK bundles further code under its own licenses (Crashpad and mini_chromium `BSD-3-Clause`, libunwind `MIT`, jsmn, mpack and stb_sprintf), which vcpkg's port copyright file lists. | Keep the notices below and the SDK's bundled licenses |
| [HIDMaestro](https://github.com/hifihedgehog/HIDMaestro) | 2.3.0 | `MIT` | Windows only. `HIDMaestro.Core.dll` (the SDK release zip, hash-pinned in `redist/SHA256SUMS`) inside the published `satellite-hm-helper.exe`, plus seven profile copies cut from its catalog files. The SDK embeds its UMDF2 driver (release 2.1.0, INF 2.1.0.1716) and usbip-win2 0.9.8.1's host-controller package, installed on the user's machine by the helper. | Keep the copyright and permission notice; usbip-win2 (`BSD-2-Clause`) carries its own notice inside the SDK |
| [ViGEmBus](https://github.com/nefarius/ViGEmBus) | 1.22.0 | `BSD-3-Clause` | Windows only. The upstream installer, unchanged, bundled by the Inno Setup installer and run once at machine scope. Upstream is archived. | Keep the copyright notice and conditions |
| [ViGEm client headers](https://github.com/ViGEm/ViGEmClient) | v1.21.222.0 | `MIT` | Windows only. A hand-maintained subset of the IOCTL definitions under `vigem/include/ViGEm/`, compiled in, with local corrections (`lib/VENDORED.md`). | Keep the copyright and permission notice below |
| Linux system libraries: libcurl, GTK 3, libayatana-appindicator, libnotify | the distribution's | `curl`, `LGPL-2.1-or-later`, `LGPL-3.0-only` / `LGPL-2.1-only`, `LGPL-2.1-or-later` | Linked dynamically from the user's distribution (the AppImage carries the copies linuxdeploy bundles). The tray libraries are optional at build time. | None beyond the libraries' own terms for a dynamic link |
| [Kenney Input Prompts](https://kenney.nl/assets/input-prompts) | the pack as vendored | `CC0-1.0` | The controller artwork under `web/img/icons/ctrl-*.svg` and `web/img/catalog/`, recolored. See [`DESIGN.md`](DESIGN.md). | None (public domain dedication) |
| [Inter](https://rsms.me/inter/) via Google Fonts | served by Google | `OFL-1.1` | Not shipped. `web/style.css` imports it from `fonts.googleapis.com` when the dashboard is opened in a browser that can reach it; otherwise the system font is used. | None for redistributors |
| [vcpkg](https://github.com/microsoft/vcpkg) | 277978ad2eddc3b05ecf288dcd33071968cd2f79 | `MIT` | Build only: the builtin-baseline in `vcpkg.json` that fixes the MSVC lane's dependency versions. Not in the app. | None for redistributors of the app |
| [clang-format](https://pypi.org/project/clang-format/) | 23.1.5 | `Apache-2.0 WITH LLVM-exception` | Build and CI only, the hash-pinned PyPI wheel `scripts/tools.sh` installs. Not in the app. | None for redistributors of the app |
| [clang-tidy](https://pypi.org/project/clang-tidy/) | 23.1.4 | `Apache-2.0 WITH LLVM-exception` | Build only, as the pinned PyPI wheel `scripts/tools.sh` fetches (`scripts/check-tidy.sh`). Not in any artifact. | None for redistributors |
| [sentry-cli](https://github.com/getsentry/sentry-cli) | 3.8.2 | `FSL-1.1-MIT` | Release CI only, the hash-pinned binary `scripts/tools.sh` fetches to upload debug symbols. Not in the app. | None for redistributors of the app |
| [linuxdeploy](https://github.com/linuxdeploy/linuxdeploy) | 1-alpha-20251107-1 | `MIT` | Build only, the hash-pinned AppImage `scripts/tools.sh` fetches to assemble Satellite's AppImage. Not in the app. | None for redistributors of the app |
| [Inno Setup](https://jrsoftware.org/isinfo.php) | 6.7.4 | Inno Setup License | Build only, the hash-pinned installer `scripts/install-inno-setup.ps1` installs to compile `SatelliteSetup.exe`. Its setup runtime is part of every Windows installer. | None beyond the Inno Setup License |
| [cosign](https://github.com/sigstore/cosign) | 3.1.4 | `Apache-2.0` | Release and pull-request CI only (`_release_harden.yml`), as a pinned release binary: signs every release file into a Sigstore bundle. Not in any artifact. | None for redistributors |
| [Syft](https://github.com/anchore/syft) | 1.57.2 | `Apache-2.0` | Release and pull-request CI only (`_release_harden.yml`): writes the SPDX and CycloneDX SBOMs. Not in any artifact. | None for redistributors |
| [Grype](https://github.com/anchore/grype) | 0.122.1 | `Apache-2.0` | Release and pull-request CI only (`_release_harden.yml`): scans the release files. Not in any artifact. | None for redistributors |
| [OSV-Scanner](https://github.com/google/osv-scanner) | 2.7.1 | `Apache-2.0` | CI and `scripts/ci-local.sh` only (`_security.yml`, `scripts/tools.sh`). Not in any artifact. | None for redistributors |
| [gitleaks](https://github.com/gitleaks/gitleaks) | 8.30.1 | `MIT` | CI and `scripts/ci-local.sh` only (`_security.yml`, `scripts/tools.sh`). Not in any artifact. | None for redistributors |

Everything else under `web/` (the dashboard, its glyphs and the bundled
controller database under `web/controllers/v1/`, a snapshot of the one
https://dish.tinkernorth.com serves) is first-party TinkerNorth work covered
by this repository's own license.

## 2. Reused facts, not reused code

These published facts are restated in Satellite's own code, not copied from
an upstream source file:

- **The XUSB (XInput) gamepad report**, from Microsoft's XInput
  documentation: the 12-byte little-endian report the wire's INPUT carries
  and the button bits (`docs/contract.md`).
- **ViGEmBus's IOCTL interface**, restated in `vigem/include/ViGEm/` from
  ViGEmClient and corrected against the header ViGEmBus v1.22.0 builds with
  (the notification requests and layouts `lib/VENDORED.md` records).
- **HIDMaestro's shared-memory and report contracts**: the driver's input and
  output section layouts, the seqlock, the output ring and its Source codes,
  the per-controller version section, and the per-profile report byte
  layouts (`src/platform/windows/hidmaestro_wire.h`,
  `src/platform/windows/hidmaestro_report.h`,
  `src/core/hidmaestro_version.h`), re-stated from `driver/driver.h`,
  `driver/hm_contract.h` and `SharedMemoryIO.cs` and diffed on every bump.
- **The DualShock 4 v2 USB report** (`src/core/ds4_report.h`), whose
  reference in the tree is `DS4_REPORT_EX` in the ViGEm headers, shared by
  the macOS backend and the HIDMaestro packer.
- **SDL's Linux mapping row for the Switch Pro Controller** (057e:2009),
  which `src/platform/linux/gamepad_adapter.cpp` follows so SDL numbers the
  virtual pad's buttons the way it numbers a real one. A device fact, not
  SDL code.

## 3. Notices

### cpp-httplib

```
Copyright (c) 2026 Yuji Hirose. All rights reserved.
MIT License
```

The full MIT permission notice is at the top of `lib/httplib.h`.

### nlohmann/json

```
SPDX-FileCopyrightText: 2013 - 2025 Niels Lohmann <https://nlohmann.me>
SPDX-License-Identifier: MIT
```

### libsodium

```
ISC License

Copyright (c) 2013-2026
Frank Denis <j at pureftpd dot org>
```

The full text is libsodium's `LICENSE`
(<https://github.com/jedisct1/libsodium/blob/master/LICENSE>).

### opus

```
Copyright 2001-2023 Xiph.Org, Skype Limited, Octasic,
                    Jean-Marc Valin, Timothy B. Terriberry,
                    CSIRO, Gregory Maxwell, Mark Borgerding,
                    Erik de Castro Lopo, Mozilla, Amazon
```

The full BSD-3-Clause text is opus's `COPYING`
(<https://opus-codec.org/license/>).

### sentry-native

```
Copyright (c) 2019 Sentry (https://sentry.io) and individual contributors.
All rights reserved.
```

MIT; the full text is the SDK's `LICENSE`.

### ViGEm client headers

```
Copyright (c) 2016-2023 Nefarius Software Solutions e.U. and Contributors
```

MIT, as stated at the top of each file under `vigem/include/ViGEm/`.

### HIDMaestro and ViGEmBus

Both ship as their upstream release files with their own `LICENSE`
(HIDMaestro: MIT, including `THIRD-PARTY-NOTICES.txt` in the release zip;
ViGEmBus: BSD-3-Clause), installed unchanged.
