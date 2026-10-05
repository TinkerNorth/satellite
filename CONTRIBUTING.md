# Contributing to Satellite

Thanks for your interest in improving Satellite! This document captures the
conventions that aren't obvious from skimming the code.

## Getting set up

```bash
# 1) Install the toolchain CI uses (Windows: scripts\install-deps.ps1)
scripts/install-deps.sh
# 2) Build + run the test suite via the CI presets
#    (Windows: scripts\build.ps1 release test)
scripts/build.sh release test
# 3) Point git at the in-tree pre-commit hook
./scripts/setup-hooks.sh
```

`CMakePresets.json` is the single source of configure truth: each CI lane
(`windows-mingw`, `linux`, `macos`, `windows-msvc`) is a preset, the
workflows and the `scripts/build.*` wrappers both drive them, and
`scripts/ci-local.ps1` / `scripts/ci-local.sh` re-run every PR gate locally
in CI's order. Add `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` on top of a preset
if you want `compile_commands.json` for clangd.

The pre-commit hook runs `clang-format -i` (autofix, re-stages) on staged
C/C++/Objective-C++ files. It skips gracefully if the tool isn't installed.
CI re-runs `clang-format --dry-run --Werror` in strict mode on Linux,
macOS, and Windows runners, so anything that slips locally fails the PR.

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
`lib/` and `vigem/include/` keep their original (MIT-compatible) licenses
and are noted in the README.

## Style

- C++17, four-space indent, 100-column soft limit. The same `.clang-format`
  ships with `dish-android` (JNI) and `dish-linux`; run `clang-format -i`
  if you're unsure.
- Hexagonal architecture: `core/` is pure domain logic with no platform
  deps. Anything that touches sockets, files, the registry, the Cocoa
  runtime, ViGEm, etc. lives in `adapters/` or `platform/<os>/`.
  `SessionService` only depends on the abstract port interfaces in
  `core/ports.h`.
- New ports must be testable from `tests/test_session_service.cpp` with a
  mock implementation. Don't add framework dependencies to the test suite;
  the tiny in-tree assertion macros are enough for the current scope.

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

These rules come from the Parchment library and are adapted where C++17 or
the hexagonal layout make the literal form worse than the thing it is meant
to achieve. They apply to `src/` and `tests/` alike, and they are the rules
the three Dish clients follow, so a fact ported between a client and this
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
  `app/app_state.h` (`g_config` and its mutex, the counters the receiver
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
- Use the PR template (`.github/pull_request_template.md`) to describe
  the change, the manual test matrix you ran (which platforms, with/without
  ViGEm), and call out anything that touches the wire protocol.
- Keep commits focused; squash noisy fixup commits before review.

## What CI runs

Build + style workflows run on every PR:

| Workflow | Runner | What it does |
|---|---|---|
| `linux-ci.yml` | ubuntu-24.04 | clang-format check, tray-enabled + headless builds, ctest, AppIndicator link verification, fuzz smoke, Sentry SDK build, build-reproducibility gate (two Release builds must match byte for byte) |
| `macos-ci.yml` | macos-15 | clang-format check, build + ctest, .app layout verification, uploads `satellite-macos-stub.app` |
| `windows-ci.yml` | windows-latest | clang-format check, MinGW MSYS2 build, ctest, redist script tests (`tests/test_fetch_redist.ps1`), HIDMaestro helper publish held to `driver_pins.h`, uploads `satellite.exe` |

All three workflows install clang-format **pinned to 22.1.4** so verdicts
match across runners, run the same `scripts/check-format.sh` gate, and
configure through the same `CMakePresets.json` presets the local scripts
drive. If any step fails, the PR is blocked. To get the same verdict before
pushing, run `scripts/ci-local.ps1` (Windows) or `scripts/ci-local.sh`
(Linux / macOS).

Security gates also run on every PR:

| Workflow | What it does |
|---|---|
| `security.yml` | action-pin lint, vulnerability allowlist expiry, OSV-Scanner against the upstream commit each `lib/VENDORED.md` pin names (`scripts/vendored-osv-lockfile.sh`), gitleaks secret scan, GitHub `dependency-review-action`, vendored-component freshness check (`lib/VENDORED.md` <= 90 days). |
| `codeql.yml` | CodeQL `cpp` analysis (security-extended + security-and-quality query packs). |

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
# Action-pin lint (40-char SHA enforcement on every uses: line)
.github/workflows/_security.yml      # source of truth: read the action-pin-lint job
# Quick local equivalent:
grep -REn '^\s*uses:' .github/workflows/ \
  | grep -vE '@[0-9a-f]{40}\b' \
  || echo "all pinned"

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

# CodeQL (requires the CodeQL CLI; see https://docs.github.com/en/code-security/codeql-cli)
codeql database create build-db --language=cpp --command='cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j'
codeql database analyze build-db --format=sarif-latest --output=codeql.sarif \
  cpp-security-extended.qls cpp-security-and-quality.qls
```

### Verifying a release artifact

Each GitHub Release ships:

- the platform installer / binary (`SatelliteSetup-X.Y.Z.exe`, `satellite-macos-stub-X.Y.Z.zip`, `satellite_X.Y.Z_amd64.deb`, `satellite-X.Y.Z-x86_64.AppImage`)
- per-artifact `*.sig` + `*.crt` (cosign keyless signature + certificate)
- `SHA256SUMS` + `SHA256SUMS.sig` + `SHA256SUMS.crt`
- `satellite.sbom.spdx.json` + `satellite.sbom.cdx.json` (Syft)
- `satellite.intoto.jsonl` (SLSA L3 provenance)

Verify with:

```bash
# 1) Checksums match
sha256sum -c SHA256SUMS

# 2) cosign verifies SHA256SUMS came from the right workflow
cosign verify-blob \
  --certificate SHA256SUMS.crt \
  --signature   SHA256SUMS.sig \
  --certificate-identity-regexp '^https://github\.com/TinkerNorth/satellite/\.github/workflows/release\.yml@refs/tags/v?[0-9].*$' \
  --certificate-oidc-issuer 'https://token.actions.githubusercontent.com' \
  SHA256SUMS

# 3) Provenance attests this artifact came from the tagged release
slsa-verifier verify-artifact \
  --provenance-path satellite.intoto.jsonl \
  --source-uri      github.com/TinkerNorth/satellite \
  --source-tag      X.Y.Z \
  SatelliteSetup-X.Y.Z.exe
```

Replace `TinkerNorth` with the GitHub org/owner. The exact recipe is also
documented in [`TinkerNorth/SECURITY.md`](SECURITY.md).

## Touching the hot path

The receiver thread runs at `recvfrom()` rate and must never allocate or
take a long lock. From `net/receiver.cpp`:

- The hot path is three syscalls with zero allocations:
  `recvfrom()` → `memcpy()` → `DeviceIoControl()` (Windows) or
  `write(uinput_fd)` (Linux).
- Mutexes guard the connection table only at packet boundaries.
- No logging on the per-packet path.

## Touching the wire protocol

The server and all three Dish clients (Android, macOS, Linux) must
produce byte-identical traffic:

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

Any change here must be coordinated with `dish-android`, `dish-linux`,
and `dish-mac` in the same PR / release cycle.

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
  `.dmp` under `%LOCALAPPDATA%\TinkerNorth\Satellite\dumps\` (from Windows
  Error Reporting for the fast-fail), an *Application Error* event in Event
  Viewer for anything Crashpad did not take over, and, on a release build
  with the switch on, an event in Sentry.
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
