# Vendored third-party components

Source-of-truth inventory for components vendored under
`satellite/lib/` and `satellite/vigem/include/`. Ecosystem scanners
(OSV-Scanner, Dependabot, Trivy) cannot deduce these by reading the
source tree, so we maintain this file by hand and feed it to OSV-Scanner
through `satellite/osv-scanner.toml`.

Each component MUST list:

- **Component**: human-readable name
- **Upstream**: canonical repo URL
- **Pinned-commit**: 40-char SHA or release tag we vendored from
- **Last-vendored**: YYYY-MM-DD (must be within 90 days; CI fails otherwise)
- **License**: SPDX identifier
- **Notes**: any local modifications

If you bump a component, update its block here in the same PR. The
`vendored-freshness` job in `.github/workflows/security.yml` enforces
the 90-day freshness window.

---

## cpp-httplib (`lib/httplib.h`)

- Component: yhirose/cpp-httplib
- Upstream: https://github.com/yhirose/cpp-httplib
- Pinned-commit: v0.51.0
- Last-vendored: 2026-07-25
- License: MIT
- Notes: vendored as a single header, byte-identical to the upstream tag
  (modulo CRLF in the working tree; git stores it LF). No local
  modifications. Used by `src/net/webserver.cpp` and the `src/net/routes_*`
  handlers.

## nlohmann/json (`lib/nlohmann/json.hpp`)

- Component: nlohmann/json
- Upstream: https://github.com/nlohmann/json
- Pinned-commit: v3.11.3
- Last-vendored: 2026-06-19
- License: MIT
- Notes: vendored as the single-header amalgamation. No local modifications.
  Used project-wide via `src/core/json.h` for response building and request /
  config / GitHub-API parsing.

## libsodium (`lib/libsodium/`)

- Component: jedisct1/libsodium
- Upstream: https://github.com/jedisct1/libsodium
- Pinned-commit: 1.0.22-RELEASE
- Last-vendored: 2026-07-25
- License: ISC
- Notes: bundled MinGW prebuilt archives (`libsodium-mingw.tar.gz`,
  `libsodium-win32/`, `libsodium-win64/`) consumed by the Windows
  toolchain. Linux + macOS link against the system package.

## ViGEm Bus Driver SDK (`vigem/include/ViGEm/`)

- Component: ViGEm/ViGEmClient (the SDK submodule of nefarius/ViGEmBus)
- Upstream: https://github.com/ViGEm/ViGEmClient
- Pinned-commit: v1.21.222.0 (driver ABI targeted: ViGEmBus v1.22.0)
- Last-vendored: 2026-07-25
- License: MIT
- Notes: **not** a verbatim upstream copy — a hand-maintained minimal
  subset of `include/ViGEm/Common.h` and `include/ViGEm/km/BusShared.h`,
  reduced to the IOCTLs and structs needed to talk to ViGEmBus directly
  via `DeviceIoControl`, plus local corrections that upstream does not
  carry (notably: the extended DS4 report is submitted through
  `IOCTL_DS4_SUBMIT_REPORT` 0x202 and dispatched by buffer size — there
  is no `_EX` submit IOCTL; the 0x205 code is retained only as an unused
  historical note). **Do not "refresh" these by copying upstream over
  them** — that would silently reintroduce the motion-data bug. Re-verify
  by diffing intent, not bytes. The runtime driver itself is installed by
  ViGEmBus's own installer on the user's machine; we ship no driver code.

## HIDMaestro shared-memory protocol (`src/platform/windows/hidmaestro_wire.h`)

- Component: hifihedgehog/HIDMaestro (driver shared-memory + report contracts)
- Upstream: https://github.com/hifihedgehog/HIDMaestro
- Pinned-commit: v1.7.0
- Last-vendored: 2026-08-24
- License: MIT
- Notes: not copied source — a hand-written re-statement of the driver's
  shared-memory contract (`driver/driver.h` `HIDMAESTRO_SHARED_INPUT` /
  `HIDMAESTRO_SHARED_OUTPUT` and `SharedMemoryIO.cs`): section layouts,
  seqlock discipline, output-ring geometry and Source codes, plus the
  per-profile report byte layouts in `hidmaestro_report.h`
  (xbox-360-wired, dualshock-4-v2, dualsense, switch-pro) and the neutral
  Sony calibration scale (20 LSB/deg/s, 10000 LSB/g) the driver's
  feature-report stubs imply. The protocol carries NO version field, so a
  pin bump REQUIRES diffing those upstream files against the constants
  here (`test_hidmaestro_wire` / `test_hidmaestro_report` pin them; the
  section sizes 362 / 16904 are the runtime layout check). The runtime
  driver itself deploys via the bundled `satellite-hm-helper.exe` from the
  hash-pinned release zip (see `redist/README.md`); we ship no driver code.
