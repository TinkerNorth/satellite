# Vendored third-party components

Source-of-truth inventory for components vendored under
`satellite/lib/` and `satellite/vigem/include/`. Ecosystem scanners
(OSV-Scanner, Dependabot, Trivy) cannot deduce these by reading the
source tree, so we maintain this file by hand, and
`scripts/vendored-osv-lockfile.sh` hands OSV-Scanner the upstream commit
each block's `Upstream` and `Pinned-commit` resolve to.

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
- Pinned-commit: v0.58.0
- Last-vendored: 2026-09-30
- License: MIT
- Notes: vendored as a single header, byte-identical to the upstream tag
  (modulo CRLF in the working tree; git stores it LF). No local
  modifications. Used by `src/net/webserver.cpp` and the `src/net/routes_*`
  handlers. v0.51.0 -> v0.56.0 review (2026-09-20): `Headers`/`Params` are
  now an insertion-ordered container (still `find`/`end`/`->second`, which is
  all `routes_client.cpp` uses, and still constructible from an initializer
  list, which the route tests use); regex routes refuse paths longer than
  `CPPHTTPLIB_REGEX_ROUTE_PATH_MAX_LENGTH` (256) and ours carry a short
  connection id and a slot index; the SSE provider in `routes_admin.cpp`
  writes on every tick and returns false when a write fails, so the new
  no-progress rule for content providers never trips; `Error` values shifted
  and are not used numerically; `CPPHTTPLIB_NO_EXCEPTIONS` is still honoured.
  v0.56.0 -> v0.58.0 review (2026-09-29): the 0.57.0 server fixes come along
  (a chunk-size line may carry nothing but a chunk-ext, which closes a
  request-smuggling gap, and both the declared and the received trailers are
  capped), as does 0.57.1's check that a client method is a token; an OpenSSL
  connection now sends `close_notify` without waiting for the peer's, so an
  idle keep-alive client no longer holds a worker or `Server::stop()`;
  `100 Continue` now waits until the body is read and WebSocket upgrades run
  `pre_request_handler`, and Satellite has no `Expect` handler and no
  WebSocket route. No API Satellite calls changed. Re-checked 2026-09-30:
  v0.58.0 is still the newest release and the header is byte-identical to
  the tag's.

## nlohmann/json (`lib/nlohmann/json.hpp`)

- Component: nlohmann/json
- Upstream: https://github.com/nlohmann/json
- Pinned-commit: v3.12.0
- Last-vendored: 2026-09-30
- License: MIT
- Notes: vendored as the single-header amalgamation. No local modifications.
  Used project-wide via `src/core/json.h` for response building and request /
  config / GitHub-API parsing. Re-checked 2026-09-30: v3.12.0 is still the
  newest release and the header is byte-identical to its `json.hpp` asset.

## libsodium (`lib/libsodium/`)

- Component: jedisct1/libsodium
- Upstream: https://github.com/jedisct1/libsodium
- Pinned-commit: 1.0.22-RELEASE
- Last-vendored: 2026-09-30
- License: ISC
- Notes: bundled MinGW prebuilt archives (`libsodium-mingw.tar.gz`,
  `libsodium-win32/`, `libsodium-win64/`) consumed by the Windows
  toolchain. Linux + macOS link against the system package.
  `libsodium-mingw.tar.gz` is the release asset
  `libsodium-1.0.22-mingw.tar.gz` from the 1.0.22-RELEASE GitHub release,
  SHA-256 `1d99e0afaf27bce664249232e9dc628ae6bb7b49f0ab53ad6db372b028a35d9d`,
  minisign-verified against the libsodium release key
  (`RWQf6LRCGA9i53mlYecO4IzT51TGPpvWucNSCh1CBM0QTaLn73Y7GFO3`), and the two
  directories are its contents unpacked. Until 2026-09-20 the archive was a
  2026-04-16 rebuild of the same version (identical headers, different
  `libsodium.a`/`libsodium-26.dll` bytes); it was replaced with the tagged
  release bytes so the pin names what is actually here. Re-checked
  2026-09-30: 1.0.22 is still the newest release, the archive's SHA-256 is
  the one above, and the two directories are its contents (the working
  tree's line endings aside, which git normalises).

## ViGEm Bus Driver SDK (`vigem/include/ViGEm/`)

- Component: ViGEm/ViGEmClient (the SDK submodule of nefarius/ViGEmBus)
- Upstream: https://github.com/ViGEm/ViGEmClient
- Pinned-commit: v1.21.222.0 (driver ABI targeted: ViGEmBus v1.22.0)
- Last-vendored: 2026-09-30 (upstream archived 2023-09-08 at b66d02d; each
  re-stamp is a check that nothing moved upstream, and nothing has)
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
  Review (2026-09-29) against the header ViGEmBus v1.22.0 builds with (its
  `sdk` submodule, ViGEmClient cb8c9f4, whose `BusShared.h` is the same blob
  as v1.21.222.0's and b66d02d's): the two notification requests had drifted
  from it. `IOCTL_DS4_REQUEST_NOTIFICATION` is write-only (the driver's DMF
  table matches the whole code, access bits included, and failed the
  read-write code we sent, so DS4 rumble and lightbar never came back),
  `XUSB_REQUEST_NOTIFICATION` carries `LargeMotor, SmallMotor, LedNumber` (we
  read the LED slot as the small motor), and the DS4 notification carries the
  small motor first. `test_vigem_adapter` now pins every IOCTL code, both
  notification sizes and both layouts.

## HIDMaestro shared-memory protocol (`src/platform/windows/hidmaestro_wire.h`)

- Component: hifihedgehog/HIDMaestro (driver shared-memory + report contracts)
- Upstream: https://github.com/hifihedgehog/HIDMaestro
- Pinned-commit: v2.1.0
- Last-vendored: 2026-11-01
- License: MIT
- Notes: not copied source — a hand-written re-statement of the driver's
  shared-memory contract (`driver/driver.h` `HIDMAESTRO_SHARED_INPUT` /
  `HIDMAESTRO_SHARED_OUTPUT` and `SharedMemoryIO.cs`): section layouts,
  seqlock discipline, output-ring geometry and Source codes, plus the
  per-profile report byte layouts in `hidmaestro_report.h`
  (xbox-360-wired, dualshock-4-v2, dualsense, switch-pro) and the Sony
  calibration scale (16 LSB/deg/s, 8192 LSB/g) the SDK's
  `profiles/models/sony-dualsense.json` and `sony-dualshock4.json` answer.
  The input and output layouts carry NO version field (v2.0.0 added a
  separate per-controller `Global\HIDMaestroVersion<N>` section, plus
  `InputAck`, `Signal` and remove events, which the SDK creates and this
  backend never opens), so a pin bump REQUIRES diffing those upstream files
  against the constants here (`test_hidmaestro_wire` /
  `test_hidmaestro_report` pin them; the section sizes 362 / 16904 are the
  runtime layout check). The runtime driver itself deploys via the bundled
  `satellite-hm-helper.exe` from the hash-pinned release zip (see
  `redist/README.md`); we ship no driver code.
  v1.9.2 -> v2.1.0 (2026-11-01): `HIDMAESTRO_SHARED_INPUT` and
  `HIDMAESTRO_SHARED_OUTPUT` are byte-identical (362 / 8 + 64 x 264) and
  every Source code is the same, so the wire pins hold. What moved is
  around the sections. The driver holds no device code: a Sony pad's
  calibration, pairing and firmware answers come from the profile's model,
  compiled by the SDK at create and served only to a controller the SDK
  created, and the stub the 1.9.2 driver answered (+/-10000 everywhere, so
  20 LSB/deg/s and 10000 LSB/g) became the identity table (+/-8000 gyro,
  +/-8192 accel, speed 500), so `sonyGyroFromWire` / `sonyAccelFromWire`
  replace the old x40000/32767. The Switch Pro handshake, SPI image and
  0x30 stream run in the SDK's protocol host from a 48-byte state block
  (the body `packSwitchBody` already produced), so the helper relays
  Satellite's block to `SubmitRawReport` instead of letting Satellite write
  the section; the catalog's simple report (0x3F) goes out on change only,
  which leaves an idle pad silent and makes Steam's SDL2-era Switch driver
  drop it after three seconds, so the helper's `satellite-switch-pro` copy
  (cut from the pinned `profiles/nintendo/switch-pro.json`, see
  `redist/README.md`) sends it on the 15 ms clock instead, as the 1.x
  driver did before a handshake. The four Sony profiles carry a 4 ms `stream`, an SDK idle
  repeat that writes the input section whether or not anything was
  submitted; the helper ships copies without it (`helper/hidmaestro-core`
  `SatelliteProfiles`, exported from the SDK at publish and loaded back
  through it). A controller's sections sit at the SDK's transport index
  (`HMController.TransportIndex`), which is the slot only while no other
  program holds that number. `RemoveAllVirtualControllers` now removes the
  devices of exited programs only, `InstallDriver` upgrades in place or
  beside a running program's older driver and never downgrades
  (`HMContext.LastDriverInstall`), and `HMUninstall.Run` is the removal,
  refused while another HIDMaestro program runs. The SDK's own driver INF is
  2.1.0.1716 (`driver_pins.h`); 2.0.0 to 2.0.3 share 2.0.0.2201 and a 2.0.x
  SDK runs on the 2.1.0 driver. `satellite-hm-helper.exe selftest`, run
  elevated, checks the idle repeat, the stream-free copies, the Switch Pro
  state block and the section names against the installed driver.
  v1.7.0 -> v1.9.0 (2026-09-20): `driver/driver.h` and
  `Internal/SharedMemoryIO.cs` changed only in comment punctuation; every
  offset, size, ring constant and Source code is the same, so the pins here
  are unchanged and the compatibility review is the diff itself.
  v1.9.0 -> v1.9.2 (2026-10-04): the upstream compare touches nothing under
  `driver/` and not `Internal/SharedMemoryIO.cs`, and the only profile that
  moved is `valve/steam-controller-2.json`, which this backend never plugs,
  so every pin here still holds. What changed is beside the contract: the
  bundled USB transport for the composite personas is usbip-win2 0.9.8.1
  (was 0.9.7.5), the client speaks the 0.9.7.x, 0.9.8.0 and 0.9.8.1 request
  formats, and the SDK's own driver INF is 1.8.1.2248 (`driver_pins.h`).
