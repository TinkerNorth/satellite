# Security policy: TinkerNorth

This file covers the six TinkerNorth repositories that ship the
wireless-gamepad product end-to-end (the website is
https://dish.tinkernorth.com):

- [`satellite`](https://github.com/TinkerNorth/satellite): Satellite, the server (Windows / Linux / macOS)
- [`dish-android`](https://github.com/TinkerNorth/dish-android): Dish for Android
- [`dish-ios`](https://github.com/TinkerNorth/dish-ios): Dish for iOS (SwiftUI)
- [`dish-mac`](https://github.com/TinkerNorth/dish-mac): Dish for macOS (SwiftUI)
- [`dish-windows`](https://github.com/TinkerNorth/dish-windows): Dish for Windows (Qt6 / SDL2)
- [`dish-linux`](https://github.com/TinkerNorth/dish-linux): Dish for Linux (Qt6 / SDL2)

Each repo has its own `CONTRIBUTING.md#security` section with
ecosystem-specific local commands; this file is the single source of
truth for **(1) reporting a vulnerability**, **(2) what we do with the
report**, and **(3) how a downstream consumer verifies a release
artifact**.

---

## Reporting a vulnerability

**Do not file a public issue for a suspected vulnerability.**

Use one of:

1. **GitHub private vulnerability reporting**: open the repo, click
   *Security → Report a vulnerability*. This is preferred because it
   creates a tracked advisory and a private discussion thread.
2. **Email**: `security@tinkernorth.com` (PGP key on request).
   Include the repo, version (commit SHA or release tag), reproduction
   steps, and impact.

Please do not test exploits against infrastructure you don't own.
The on-LAN threat model already covers an attacker with packet-injection
ability on the local network; that's the documented design boundary,
not a bug.

### Response SLA

| Severity | Triage acknowledgement | Initial assessment | Fix target |
|---|---|---|---|
| Critical (CVSS >= 9.0) | 1 business day | 3 business days | 14 days, coordinated disclosure |
| High (CVSS 7.0-8.9)    | 2 business days | 5 business days | 30 days |
| Medium / Low           | 5 business days | 10 business days | next minor release |

If we miss the SLA, you may publish 90 days after the original report
date regardless. We'd rather know than not know.

### Scope

In scope:

- All six repos above.
- Release artifacts attached to GitHub Releases for any of the six repos.
- The wire protocol (protocol 4; the single source of truth is
  [`docs/contract.md`](docs/contract.md)): the HTTPS `:9443` control plane
  (TOFU-pinned self-signed TLS, HMAC proof-of-key headers, declarative
  session PUT) and the UDP `:9876` data plane (ChaCha20-Poly1305-IETF under
  per-session HKDF keys, per-direction counters, replay guard).
- The pairing flow (PIN paths A/B on `:9443`) + the loopback-only admin
  HTTP/SSE web UI exposed by `satellite`.

- On Windows, the `SatelliteHmBroker` service boundary: the LocalSystem
  broker that creates HIDMaestro virtual devices for the unelevated
  `satellite.exe` over `\\.\pipe\satellite-hm-broker` (interactive-logon
  DACL, client image-path check; see `docs/ARCHITECTURE.md`, "Elevation
  story"). A way for a non-interactive or remote caller to reach it, or to
  obtain anything beyond virtual controller / audio device creation through
  it, is in scope.

Out of scope:

- Anything that requires the attacker to already have local privileges
  on the user's PC (root, Administrator, ability to drop binaries in
  `%APPDATA%`, etc.). A process already running as the signed-in user can
  reach the broker by design; that is the same trust it already holds over
  the user's own session.
- The vendored ViGEmBus driver itself; file with [nefarius/ViGEmBus](https://github.com/nefarius/ViGEmBus).
- The bundled HIDMaestro driver/SDK itself; file with [hifihedgehog/HIDMaestro](https://github.com/hifihedgehog/HIDMaestro).
- DoS via raw network flooding. UDP without rate-limit is a known
  trade-off for hot-path latency; mitigations belong in the network
  fabric, not the protocol.

---

## Supported versions

| Repo | Supported | Notes |
|---|---|---|
| `satellite` | latest minor on `main` (2.3.0 today); previous minor for 90 days | Windows is the canonical target; Linux is supported; macOS ships as a stub with no virtual gamepad (an unentitled build falls back to an inert backend) that runs only where Homebrew's libsodium, OpenSSL and opus are installed |
| `dish-android` | latest minor on `main` (2.7.0 today); previous minor for 90 days | minSdk 24 |
| `dish-ios` | latest minor on `main` (no release tagged yet); previous minor for 90 days | iOS 26.0+ |
| `dish-mac` | latest minor on `main` (1.4.0 today); previous minor for 90 days | macOS 13+ |
| `dish-windows` | latest minor on `main` (2.8.0 today); previous minor for 90 days | Windows 10 1809 (build 17763)+ |
| `dish-linux` | latest minor on `main` (2.8.1 today); previous minor for 90 days | tracks the oldest LTS the release CI builds against |

Patch releases (`X.Y.Z+1`) are issued on demand for the latest minor;
the previous minor only receives backports for high/critical fixes.

---

## Satellite local-surface hardening

The `satellite` server exposes two HTTP surfaces with deliberately
different trust models:

- **Admin API / web UI**: plain HTTP, bound to `127.0.0.1`. There is no
  authentication because loopback *is* the trust boundary. Two
  browser-borne attacks can still reach a loopback port, so an origin
  guard runs before every route:
  - **DNS rebinding**: a page on `evil.com` whose A record is flipped to
    `127.0.0.1` can script requests at us, but the browser still sends
    `Host: evil.com`. We reject any request whose `Host` is present and
    not loopback.
  - **CSRF**: a page can fire a no-cors cross-site `POST` straight at
    `http://127.0.0.1:<port>` (loopback `Host`, so the check above
    passes). The browser attaches `Origin: http://evil.com` to such a
    write, so we reject state-changing methods whose `Origin` is present
    and non-loopback. Same-origin dashboard requests carry a loopback (or
    absent) `Origin` and pass untouched.

- **Client API**: HTTPS (self-signed; clients TOFU-pin the certificate),
  bound to `0.0.0.0` and therefore LAN-reachable. Authenticated routes
  require a paired `deviceId` plus a per-request HMAC proof of the pairing
  key (`X-Device-Id` / `X-Hmac-Proof` headers); pairing itself is PIN-gated.

### PIN pairing

PINs gate the LAN-facing pairing flow and are hardened accordingly:

- PINs and identity tokens are drawn from libsodium's CSPRNG, never a
  deterministic PRNG such as `std::mt19937`.
- PIN comparison is constant-time (`sodium_memcmp`) so a wrong guess
  cannot leak, via timing, how many leading digits matched.
- PINs rotate every 5 minutes (the previous PIN stays valid for one
  extra period), are consumed on a successful pair, and both are burned
  after 5 failed guesses, so the 4-digit space is not online-brute-forceable.
- The rotating PINs are echoed only on the loopback-bound admin surface
  (dashboard SSE / status endpoint), never on the LAN-facing client API.

---

## How CI prevents vulnerable code from shipping

Each repo runs the same shape of gates:

**On every PR** (blocking):

- Action-pin lint (`scripts/check-action-pins.sh`): every `uses:` line
  names a 40-character commit SHA followed by its `# vX.Y.Z`.
- Allowlist (`scripts/security-allowlist.py`): `.security/allowlist.yaml`
  entries must be complete, unexpired and at most 90 days out; they are the
  only place a vulnerability is accepted, and OSV-Scanner reads them.
- Dependency review: GitHub advisory DB (PR-only), blocking on a high-severity
  finding.
- OSV-Scanner 2.7.1 (`scripts/osv-scan.sh`): the tree's manifests (for
  Satellite, the helper's NuGet test packages) and the vendored components
  by upstream commit; a scan that finds no package fails.
- Gitleaks 8.30.1: secret scanning over the full history.
- CodeQL: `cpp` for `satellite` (first-party code; the byte-identical vendored
  components are OSV-Scanner's and the freshness gate's) / `dish-linux`, `swift` for `dish-mac`,
  `java-kotlin` + `cpp` for `dish-android`.

**On every tagged release** (also blocking):

- The tag must name `/VERSION`, and `CHANGELOG.md` must have its heading
  (`scripts/check-version-consistency.sh --tag`).
- Re-run of every PR-time gate against the tagged commit.
- Required-secrets gate: reports which platform signing secrets are
  missing for a tag (Windows Authenticode, Apple Developer ID +
  notarization, Android keystore). It is **advisory by default**:
  `satellite` currently ships without those certificates, so releases
  degrade to `-unsigned` artifacts instead of failing, and authenticity
  rests on cosign + SLSA provenance rather than an OS-vendor
  certificate. Setting the repository variable
  `ENFORCE_RELEASE_SIGNING=true` restores the hard failure for stable
  tags once the certificates exist.
- Artifact-level vulnerability scan: Grype 0.122.1, fails on HIGH and
  CRITICAL.
- SBOM generation: Syft 1.57.2, both SPDX-JSON and CycloneDX-JSON.
- `SHA256SUMS` over every artifact and the SBOMs.
- Cosign 3.1.4 keyless signing: every artifact and `SHA256SUMS` get one
  Sigstore bundle (`<file>.sigstore.json`), anchored in the Sigstore
  transparency log. Every pull request runs the same job as a dry run over
  the packages and installer it built, signed with a throwaway key.
- SLSA L3 build provenance: `slsa-framework/slsa-github-generator`
  emits `<repo>.intoto.jsonl`.

The result: a known-vulnerable dep, a missing signature, or a tampered
binary all fail the release before any artifact lands on the GitHub
Release page.

---

## Verifying a release artifact (consumer recipe)

This recipe works the same way for every release, every repo, every
platform; only the artifact filenames change. Releases cut after the shared
release-hardening workflow landed carry one Sigstore bundle per file
(`<file>.sigstore.json`); earlier releases (Satellite up to 2.3.0) carry a
`.sig` and a `.crt` per file and are verified the second way below. The tag is
bare `X.Y.Z` in every command.

### 0) Pre-requisites

```bash
# cosign 3.x: brew install cosign, or a release binary from
# github.com/sigstore/cosign/releases (check it against cosign_checksums.txt)
```

### 1) Download every file from the Release

```bash
TAG=X.Y.Z
REPO=<repo>     # satellite, dish-android, dish-ios, dish-mac, dish-windows, dish-linux
gh release download "$TAG" -R "TinkerNorth/$REPO" -D "$REPO-$TAG"
cd "$REPO-$TAG"
```

You should see the platform artifacts, `SHA256SUMS` with its signature,
`<product>.sbom.spdx.json` and `<product>.sbom.cdx.json`, and
`<product>.intoto.jsonl` (the SLSA L3 provenance).

### 2) Releases after the shared workflow: one bundle per file

```bash
# SHA256SUMS was signed by this repository's release workflow, on this tag.
cosign verify-blob \
  --bundle SHA256SUMS.sigstore.json \
  --certificate-identity "https://github.com/TinkerNorth/$REPO/.github/workflows/_release_harden.yml@refs/tags/$TAG" \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com \
  SHA256SUMS

# Every file you downloaded matches it (macOS: shasum -a 256 -c).
sha256sum --ignore-missing -c SHA256SUMS
```

The identity is exact, not a regular expression, and names
`_release_harden.yml` because the certificate's identity is the workflow that
ran the signing step. To verify one file on its own, run the same
`cosign verify-blob` with `--bundle <file>.sigstore.json` and `<file>`.

### 3) Releases up to Satellite 2.3.0: `.sig` and `.crt`

Satellite signed these in the reusable `_release_harden.yml` too, so the
identity names that file. cosign 3 still verifies them with the deprecated
flags (it prints two deprecation lines):

```bash
cosign verify-blob \
  --certificate SHA256SUMS.crt --signature SHA256SUMS.sig \
  --certificate-identity "https://github.com/TinkerNorth/$REPO/.github/workflows/_release_harden.yml@refs/tags/$TAG" \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com \
  SHA256SUMS
shasum -a 256 --ignore-missing -c SHA256SUMS
```

(Run against Satellite 2.3.0 with cosign 3.1.4: `Verified OK` for
`SHA256SUMS` and for `satellite_2.3.0_amd64.deb`'s own `.sig` and `.crt`,
and the `.deb` `OK` against `SHA256SUMS`. Other repositories' earlier
releases name `release.yml` or `_release_harden.yml` in the identity; read
the workflow out of the certificate.)

### 4) The SLSA L3 provenance

`<product>.intoto.jsonl` is a Sigstore bundle whose statement names every
released file by SHA-256. It is signed by the SLSA generator at the exact
commit the workflow pins, which `slsa-verifier` refuses ("invalid ref") because
that is not a release tag, so verify it with cosign, naming the generator's
commit from the provenance's `builder.id`:

```bash
cosign verify-blob-attestation \
  --bundle "satellite.intoto.jsonl" --new-bundle-format --type slsaprovenance \
  --certificate-identity "https://github.com/slsa-framework/slsa-github-generator/.github/workflows/generator_generic_slsa3.yml@f7dd8c54c2067bafc12ca7a55595d5ee9b75204a" \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com \
  satellite_2.3.0_amd64.deb
```

(`Verified OK` against Satellite 2.3.0's `.deb`. The commit is the generator
v2.1.0 the workflow pins; use the one in the provenance you are checking.)

### 5) (Optional) Inspect the SBOM

```bash
jq -S . satellite.sbom.spdx.json | less
```

---

## Known gaps

- **slsa-verifier refuses the provenance.** The generator is called by
  commit SHA, which slsa-verifier rejects as "not a release tag"; cosign
  verifies the same provenance (step 4 above).
- **Branch protection on `main`.** Changes land through pull requests.
  On `satellite`, `main` requires the CI, security and CodeQL checks to pass
  and the branch to be up to date before a merge; the other repos carry
  their own rules, which this file does not restate.
- **Vendored-header scanners.** `satellite/lib/` and
  `satellite/vigem/include/` are not understood by ecosystem scanners.
  `scripts/vendored-osv-lockfile.sh` hands OSV-Scanner the upstream
  commit each [`lib/VENDORED.md`](lib/VENDORED.md) pin names, so an OSV
  advisory whose git range covers a pinned commit fails CI. OSV cannot
  place every advisory on a release commit (libsodium tags its releases
  on a branch its fixes do not descend from), so each refresh also
  reviews the upstream's advisories by hand, and the
  `vendored-freshness` CI job fails if any `Last-vendored:` date is
  more than 90 days old. This is best-effort, not exhaustive; file an
  advisory if you spot a vendored component that's missing from
  `VENDORED.md`.
- **Controller audio loads a kernel USB transport, and its personas are
  deliberately indistinguishable from real hardware.** An emulated
  DualSense or DualShock 4 v2 can present the pad's own microphone and
  speaker to Windows. Those "composite" personas are served over
  HIDMaestro's bundled WHLK-certified usbip-win2 kernel USB transport,
  which HIDMaestro installs the first time such a controller is created.
  Every other Satellite path on Windows is user-mode; this one is not.
  The `controllerAudio` setting (Settings > Controller audio, on by
  default, `GET /api/server/capabilities` reports it per backend as
  `audio` and host-wide as `controllerAudio.enabled`) turns it off, and
  with it off the transport is never installed. Only that master switch
  does: the `controllerAudioMic` and `controllerAudioSpeaker` settings
  beside it gate the wire rather than the persona, so turning both off
  stops the network traffic while the composite pad, and therefore the
  kernel transport, is still created. Two consequences worth stating
  plainly rather than discovering:
  - A composite persona is **not identifiable as virtual** from the HID
    node. It enumerates under `ROOT\HIDMAESTRO_UDE`, four parents above
    the HID device, so software that checks a pad's own device path,
    VID/PID or container id sees a real DualSense. That is the point (a
    pad that announces itself as emulated is a pad games refuse), but it
    means Satellite cannot be used to satisfy an anti-cheat or
    attestation requirement, and we will not treat "software could not
    tell it was virtual" as a vulnerability.
  - Microphone audio from a client crosses the encrypted session and is
    written to the emulated pad's mic endpoint, where any application on
    the host can record it exactly as it could from a physically plugged
    controller. Satellite never stores it and never sends it anywhere but
    that endpoint. The client owns muting: a muted client sends no audio
    at all rather than sending silence. `controllerAudioMic` off makes the
    host drop those frames before decode, but that is a host-side routing
    choice, not a privacy guarantee a client can rely on; the endpoint
    still exists either way.
- **macOS virtual pads are entitlement-gated.** The macOS backend
  synthesizes virtual DualShock 4 controllers via `IOHIDUserDevice`,
  which requires the `com.apple.developer.hid.virtual.device`
  entitlement. No entitled build has shipped: the released
  `satellite-macos-stub-*` artifacts, CI artifacts and local builds detect the missing entitlement at runtime
  and fall back to the historical inert backend: the protocol stack
  runs, but every controller descriptor applies as
  `backendUnavailable`. The CI artifact name
  (`satellite-macos-stub-...`) reflects that fallback. Don't open a
  vulnerability report for the absence of virtual-gamepad creation in
  an unentitled build; it's the documented, probe-enforced fallback.
  The stub links Homebrew's libsodium, OpenSSL and opus from
  `/opt/homebrew` rather than bundling them, so their security updates
  come from `brew upgrade`, not from a Satellite release.
