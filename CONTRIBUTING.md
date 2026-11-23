# Contributing to Satellite

Thanks for your interest in improving Satellite! This document captures the
conventions that aren't obvious from skimming the code.

## Code of conduct

This project follows the [Contributor Covenant](CODE_OF_CONDUCT.md). By
participating, you agree to uphold it. Report unacceptable behavior to
`security@tinkernorth.com`.

## Getting set up

```bash
# 1) Install the toolchain CI uses (Windows: scripts\install-deps.ps1)
scripts/install-deps.sh
# 2) Build and run the test suite through the CI preset
#    (Windows: scripts\build.ps1 release test)
scripts/build.sh release test
# 3) Point git at the in-tree pre-commit hook
./scripts/setup-hooks.sh
# 4) Run every CI gate against your worktree, in CI's order
scripts/ci-local.sh            # Windows: scripts\ci-local.ps1
```

`CMakePresets.json` is the single source of configure truth: `release`
(the Linux and macOS lane; `scripts/build.sh` and `scripts/ci-local.sh`),
`debug`, `package` (what every shipped Linux artifact and the macOS bundle
are built from), and, where the toolchain differs, `windows-mingw`,
`windows-mingw-debug` and `windows-msvc` (the hardened MSVC + vcpkg build the
release ships; `scripts/setup-vcpkg.ps1` checks vcpkg out at the commit
`vcpkg.json`'s `builtin-baseline` names). Every preset sets warnings as
errors. Add `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` on top of a preset if you
want `compile_commands.json` for clangd.

`scripts/ci-local.sh` (Linux and macOS) runs what the main CI jobs run:

| Flag | Adds or changes |
|---|---|
| (none) | format, action pins, version consistency, vendored freshness, core purity, preset `release` build and ctest, and on macOS the `.app` checks and the alive smoke |
| `--quick` | only the gates before the build |
| `--with-packages` | the `.deb` and `.rpm` lanes in debian:trixie and fedora:44 (`scripts/package-in-container.sh`, docker) |
| `--with-appimage` | the AppImage lane in ubuntu:22.04 (docker) |
| `--with-bundle` | macOS: the release bundle lane, unsigned |
| `--allow-missing` | a missing tool becomes a notice instead of a failure |

`scripts/ci-local.ps1` (Windows) runs `windows-ci.yml`'s gates;
`-Quick` stops before the build, `-WithMsvc` adds the MSVC build, its tests,
the app smoke and the unsigned installer with its round-trip (elevated), and
`-AllowMissing` is the same downgrade. Without it, a gate whose tool is
missing fails: a green run must not hide a skipped gate.

The pre-commit hook formats the staged C, C++ and Objective-C++ files with
the clang-format `scripts/tools.sh` pins and re-stages them; CI runs the same
formatter in check mode, so anything that slips locally fails the PR.

## Where code goes

- Hexagonal architecture: `core/` is pure domain logic with no platform
  deps. Anything that touches sockets, files, the registry, the Cocoa
  runtime, ViGEm, etc. lives in `adapters/` or `platform/<os>/`.
  `SessionService` only depends on the abstract port interfaces in
  `core/ports.h`. `scripts/check_core_purity.sh` holds `src/core/` to the
  standard library, other core headers and nlohmann/json.
- New ports must be testable from `tests/test_session_service.cpp` with a
  mock implementation. Don't add framework dependencies to the test suite;
  the tiny in-tree assertion macros are enough for the current scope.
- The design is in [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md), the
  threading rules in [`docs/threading.md`](docs/threading.md), and the wire
  contract in [`docs/contract.md`](docs/contract.md).

## Touching the UI

The dashboard under `web/` takes every color from the tokens
[`DESIGN.md`](DESIGN.md) lists. What a page reads of the server is held to
what the server writes by `tests/test_dashboard_contract.cpp`,
`tests/test_debug_ui_contract.cpp` and `tests/test_update_ui_contract.cpp`.

## Translations

The dashboard's strings live in `web/lang/<locale>.json` (`en`, `bs`, `de`,
`es`, `fr`, `pt-BR`), with English as the source. A string the dashboard
looks up must exist in all six: the dashboard contract tests check each new
surface's strings against every locale, and the controller-type strings are
asserted against the real catalogs.

## License headers

Every source file (`*.h`, `*.cpp`, `*.mm`) starts with a single SPDX line:

```cpp
// SPDX-License-Identifier: LGPL-3.0-or-later
```

That one line is the whole header. No per-file copyright blurb or
description block (the path names the file; authorship lives in git). New
files must include the SPDX line. Don't introduce code under a different
license: the project is LGPL-3.0-or-later end-to-end (`LICENSE`,
`COPYING.GPL3`, source headers). Vendored third-party components under
`lib/` and `vigem/include/` keep their original licenses (MIT and ISC), listed
with every other third-party component in [`THIRD_PARTY.md`](THIRD_PARTY.md).

## Style

- C++20, four-space indent, 100-column soft limit. The same `.clang-format`
  ships with `dish-android` (JNI), `dish-windows` and `dish-linux`; run
  `scripts/check-format.sh --fix` to format with the pinned clang-format.

### Comments: why, not what

Comments justify why, not what. Names and types already describe
behaviour; a comment earns its place only by explaining a decision,
constraint, or surprise the code can't show. Keep them short; a
one-liner is the norm. This mirrors the `dish-android` discipline.

**Keep** (condensed to a line where you can): a rationale that isn't
deducible from the code; a constraint a type can't express (units,
ranges, threading / lock order, ownership, wire-format byte layouts,
RFC citations); a workaround for a specific platform or library bug; a
surprising invariant or security note.

**Don't add**: narration that restates the code (`// increment counter`),
history (git owns it), commented-out code, decorative box-drawing
section dividers, file-header description blocks, or `TODO`/`FIXME`
markers (open an issue instead). Durable design rationale belongs in
`docs/` (architecture, protocol) or `SECURITY.md`, not in a comment
essay at the top of a file.

### Shape of the code

These rules come from the Parchment library and are adapted where C++20 or
the hexagonal layout make the literal form worse than the thing it is meant
to achieve. They apply to `src/` and `tests/` alike, and they are the rules
the Dish clients follow, so a fact ported between a client and this
server keeps its shape.

- **As immutable and as static as possible.** `const` on every local and
  parameter that is not reassigned, `constexpr` for a value known at compile
  time, and file-static (or an anonymous namespace) for anything the rest of
  the translation unit does not need. A value that never changes is a named
  constant, never a literal in the middle of a function. A type that holds no
  state is a set of free functions, not a class.

- **Split values into simple, named steps.** One operation per line, with the
  result in a named `const` that says what it is, even when that reads longer:

  ```cpp
  const bool forInstance = nameEqCi(q.name, instanceFqdn) && ownsType(q.type);
  const bool forHost = nameEqCi(q.name, hostFqdn) && q.type == mdns::TYPE_A;
  return mdns::questionMatchesService(q) || forInstance || forHost;
  ```

  not one five-term boolean. The names are the documentation, the debugger can
  show each value, and a test can pin each step. A named `const bool` costs
  nothing at runtime, on the hot path included.

- **One function, one flow.** When a function would hold two algorithms chosen
  by a condition, the condition dispatches to two named things that each do
  one thing, and the dispatcher does nothing else. A `switch` over an enum
  with no `default` is the preferred form, because the compiler then checks
  that every case is handled. A guard clause is not an algorithm: do not
  invent indirection where there is only one flow.

  A function that stays long because splitting it would make it worse says so
  at the top, in one or two lines. A long function with no such note is one
  nobody has looked at.

- **A chain of `if`s over one byte is a table.** A per-bit or per-index
  mapping belongs in a `constexpr` array the code reads, not in a switch the
  reader has to diff against its twin. The point is that two mappings of the
  same thing cannot drift apart. The same goes for a route table: the
  `register*Routes` function binds paths to named handlers and does nothing
  else, so the list of routes reads as a list.

- **A callback with a body gets a name.** A lambda is fine as a one-expression
  forward, and fine as an argument to an algorithm that consumes it
  immediately (`std::sort`, `std::find_if`). A lambda that carries an
  algorithm becomes a named function, so it can be found, read and tested on
  its own. A callback that is stored rather than called immediately (an
  `httplib` route handler, a thread body, a `std::function` member) prefers a
  named function and a pointer to it. This is Parchment's "no anonymous
  methods" narrowed to what C++ can express.

- **No singletons.** A stateless helper is a free function in the file that
  owns it. There is no `instance()` and no function-local `static` holding
  state. The one process-wide object is the app state declared in
  `app/app_state.h` (`g_configStore`, the counters the receiver
  thread bumps at packet rate, the log ring), defined once per platform in
  its `globals.cpp`; it is there because the receiver, the routes and the
  tray are three threads over one state and the hot path cannot afford an
  indirection to reach it. Nothing else is a global, and new code takes what
  it needs as a parameter, which is also what makes it replaceable in a test
  (`SessionService` and its ports are the model).

- **No discarded results.** A value is either used or not produced. A
  `static_cast<void>(x)` or `(void)x` exists only to quiet a warning about
  something that should not be there, so the warning is the thing to fix.

- **Member naming.** Members carry a TRAILING underscore (`mtx_`, `ports_`,
  `next_`), which is what every class in `src/` uses. It is the same "state,
  not scratch" signal Parchment's `m` prefix gives at the point of use.
  Process-wide state is `g_`, and only in `app/app_state.h`.

- **Prefer a test to a comment.** Behaviour that needs explaining gets a test
  named for the behaviour. A comment is the last resort for a constraint that
  genuinely cannot be tested (a platform quirk, a wire-format byte layout, a
  lock order, an RFC clause), states why in one or two lines, and never
  narrates what the next line does. The "why, not what" rule above is the
  same rule from the other side.

## Tests

### Test-driven, every flow

The suite under `tests/` uses the in-tree `TEST` / `EXPECT` / `EXPECT_EQ`
macros from `tests/test_util.h`, one executable per file, registered through
`satellite_add_pure_test` (or its route and platform siblings) in
`CMakeLists.txt`. It is not a coverage exercise; it is how a flow is known to
work at all.

- **Red first.** Write the failing test, watch it fail for the reason you
  expect, then write the code. A test that has never failed has not been
  shown to test anything; when adding a test for behaviour that already
  exists, break the behaviour on purpose and watch the test catch it.
- **Cover every flow.** Each branch a function can take gets a `TEST` named
  for the behaviour it pins, not for the function it calls. A block with its
  own `TEST` label is the right tool when cases share a fixture; a separate
  file is right when they need a different one.
- **Assume nothing.** Where behaviour depends on a platform, a library
  version or the wire, prove it with a probe before writing the code that
  assumes it, and name the probe's finding in the test.
- **Test where the behaviour lives.** `src/core/` is platform-free so it is
  testable on every lane, including the two (`windows-mingw`, `windows-msvc`)
  that cannot build the route tests. Logic that decides what a route or a
  thread does belongs in `core/` as a pure function of what it was given, with
  the socket, the file or the registry call left in the adapter that owns it.
  A decision that needs `httplib::Server` to be tested is a decision with a
  dependency it should not have.
- **Knowledge written twice is checked by a test.** When the same fact lives
  in two places (a wire constant and `docs/contract.md`, a JSON field the
  server writes and the one the dashboard reads, a table and its twin in a
  Dish client), a test walks one and requires the other, from the serialized
  form rather than from a third list kept in the test.
- **Tests follow the same shape rules.** A fixture is a named type, not a
  lambda that builds one; a helper with a body gets a name; a test that is
  longer than the thing it tests is usually two tests.

## Branching & PRs

- All changes land on `main` via pull request; no direct pushes.
- Commits and PR descriptions name no tool. No `Co-Authored-By` trailer for an
  assistant, no "Generated with …" footer: the author of record is the person
  who opened the PR, whatever helped write it.
- Use the PR template (`.github/pull_request_template.md`) to describe
  the change, the manual test matrix you ran (which platforms, with/without
  ViGEm), and call out anything that touches the wire protocol.
- Keep commits focused; squash noisy fixup commits before review.

## What CI runs

Every pull request runs these; a failure blocks the merge.

| Workflow | Job (required-check name) | What it does |
|---|---|---|
| `linux-ci.yml` | `Build, test, lint, format (linux)` | `scripts/check-format.sh`, the core purity gate, preset `release` build and ctest, the uinput smoke as root, the AppIndicator link check |
| | `Verify headless fallback build` | The build and tests without the tray libraries |
| | `Fuzz smoke (decrypt->dispatch robustness, bounded)` | clang and libFuzzer: the committed corpus, then a 10 s run |
| | `Build reproducibility` | Two preset `package` builds from differently named directories must be bit-identical |
| | `Build with the Sentry SDK (dummy DSN)` | The only pull-request build that compiles and links sentry-native |
| | `Package (.deb, Debian 13) and launch it` | In debian:trixie: `scripts/build-deb.sh`, then `scripts/check-package.sh deb` (contents, lintian, scriptlets, web payload, install, launch, the bundled controller database's start-up line, exit 0 on SIGTERM) |
| | `Package (.rpm, Fedora 44) and launch it` | The same in fedora:44 with `scripts/build-rpm.sh` |
| | `AppImage (as the release builds it)` | On ubuntu-22.04 (the glibc floor): `scripts/build-appimage.sh` with its content check, then the AppImage stays up headless |
| `macos-ci.yml` | `Build, test, lint, format (macos)` | On macos-15 with Xcode 16.4: the format check, preset `release` build and ctest, the `.app` layout, a 45 s alive smoke, uploads `satellite-macos-stub.app` |
| | `macOS bundle (as the release builds it, unsigned)` | `scripts/package-macos.sh build`, `stage` and `check`: the release's `.dmg` and `.zip`, the `.dmg` mounted and its app run |
| `windows-ci.yml` | `Build, test, lint, format (windows)` | MSYS2 MINGW64: the format check, preset `windows-mingw` build and ctest, the redist and measurements script tests, the HIDMaestro helper's tests and publish held to `driver_pins.h` |
| `windows-msvc-ci.yml` | `Build, test (MSVC + vcpkg, hardened)` | The shipping build: vcpkg at the manifest's baseline (`scripts/setup-vcpkg.ps1`), preset `windows-msvc`, ctest, the app smoke, the CFG and CET bits read back with dumpbin, then `scripts/build-installer.ps1` (unsigned) and the installer's silent install, launch, repair and uninstall round-trip, then a `-DSATELLITE_UIACCESS=ON` rebuild and its installer compiled to inspect (an unsigned UIAccess build cannot start) |
| `version-consistency.yml` | `Version, pins and documented facts agree` | `scripts/check-version-consistency.sh`: the version and its mirrors, the CHANGELOG heading, the driver pins, every repeated tool and dependency pin, the protocol number and C++ standard the documents name |
| `security.yml` | `shared / ...` (action-pin lint, allowlist expiry, OSV-Scanner, dependency review, gitleaks) | The shared `_security.yml` gates; OSV-Scanner reads the upstream commit each `lib/VENDORED.md` pin names (`scripts/vendored-osv-lockfile.sh`) |
| | `Vendored components <= 90 days old` | `scripts/check-vendored-freshness.sh` |
| `codeql.yml` | `Analyze (cpp)` | CodeQL `security-extended` and `security-and-quality` over the preset `release` build; blocking, results uploaded to code scanning |
| `measurements.yml` | `Measure (MSVC + vcpkg, Release)` | Pull requests touching `src/`, `tools/bench/`, the lock-law test or `CMakeLists.txt`: the frame-path benches and the lock laws, with the report's gates |

`release.yml` and `publish-repos.yml` run only for a tag or a dispatch; see
"Cutting a release". Every build lane configures through a
`CMakePresets.json` preset, and every gate is a script under `scripts/` that
the workflow and `scripts/ci-local.sh` / `scripts/ci-local.ps1` both call.
What the local scripts cannot run is listed in their headers with the
reason: the root uinput smoke and the AppIndicator check (the runner's
kernel module and package set), the headless, fuzz, reproducibility and
Sentry jobs (their own toolchains), CodeQL, the shared security gates and the
measurements (GitHub's services or the Windows runner), and the MSVC lane's
dumpbin read-back and UIAccess compile.

The format gate uses clang-format **23.1.5**, the hash-checked PyPI wheel
`scripts/tools.sh` pins; `scripts/check-format.sh` fetches it on first use,
so every runner and every local run formats with the same binary.

An OSV advisory whose git range covers a vendored component's pinned
commit fails the next PR, and the Monday run, without code changes. OSV
cannot place every advisory on a release commit: libsodium tags its
releases on a branch its fixes do not descend from, so CVE-2025-69277
matches libsodium's master but neither 1.0.18-RELEASE nor 1.0.20-RELEASE,
though both are affected. The hand review in "Updating a vendored
component" covers what the gate cannot see. To verify the gate locally
before pushing, see "Running security checks locally" below.

## Security

### Adding a vulnerability allowlist entry

Open a PR that adds an entry to [`.security/allowlist.yaml`](.security/allowlist.yaml):

```yaml
exceptions:
  - cve: CVE-YYYY-NNNNN
    reason: "Specific, reachable-codepath analysis. Cite the call graph or upstream PR."
    owner: "@github-handle"
    expires: 2026-07-01
```

CI rejects the PR if any field is missing, if `expires` is in the past,
or if it's more than 90 days in the future. Allowlist entries require a
security-team review label before merge. Renew or remove on or before
`expires`; there's no silent suppression.

If the same CVE is also flagged by OSV-Scanner, add an `[[IgnoredVulns]]`
block to [`osv-scanner.toml`](osv-scanner.toml) referencing the same
expiration so both tools agree.

### Updating a vendored component

[`lib/VENDORED.md`](lib/VENDORED.md) is the source-of-truth inventory
for everything under `lib/` and `vigem/include/`. When you bump
cpp-httplib, libsodium, or the ViGEm headers, in the same PR:

1. Update the component block in `lib/VENDORED.md` (commit SHA / version
   tag, `Last-vendored:` set to today's date).
2. Run the OSV gate locally (see below): `scripts/vendored-osv-lockfile.sh`
   must resolve every pin, and OSV-Scanner must report no advisories.
3. Review what the gate cannot see: the upstream's GitHub security
   advisories (`gh api repos/<owner>/<repo>/security-advisories`), NVD, and
   every release note between the old pin and the new one.

The `vendored-freshness` CI step fails if any `Last-vendored:` date is
more than 90 days old, so quarterly refreshes happen even when no
upstream advisory has fired.

### Updating a runtime redistributable (`redist/`)

`redist/` holds third-party binaries that the Windows Inno Setup installer
ships: the ViGEmBus driver installer, and the HIDMaestro SDK release that
`helper/hidmaestro` builds `satellite-hm-helper.exe` from. These are not
vendored under `lib/`: they're not source we compile against, they're
prebuilt binaries from upstream that we re-distribute unchanged. The pin
lives in [`redist/SHA256SUMS`](redist/SHA256SUMS) and the inventory is in
[`redist/README.md`](redist/README.md). The staged SDK is held to
`src/platform/windows/driver_pins.h` by `scripts/fetch-redist.ps1` and the
published helper by `scripts/verify-helper-sdk.ps1`;
`tests/test_fetch_redist.ps1` pins both and runs in `windows-ci.yml` and
`scripts/ci-local.ps1`.

The `vendored-freshness` 90-day check intentionally does not cover
`redist/`. ViGEmBus is end-of-life upstream (repo archived 2023-11) and
will not get a newer release, so a freshness alarm would be noise. If a
successor (NVGE) supersedes it, the bump procedure is documented in
`redist/README.md` and lives in the same PR as the `installer.iss`
`#define` change.

### Running security checks locally

```bash
# Action-pin lint (40-char SHA on every uses: line), as _security.yml runs it
bash scripts/check-action-pins.sh

# Vendored components <= 90 days old, as security.yml's vendored-freshness job runs it
bash scripts/check-vendored-freshness.sh

# Allowlist expiry
python3 - <<'PY'
import datetime, yaml, sys
data = yaml.safe_load(open('.security/allowlist.yaml').read()) or {}
today = datetime.date.today()
for e in data.get('exceptions', []) or []:
    if datetime.date.fromisoformat(str(e['expires'])) < today:
        print('EXPIRED:', e); sys.exit(1)
PY

# OSV-Scanner against the vendored components' pinned upstream commits
bash scripts/vendored-osv-lockfile.sh > "${TMPDIR:-/tmp}/vendored-osv.json"
osv-scanner --config=osv-scanner.toml --lockfile "osv-scanner:${TMPDIR:-/tmp}/vendored-osv.json"

# Gitleaks
gitleaks detect --no-banner --redact --source .
```

`scripts/ci-local.sh` and `scripts/ci-local.ps1` run the first two.

### Verifying a release artifact

Each GitHub Release ships the platform installers and packages
(`SatelliteSetup-X.Y.Z.exe`, `satellite-macos-stub-X.Y.Z.dmg` and `.zip`,
`satellite_X.Y.Z_amd64.deb`, `satellite-X.Y.Z-1.x86_64.rpm`,
`satellite-X.Y.Z-x86_64.AppImage`, each with an `-unsigned` suffix where no
platform certificate signed it, plus their stable-name copies), a cosign
`.sig` and `.crt` for every file, `SHA256SUMS` with its own signature, the
SPDX and CycloneDX SBOMs and `satellite.intoto.jsonl`. The verification
recipe, with the certificate identity the signatures carry, is in
[`SECURITY.md`](SECURITY.md), "Verifying a release artifact".

## Cutting a release

`release.yml` runs on a bare `MAJOR.MINOR.PATCH` tag (no `v`; a prerelease
such as `1.2.3-rc.1` publishes marked as a prerelease) or a
`workflow_dispatch` with a tag input.

1. **Bump the version** in a pull request of its own: `VERSION`,
   `src/core/version.h`, `vcpkg.json`'s `version`, a new `<release>` at the
   top of `packaging/io.github.tinkernorth.satellite.metainfo.xml`, and the
   `## X.Y.Z` heading in `CHANGELOG.md`. `version-consistency.yml` fails the
   PR when one of them disagrees. Update `PRIVACY.md` first if anything about
   data collection changed.
2. **Merge it**, then push a lightweight tag on the squash commit:
   `git tag X.Y.Z <sha> && git push origin X.Y.Z`.
3. **The workflow then runs in this order**: the tag is checked against
   `VERSION` and the CHANGELOG heading; every pull-request security gate
   re-runs against the tagged commit; the signing-secret report (advisory:
   without the certificates the artifacts carry `-unsigned`); the builds
   (the Windows installer, the macOS stub, the `.deb`, the `.rpm`, the
   AppImage, each through the script its pull-request lane runs, each
   installed or launched before upload); the shared hardening (Grype,
   the SBOMs, `SHA256SUMS`, cosign); the SLSA provenance; the release
   uploaded as a draft; the measurements attached; the draft flipped to
   published; the Sentry release; and, for a stable tag, the APT, DNF and
   pacman repositories regenerated on `gh-pages` by `publish-repos.yml`.

**The fleet order.** The website (dish-website) is tagged first. Satellite
and the Dish clients are tagged once
`https://dish.tinkernorth.com/controllers/v1/manifest.json` serves the
controller database the release carries.

**After a bad release.** The repository has no rollback procedure. What the
tree gives you: a release is published only after every asset is attached
(draft, then flip), so a failure before the flip leaves the previous release
as `latest`; the stable names (`SatelliteSetup.exe`, `satellite-amd64.deb`,
`satellite-x86_64.rpm`, `satellite-x86_64.AppImage`) under
`releases/latest/download/` follow whichever release is newest, so
demoting or deleting a bad release moves them back; and
`gh workflow run publish-repos.yml --ref main -f tag=X.Y.Z` regenerates the
package repositories for a named release
([`packaging/repo/README.md`](packaging/repo/README.md)).

## Touching the hot path

The receiver thread runs at `recvfrom()` rate and must never allocate or
take a long lock. From `net/receiver.cpp`:

- The hot path is three syscalls with zero allocations:
  `recvfrom()` → `memcpy()` → `DeviceIoControl()` (Windows) or
  `write(uinput_fd)` (Linux).
- Mutexes guard the connection table only at packet boundaries.
- No logging on the per-packet path.

## Touching the wire protocol

Satellite and the five Dish clients (Dish for Android, Dish for iOS, Dish
for macOS, Dish for Windows, Dish for Linux) must produce byte-identical
traffic:

- AEAD: ChaCha20-Poly1305 IETF. The 12-byte nonce carries a direction
  byte in byte 0 and the per-session counter (big-endian) in bytes 8..11;
  counters restart per session.
- Packet layout: `token(4) | counter(4) | ciphertext+tag`, with the
  4-byte big-endian token as AAD.
- XUSB report: 12 bytes, little-endian.
- Ports: streaming UDP 9876, HTTPS client API (pairing + sessions) 9443,
  discovery via mDNS on 5353 with a legacy UDP beacon on 9879. The admin
  HTTP UI on 9877 is loopback-only and not part of the wire contract. See
  [`docs/contract.md`](docs/contract.md) for the authoritative port map.

Any change here must be coordinated with `dish-android`, `dish-ios`,
`dish-mac`, `dish-windows` and `dish-linux` in the same release cycle.

## Platform notes

- **Windows** is the canonical target, with virtual gamepads via ViGEmBus.
  The Inno Setup installer (`installer.iss`) bundles ViGEmBus 1.22.0 as
  a prerequisite; building the installer requires running
  `pwsh scripts/fetch-redist.ps1` first (verifies SHA-256 against
  `redist/SHA256SUMS`), then `iscc installer.iss`.
  `scripts/build-installer.ps1` chains both (plus the helper publish,
  env-gated signing, and the version pass-through from `/VERSION`).
  The installer accepts two unattended-install switches,
  documented in the README and in the comment blocks at the top of
  `installer.iss`:
    - `/VIGEM=auto|bundled|skip` (install path): override the bundled-driver auto-detect.
    - `/REMOVEVIGEM=auto|yes|no` (uninstall path): control whether the
      driver is removed alongside Satellite. Default `auto` prompts in
      attended uninstalls and leaves the driver alone in silent uninstalls.
  To prove the crash pipeline end to end on a machine, run
  `satellite.exe /crash-test` (an unhandled access violation: Satellite's
  own minidump writer, then Crashpad), `satellite.exe /crash-test=abort`
  (`abort()`, the route every `std::terminate` takes: the SIGABRT hook, then
  Crashpad) or `satellite.exe /crash-test=fastfail` (a fast-fail, which only
  Windows Error Reporting and the `crashpad_wer.dll` module can see). Each
  runs beside an already-running Satellite, keeps its Sentry state under
  `sentry\crash-test` and honours the *Share crash reports* switch. Expect a
  `.dmp` under `%LOCALAPPDATA%\TinkerNorth\Satellite\dumps\` from
  Satellite's own writer for the first two, and from Windows Error Reporting
  for the fast-fail while Crashpad is not armed; an *Application Error* event
  in Event Viewer for the fast-fail, and for the other two while Crashpad is
  not armed; and, on a release build with the switch on, a report under
  `sentry\crash-test\reports\` and an event in Sentry for all three. A source
  build carries no DSN, so start it with `SENTRY_DSN` set to aim the test at
  a scratch project. The fast-fail route needs Windows 10 version 2004
  (build 19041) or later.
- **Linux** synthesizes virtual gamepads through `/dev/uinput`. Optional
  tray icon via libayatana-appindicator (CMake auto-detects; falls back
  to a headless `sigwait` loop).
- **macOS** is a stub: no signed DriverKit equivalent of ViGEmBus is
  available, so the build runs the protocol stack but applies every
  controller descriptor as `backendUnavailable`. The CI artifact
  is named `satellite-macos-stub.app` to make this explicit, and the
  binary logs a banner at startup.

## Reporting bugs

Use the issue templates under `.github/ISSUE_TEMPLATE/`. Include the
OS and version, the relevant log excerpt (Windows:
`%LOCALAPPDATA%\TinkerNorth\Satellite\logs\`, plus any `.dmp` from the
`dumps\` folder beside it; Linux: `journalctl --user -e`), and which Dish
client is connecting.
