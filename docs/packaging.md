# Packaging and distribution channels

How Satellite reaches Linux users, which channels are viable, and — for the
ones that are not — why, so the question does not get re-litigated later.

Everything here turns on one constraint.

## The constraint: /dev/uinput is a host-level privilege

Satellite's core function is creating a virtual Xbox 360 pad through
`/dev/uinput`. On a stock system that node is `root:root 0600`: systemd ships
no rule for it, and `50-udev-default.rules` grants `GROUP="input"` only to
`SUBSYSTEM=="input"` — while uinput is `SUBSYSTEM=="misc"`. Being in the
`input` group therefore does nothing on its own.

Access must be granted by a udev rule installed on the host. That single fact
partitions the channels:

| | Can install a udev rule? | Consequence |
|---|---|---|
| `.deb`, `.rpm`, AUR | **Yes** — privileged install scriptlets | Works out of the box |
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
| **APT/DNF repo** (gh-pages) | Wired, needs activation | Publisher is written; needs `GPG_PRIVATE_KEY`, `GPG_KEY_ID`, and Pages enabled |
| **`.deb` / `.rpm`** | Shipping | CPack, built in release CI |
| **AppImage** | Shipping | Built in release CI |
| **AUR** (`satellite-bin`) | Ready, not submitted | Needs an AUR account + SSH key |
| **Flatpak** | Manifest written, **unbuilt** | Self-hosted/sideload only. Not Flathub — see below |
| **Snap** | Manifest written, **unbuilt** | Sideload only. Not the Snap Store — see below |
| **Launchpad PPA** | Not started | Would need a full hand-written `debian/` tree |
| **Fedora COPR** | Not started | Would need a hand-written `.spec` |

Neither sandbox manifest has been built or run. No `flatpak-builder` or
`snapcraft` was available on the machine they were authored on, so treat both
as reviewed-but-unverified until someone builds them.

## Why not Flathub

Flathub's *impermissible submissions* list rules out, verbatim: tray-only
applications, system utilities "generally used on host", applications relying
on "complicated post installation setups for core functionality", and projects
with "insufficient development history". Satellite matches four of four.

The two closest precedents were both closed unmerged — `turbo-clicker` (a
Flathub admin answered the uinput-permissions question with "no this is not
possible") and `Plover`. Apps that *do* ship with uinput on Flathub — Steam,
Solaar, AntiMicroX, Sunshine — are all mature upstream projects, and every one
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

Of every snap published in the store, **three** plug `uinput` — two of them
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
payload read-only — misdetecting either as `Portable` would tell the user to
replace bytes that physically cannot be replaced.

| Type | Detected by | Update method |
|---|---|---|
| Snap | `$SNAP` | `sudo snap refresh satellite` |
| Flatpak | `/.flatpak-info` exists | `flatpak update io.github.tinkernorth.satellite` |
| AppImage | `$APPIMAGE` | Self-install (swap and re-exec) |
| Deb / Rpm | exe path + package DB | `apt` / `dnf` command |
| AUR | `/opt/satellite/satellite.AppImage` | `yay -Syu satellite-bin` |

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
  a mismatch — only `validate-tree` against a staged install does.

**Screenshots are deliberately absent.** Screenshot URLs must resolve over
HTTPS or networked validation fails, and none are published yet. Any Flathub
submission would require at least one. To add them: publish PNGs, reference
them at a tag (not a branch), and re-run validation without `--no-net`.
