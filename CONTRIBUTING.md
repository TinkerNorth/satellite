# Contributing to Satellite

Thanks for your interest in improving Satellite! This document captures the
conventions that aren't obvious from skimming the code.

## Getting set up

```bash
# 1) Install build deps for your platform (see README "Prerequisites")
# 2) Generate compile_commands.json + run the test suite
cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
# 3) Point git at the in-tree pre-commit hook
./scripts/setup-hooks.sh
```

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
| `linux-ci.yml` | ubuntu-24.04 | clang-format check, tray-enabled + headless builds, ctest, AppIndicator link verification, advisory build-reproducibility check |
| `macos-ci.yml` | macos-14 | clang-format check, build + ctest, .app layout verification, uploads `satellite-macos-stub.app` |
| `windows-ci.yml` | windows-latest | clang-format check, MinGW MSYS2 build, ctest, uploads `satellite.exe` |

All three workflows install clang-format **pinned to 22.1.4** so verdicts
match across runners. If any step fails, the PR is blocked.

Security gates also run on every PR:

| Workflow | What it does |
|---|---|
| `security.yml` | action-pin lint, vulnerability allowlist expiry, OSV-Scanner against `lib/` + `vigem/include/` (with the synthetic [`osv-scanner.toml`](osv-scanner.toml) lockfile), gitleaks secret scan, GitHub `dependency-review-action`, vendored-component freshness check (`lib/VENDORED.md` <= 90 days). |
| `codeql.yml` | CodeQL `cpp` analysis (security-extended + security-and-quality query packs). |

A new high-severity CVE published against any vendored component causes
the next PR to fail without code changes. To verify the gate locally
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
2. Update the matching `[[PackageOverride]]` version in
   [`osv-scanner.toml`](osv-scanner.toml).
3. Re-run OSV-Scanner locally (see below) to confirm no new advisories.

The `vendored-freshness` CI step fails if any `Last-vendored:` date is
more than 90 days old, so quarterly refreshes happen even when no
upstream advisory has fired.

### Updating a runtime redistributable (`redist/`)

`redist/` holds third-party installers that the Windows Inno Setup
installer chains in at install time (currently just the ViGEmBus driver).
These are not vendored under `lib/`: they're not source we compile
against, they're prebuilt binaries from upstream that we re-distribute
unchanged. The pin lives in [`redist/SHA256SUMS`](redist/SHA256SUMS) and
the inventory is in [`redist/README.md`](redist/README.md).

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

# OSV-Scanner against vendored components
osv-scanner --recursive --skip-git --lockfile=osv-scanner.toml lib vigem/include

# Gitleaks
gitleaks detect --no-banner --redact --source .

# CodeQL (requires the CodeQL CLI; see https://docs.github.com/en/code-security/codeql-cli)
codeql database create build-db --language=cpp --command='cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j'
codeql database analyze build-db --format=sarif-latest --output=codeql.sarif \
  cpp-security-extended.qls cpp-security-and-quality.qls
```

### Verifying a release artifact

Each GitHub Release ships:

- the platform installer / binary (`SatelliteSetup-vX.Y.Z.exe`, `satellite-macos-stub-vX.Y.Z.zip`, `satellite_X.Y.Z_amd64.deb`, `satellite-X.Y.Z-x86_64.AppImage`)
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
  --certificate-identity-regexp '^https://github\.com/TinkerNorth/satellite/\.github/workflows/release\.yml@refs/tags/v.*$' \
  --certificate-oidc-issuer 'https://token.actions.githubusercontent.com' \
  SHA256SUMS

# 3) Provenance attests this artifact came from the tagged release
slsa-verifier verify-artifact \
  --provenance-path satellite.intoto.jsonl \
  --source-uri      github.com/TinkerNorth/satellite \
  --source-tag      vX.Y.Z \
  SatelliteSetup-vX.Y.Z.exe
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
  `redist/SHA256SUMS`), then `iscc installer.iss`. `build-installer.bat`
  chains both. The installer accepts two unattended-install switches,
  documented in the README and in the comment blocks at the top of
  `installer.iss`:
    - `/VIGEM=auto|bundled|skip` (install path): override the bundled-driver auto-detect.
    - `/REMOVEVIGEM=auto|yes|no` (uninstall path): control whether the
      driver is removed alongside Satellite. Default `auto` prompts in
      attended uninstalls and leaves the driver alone in silent uninstalls.
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
OS and version, the relevant log excerpt (Windows: `%APPDATA%\satellite\`;
Linux: `journalctl --user -e`), and which Dish client is connecting.
