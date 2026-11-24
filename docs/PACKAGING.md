# Packaging and distribution channels

How Satellite reaches Linux users, which channels are viable, and, for the
ones that are not, why, so the question does not get re-litigated later.

Everything here turns on one constraint.

## The constraint: /dev/uinput is a host-level privilege

Satellite's core function is creating a virtual Xbox 360 pad through
`/dev/uinput`. On a stock system that node is `root:root 0600`: systemd ships
no rule for it, and `50-udev-default.rules` grants `GROUP="input"` only to
`SUBSYSTEM=="input"`, while uinput is `SUBSYSTEM=="misc"`. Being in the
`input` group therefore does nothing on its own.

Access must be granted by a udev rule installed on the host. That single fact
partitions the channels:

| | Can install a udev rule? | Consequence |
|---|---|---|
| `.deb`, `.rpm`, pacman (`satellite-bin`) | **Yes**: privileged install scriptlets | Works out of the box |
| Flatpak, Snap | **No** | Works only where the host is already configured |

A sandboxed package cannot fix this and should not pretend to. `--device=all`
(Flatpak) and the `uinput` interface (Snap) both grant *visibility* and lift
*sandbox* restrictions; neither changes file permissions.

### The udev rule

[`packaging/debian/70-satellite-uinput.rules`](../packaging/debian/70-satellite-uinput.rules)
uses two mechanisms deliberately:

```
KERNEL=="uinput", SUBSYSTEM=="misc", TAG+="uaccess", OPTIONS+="static_node=uinput", GROUP="input", MODE="0660"
```

- `TAG+="uaccess"` makes systemd-logind attach a POSIX ACL (`user:<uid>:rw-`)
  for the **active seat** user. No group membership, no logout/login cycle.
  Applied by systemd's `73-seat-late.rules`, so this file's `70-` prefix must
  keep sorting before it.
- `GROUP`/`MODE` covers what `uaccess` does not: autostart and background
  sessions that are not the active seat, SSH, and seat-less systems. Satellite
  is an autostart tray app, so this path is load-bearing, not a fallback.
- `static_node=uinput` applies permissions to the statically-created node on
  kernels where uinput is a module. Fedora, Arch and SteamOS build it `=m`;
  Ubuntu builds it `=y`. Without this, the first `open()` races module autoload.

This is the same rule Valve ships in `steam-devices`, plus the group fallback.

## Channel status

| Channel | Status | Notes |
|---|---|---|
| **`.deb` / `.rpm`** | Shipping | CPack through the `package` preset, built by release.yml in a debian:trixie and a fedora:44 container and attached to every release |
| **AppImage** | Shipping | Built by release.yml on ubuntu-22.04 (the glibc 2.35 floor), with zsync update metadata |
| **APT repository** (gh-pages) | Shipping | `https://tinkernorth.github.io/satellite/debian`, regenerated and signed by `publish-repos.yml`, which release.yml calls after every stable tag publishes |
| **DNF repository** (gh-pages) | Shipping | `https://tinkernorth.github.io/satellite/rpm`, same workflow |
| **pacman repository** (gh-pages) | Shipping | `https://tinkernorth.github.io/satellite/arch/$arch`, same workflow: it builds `satellite-bin` from [`packaging/aur/PKGBUILD`](../packaging/aur/PKGBUILD) in an Arch container and signs the package and the database |
| **AUR** (`satellite-bin`) | Not published | AUR account registration is closed. `packaging/aur/` stays AUR-ready; the pacman repository above is the Arch channel |
| **Flatpak** | Unpublished template | [`packaging/flatpak/`](../packaging/flatpak/) sits at placeholder version 1.0.0. No workflow builds, tests or publishes it. Not Flathub, see below |
| **Snap** | Unpublished template | [`packaging/snap/snapcraft.yaml`](../packaging/snap/snapcraft.yaml) sits at placeholder version 1.0.0. No workflow builds it. Not the Snap Store, see below |
| **winget** | Unpublished template | `scripts/winget/*.yaml` sit at placeholder version 1.0.0 with an all-zero installer hash, and point at an installer name (`SatelliteSetup-1.0.0.exe` under the version tag) that predates the current asset names. No workflow submits them |
| **Launchpad PPA** | Not started | Would need a full hand-written `debian/` tree |
| **Fedora COPR** | Not started | Would need a hand-written `.spec` |

The three unpublished templates are kept on purpose while the maintainer
decides whether to finish or remove them. Neither sandbox manifest has been
built or run: no `flatpak-builder` or `snapcraft` was available on the machine
they were authored on, so treat both as reviewed but unverified until someone
builds them. Each carries an "Unpublished template" comment on its first line.

The repositories are signed with the key in
[`packaging/repo/gpg.key`](../packaging/repo/gpg.key) (secrets `GPG_PRIVATE_KEY`
and `GPG_KEY_ID`); [`packaging/repo/README.md`](../packaging/repo/README.md)
covers the layout and a manual re-run, and
[`packaging/repo/index.html`](../packaging/repo/index.html), the page at
`https://tinkernorth.github.io/satellite/`, carries the install steps for each
distribution. Prerelease tags never reach the repositories.

## Building the packages

Every Linux artifact is built through one script per artifact, and the same
script runs on a pull request and on a tag:

| Artifact | Script | Pull request (linux-ci.yml) | Tag (release.yml) |
|---|---|---|---|
| `.deb` | `scripts/build-deb.sh`, then `scripts/check-package.sh deb` | job `deb`, debian:trixie | job `deb`, debian:trixie |
| `.rpm` | `scripts/build-rpm.sh`, then `scripts/check-package.sh rpm` | job `rpm`, fedora:44 | job `rpm`, fedora:44 |
| AppImage | `scripts/build-appimage.sh`, then `scripts/smoke-alive.sh` | job `appimage`, ubuntu-22.04 | job `appimage`, ubuntu-22.04 |

`scripts/install-package-deps.sh deb|rpm|appimage` installs each lane's
packages. `scripts/check-package.sh` holds a package to its contents and
scriptlets (the udev reload and `modprobe uinput`), checks the dashboard and
the bundled controller database it ships with `scripts/check-web-payload.sh`,
installs it, launches the installed `/usr/bin/satellite` headless and requires
it to stay up, to log `Controller database N (bundled), M records`, and to exit
0 on SIGTERM. `build-appimage.sh` runs the same payload check on the
AppImage's own content. The tag adds only the release version, the Sentry DSN
and the debug-symbol upload.

`scripts/package-in-container.sh deb|rpm|appimage` runs a lane locally in the
same image (docker), and `scripts/ci-local.sh --with-packages` and
`--with-appimage` call it.

## AppImage and source builds

An AppImage, or a build run from the source tree, cannot install the udev
rule a package installs, so `/dev/uinput` access is granted once by hand. The
dashboard shows these commands when it finds the device missing or not
writable (`web/dashboard.js`):

```bash
sudo modprobe uinput                                      # load the module now
echo uinput | sudo tee /etc/modules-load.d/satellite.conf # and at every boot
echo 'KERNEL=="uinput", GROUP="input", MODE="0660"' \
  | sudo tee /etc/udev/rules.d/70-satellite-uinput.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
sudo usermod -aG input "$USER"                            # then log out and back in
```

The packaged rule (`packaging/debian/70-satellite-uinput.rules`) also tags the
node `uaccess`, which grants the active seat user access without the group;
the group covers autostart and remote sessions. A source build is
`scripts/build.sh` (preset `release`), which leaves `./satellite` at the
repository root; `scripts/build-appimage.sh` builds an AppImage like the
released one, best on the oldest glibc you intend to support.

## Why not Flathub

Flathub's *impermissible submissions* list rules out, verbatim: tray-only
applications, system utilities "generally used on host", applications relying
on "complicated post installation setups for core functionality", and projects
with "insufficient development history". Satellite matches four of four.

The two closest precedents were both closed unmerged: `turbo-clicker` (a
Flathub admin answered the uinput-permissions question with "no this is not
possible") and `Plover`. Apps that *do* ship with uinput on Flathub (Steam,
Solaar, AntiMicroX, Sunshine) are all mature upstream projects, and every one
of them documents a manual host udev step anyway.

Flathub also has a **generative-AI policy**: submission PRs must not be
generated or automated with AI tools, and reviewers do ask. Relevant to how any
future submission is prepared.

The Flatpak format still has a real audience: on SteamOS, Bazzite and Nobara
the host already carries Valve's `uaccess` rule, so a sideloaded bundle works
with no setup. That is what
[`packaging/flatpak/`](../packaging/flatpak/) targets.

## Why not the Snap Store

`uinput` is a *super-privileged* interface: snapd's base declaration sets
`allow-installation: false` on the plug, so a store-published snap declaring it
cannot be installed until Canonical hand-signs a declaration.

Of every snap published in the store, **three** plug `uinput`, two of them
Canonical's own (`steam`, `bluez`). Third-party auto-connection has been
refused every time it has been requested since 2020; the one recent approval
(`aiche-desktop`) took ~99 days, covered manual connection only, and required
Verified Publisher vetting.

Sideloading has none of these problems: the install-time policy check only
iterates slots, so `snap install --dangerous` plus `snap connect satellite:uinput`
works today. That is the scope of
[`packaging/snap/snapcraft.yaml`](../packaging/snap/snapcraft.yaml).

A `daemon:` app would run as root and sidestep the DAC problem entirely. It is
deliberately not defined, because `src/net/webserver.cpp` binds the admin API
to loopback **without authentication** on the assumption it holds one user's
privileges. As root it would expose pairing, config and device creation to
every local user. Authenticate the admin API before considering that shape.

## Build flags

`-DSATELLITE_SANDBOXED=ON` skips the udev rule and `modules-load.d` install.
Both are inert inside a sandbox prefix, where neither udev nor systemd reads.
Set it for Snap and Flatpak builds; leave it off everywhere else.

## Install-channel detection

`LinuxUpdaterAdapter::detectInstallType()` decides how the in-app updater
behaves. Snap and Flatpak are checked **first**, because both mount their
payload read-only, and misdetecting either as `Portable` would tell the user to
replace bytes that physically cannot be replaced.

| Type | Detected by | Update method |
|---|---|---|
| Snap | `$SNAP` | `sudo snap refresh satellite` |
| Flatpak | `/.flatpak-info` exists | `flatpak update io.github.tinkernorth.satellite` |
| AppImage | `$APPIMAGE` | Self-install (swap and re-exec) |
| Deb / Rpm | exe path + package DB | `apt` / `dnf` command |
| pacman (`satellite-bin`) | `/opt/satellite/satellite.AppImage` | `sudo pacman -Syu satellite-bin` |

The `satellite-bin` package from the pacman repository installs the same
`/opt/satellite/` layout the AUR package would, and the updater shows the
pacman command above
(`src/core/linux_upgrade_commands.h`, held to this table by
`test_linux_upgrade_commands`). The package carries the
release's AppImage unstripped (`options=('!strip' '!debug')` in the
PKGBUILD): makepkg's strip step cut the AppImage's payload off in 2.3.0, and
linux-ci.yml's arch job now installs the package and compares the installed
AppImage with the packaged one on every pull request.

`/.flatpak-info` is preferred over `$FLATPAK_ID` because a child process
inherits the environment variable and can outlive the sandbox it describes.

## AppStream metainfo

[`packaging/io.github.tinkernorth.satellite.metainfo.xml`](../packaging/io.github.tinkernorth.satellite.metainfo.xml)
drives GNOME Software, KDE Discover and App Center listings. Validate with:

```bash
appstreamcli validate --no-net --pedantic --strict \
  packaging/io.github.tinkernorth.satellite.metainfo.xml
```

Two traps worth knowing:

- `metadata_license` describes **the metadata file**, not the program, and must
  come from AppStream's permissive allowlist. `LGPL-3.0-or-later` there is a
  hard validation error; the file uses `FSFAP`, with the real license in
  `project_license`.
- The filename must equal the component `<id>`. Plain `validate` does not catch
  a mismatch; only `validate-tree` against a staged install does.

**Screenshots are deliberately absent.** Screenshot URLs must resolve over
HTTPS or networked validation fails, and none are published yet. Any Flathub
submission would require at least one. To add them: publish PNGs, reference
them at a tag (not a branch), and re-run validation without `--no-net`.

## Windows: code signing and UIAccess

What the installer puts on a machine, its unattended switches, upgrades and
uninstalling are in [WINDOWS.md](WINDOWS.md).

Separate from the Linux constraint above, the Windows build has one of its
own. Satellite forwards the phone's touchpad as mouse input through
`SendInput`, and User Interface Privilege Isolation (UIPI) discards synthesized
input aimed at a window of higher integrity, which is where focus can land
after a game exits. Microsoft's documented way past UIPI is the manifest
attribute `uiAccess="true"`, kept beside `level="asInvoker"`. Windows honours
it only when all three of these hold:

1. The executable's manifest requests `uiAccess="true"`.
2. The executable carries an Authenticode signature that chains to a root in
   the machine's Trusted Root Certification Authorities store.
3. The executable is installed under Program Files (or Program Files (x86)).
   The policy "User Account Control: Only elevate UIAccess applications that
   are installed in secure locations" enforces this and is enabled by default.

An executable that requests UIAccess but fails 2 or 3 is refused at process
creation ("A referral was returned from the server"), so the attribute must
never ship in a build that cannot meet them. When all three hold, the process
gets the user's own non-elevated token with the UIAccess flag and no prompt:
medium integrity raised by 0x10 for a standard user, high integrity without
administrator rights for an administrator.

The CMake option `SATELLITE_UIACCESS` (default `OFF`) decides whether the
manifest generated from `satellite.manifest.in` carries the attribute, and
`release.yml` keys the whole path on the `WIN_SIGN_CERT_BASE64` secret:

- **Unsigned release** (no certificate, the current state): the
  `asInvoker`-only build and installer shipped today, with artifacts carrying
  the `-unsigned` suffix.
- **Signed release** (`WIN_SIGN_CERT_BASE64` + `WIN_SIGN_PASSWORD`): the build
  is configured with `-DSATELLITE_UIACCESS=ON`. `satellite.exe`,
  `crashpad_handler.exe`, `crashpad_wer.dll` and `satellite-hm-helper.exe` are
  signed with `scripts/sign.ps1` before packaging (it fails if a signature does
  not validate), and the job fails unless `satellite.exe` contains
  `uiAccess="true"`. Inno Setup compiles with `/DUiAccess=1`, so the installer
  accepts only a destination under Program Files. `SatelliteSetup.exe` is then
  signed, and the install/launch round-trip runs from Program Files instead of
  `RUNNER_TEMP`.

A `/DUiAccess=1` installer refuses other folders on the Select Destination
Location page and again just before installing, which also catches a silent
`/DIR=` outside Program Files. An existing install outside Program Files is
relocated instead of refused: the Select Destination Location page appears
even though this is an upgrade, proposing `C:\Program Files\Satellite`, and
silent runs (the in-app updater included) take that folder. Setup closes a
copy still running from the old folder, installs the new one, then deletes
only the old copy's own files (`satellite.exe`, `crashpad_handler.exe`,
`crashpad_wer.dll`, `satellite-hm-helper.exe`, `web\`, `LICENSE`, `README.md`
and the old `unins000.*` uninstaller) and removes the old folder if that leaves
it empty. Settings, logs and dumps under the user profile stay where they are;
the app repoints its autostart entry itself once the old target is gone, and
Setup re-adds the firewall rules and points the broker service at the new
path.

Both halves can be reproduced locally. `scripts\build-installer.ps1 -UiAccess`
requires signing credentials (`SATELLITE_SIGN_THUMBPRINT`, `SATELLITE_SIGN_PFX`
or `CLOUD_SIGN_TOOL`), checks that `satellite.exe` was built with
`-DSATELLITE_UIACCESS=ON`, and passes `/DUiAccess=1` to `iscc`.
`scripts\test-installer-roundtrip.ps1 -UiAccess` puts its sandbox under
`%ProgramFiles%` instead of the temp directory. A self-signed certificate meets
requirement 2 only on a machine whose Trusted Root store holds it.

## Code signing policy

Free code signing on Windows provided by [SignPath.io](https://signpath.io),
certificate by [SignPath Foundation](https://signpath.org). Signed releases
start once SignPath Foundation approves the project; until then the installer
is unsigned and SmartScreen warns on first run.

Committers, reviewers and approvers:
[@emir-hasanbegovic](https://github.com/emir-hasanbegovic), owner of the
[TinkerNorth](https://github.com/orgs/TinkerNorth/people?query=role%3Aowner)
organisation. Only the release workflow signs, from a tagged commit; no local
build is ever signed. Signed artifacts: `SatelliteSetup.exe`, `satellite.exe`,
`crashpad_handler.exe`, `crashpad_wer.dll` and `satellite-hm-helper.exe`.

Privacy: three things can leave your PC, and each can be turned off in
Settings: the update check against GitHub Releases, the check for a newer
controller database at dish.tinkernorth.com, and, with *Share crash reports*
on, a crash report to Sentry. The installer asks about the update check and
crash reports. [`PRIVACY.md`](../PRIVACY.md) lists what is stored and sent.
Third-party components in the signed package: sentry-native and Crashpad, the
crash reporter described in `PRIVACY.md` section 6, and the bundled ViGEmBus
installer and HIDMaestro driver and helper, under the licences listed in
[`THIRD_PARTY.md`](../THIRD_PARTY.md).
