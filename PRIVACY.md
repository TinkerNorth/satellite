# Satellite: Privacy Policy

**Effective date:** 2026-09-06.
**Canonical copy:** this file, at
[`https://github.com/TinkerNorth/satellite/blob/main/PRIVACY.md`](https://github.com/TinkerNorth/satellite/blob/main/PRIVACY.md).
The privacy index on [dish.tinkernorth.com](https://dish.tinkernorth.com/privacy/)
points here, so the code and the policy ship together.

This document describes what data Satellite stores, what it sends, to whom,
and the choices you have over it. Satellite is the receiver half of Dish: it
runs on the gaming PC and turns controller input from your Dish clients into
a virtual gamepad. The Dish clients (Android, Windows, Linux) each have their
own policy; do not read one as describing another.

---

## 1. Short version

- Satellite runs on your own PC and receives controller input from your own
  Dish clients over your own network. TinkerNorth does not operate a server
  for it, and it never contacts one during normal use.
- **Two things can leave your PC**, and you can turn both off in Settings: an
  update check against GitHub Releases (section 4) and, when Satellite
  crashes, one crash report to Sentry (section 5).
- Crash reports are on by default, the same opt-out switch every Dish client
  has. Turning it off stops the very next report. A report carries a stack
  trace and the Satellite and operating-system versions. It never carries
  controller input, audio, your paired devices or their keys.
- There is no analytics SDK, no telemetry, no advertising identifier, no
  usage reporting and no account.
- We do not sell, share or rent your data. We do not show ads. We do not
  profile you.

---

## 2. What stays on your PC

**Configuration.** Satellite keeps one JSON file: `%APPDATA%\satellite\config.json`
on Windows, `$XDG_CONFIG_HOME/satellite/config.json` (usually
`~/.config/satellite/config.json`) on Linux. It holds your settings (the UDP
port, autostart, discovery broadcast, the network interface, the controller
audio switches, the *Share crash reports* switch, update preferences) and the
list of paired Dish clients: each one's name, its public key and the pairing
secret Satellite derived with it. Nothing in it is uploaded anywhere.

**Logs.** A rolling log of what Satellite is doing (pairings, sessions,
errors) at `%LOCALAPPDATA%\TinkerNorth\Satellite\logs\` on Windows. It stays
on disk and is overwritten as it rolls. Linux writes the same messages to
standard error and, under systemd, to the journal.

**Crash artefacts.** On Windows, a crash writes a minidump to
`%LOCALAPPDATA%\TinkerNorth\Satellite\dumps\` whether or not crash reporting
is on. A minidump is a snapshot of the crashed process and can contain
whatever it held in memory, including the address of a connected Dish client;
treat it as sensitive before attaching it to a public issue. With crash
reporting on, the Sentry SDK keeps its run state and any report it has not
yet delivered under `%LOCALAPPDATA%\TinkerNorth\Satellite\sentry\` on
Windows and `~/.config/satellite/sentry/` on Linux. Delete either folder at
any time; Satellite recreates it only when it needs it again.

**The web UI.** Settings, pairing and diagnostics live in a small web page
Satellite serves to your own browser on `http://localhost:9877`. It answers
local requests only and stores nothing beyond the configuration above.

---

## 3. Sent to your own network, not to TinkerNorth

Satellite announces itself on your local network so Dish clients can find it
(mDNS and a discovery beacon), accepts pairing from clients you approve, and
then receives their encrypted controller input, and controller audio when you
turn that on, over UDP. That traffic goes between your devices on your own
network. None of it reaches TinkerNorth or any third party, and Satellite
stores none of it: input is applied to the virtual gamepad and discarded,
audio is played or captured and discarded.

---

## 4. Update check (GitHub)

Once a day (configurable under Settings, Updates), Satellite asks the GitHub
Releases API whether a newer version exists. GitHub sees your public IP
address, as it does for any request, and a standard user-agent string. No
identifier is sent. On Windows and with the Linux AppImage, Satellite can
also download and install the update after verifying its checksum; package
manager installs only show you the command. Turn the check off under
Settings, Updates, and nothing update-related leaves your PC.

---

## 5. Crash reports (Sentry)

**What is sent, and when.** Settings, Diagnostics, has a *Share crash reports*
switch, on by default. With it on, an official release build that crashes
sends one report to [Sentry](https://sentry.io), a crash-reporting service
operated by Functional Software, Inc. (San Francisco, USA), which processes
it on our behalf. The report is produced by the Sentry native SDK and
contains:

- the stack of the crashing thread and the list of loaded modules with their
  versions, so we can see where it failed;
- on Windows, a minidump of the crashed process: register state and the
  stack memory of its threads, not a full memory image. Stack memory can
  contain fragments of whatever Satellite held at that moment, which in
  principle includes the address of a connected Dish client;
- the Satellite version and a build environment label, the operating system
  name and version, the CPU architecture, and a random event id.

It does **not** contain controller input, controller audio, your paired
devices or their keys, your configuration, your user name, or a device
identifier, and Satellite never reports starts, stops, sessions or usage:
the crash is the only event. Sentry sees the public IP address of your PC
when it receives the report, as any web service does; Satellite does not
attach it and we do not use it. Sentry retains crash data for 90 days, then
deletes it. We use Sentry's US region, so the data is stored in the United
States. See Sentry's [privacy policy](https://sentry.io/privacy/) for its
role as a processor.

**Opting out.** Turn *Share crash reports* off. The switch disarms the SDK
immediately, so it stops the very next report, not just later ones, and the
choice persists across restarts and upgrades. Opting out never costs you the
local crash artefacts in section 2. The status line under the switch says
whether this build can send at all.

**Builds that cannot send.** Only official releases from this repository
carry the Sentry address (DSN) that the SDK needs. A source build, a
pull-request build or a fork carries none and cannot upload, whatever the
switch says; the status line says so.

**Symbols.** To make reports readable we upload the debug symbols of official
builds to Sentry at release time. They describe the program, not you.

---

## 6. Your choices

- **Stop crash reports being sent.** Settings, Diagnostics, *Share crash
  reports*, off.
- **Stop the update check.** Settings, Updates.
- **Unpair a device.** Removing a Dish client from the paired list deletes
  its key from the configuration.
- **Uninstall.** The Windows uninstaller removes the program files. The
  configuration in `%APPDATA%\satellite\` and the state under
  `%LOCALAPPDATA%\TinkerNorth\Satellite\` are left behind so that
  reinstalling restores your pairings; delete both folders by hand to remove
  everything. On Linux, removing the package leaves `~/.config/satellite/`
  for the same reason.
- **Verify any of this.** Satellite is free software under
  [LGPL-3.0-or-later](LICENSE). Every claim above is checkable in this
  repository, and you can build the binary yourself.

---

## 7. Children's privacy

Satellite is suitable for general audiences. It collects nothing from anyone,
of any age, beyond the crash report described above, so there is no
children's data for us to hold. If you believe that is wrong in some way we
have not anticipated, contact `privacy@tinkernorth.com`.

---

## 8. International transfers

Crash reports, when enabled, are stored by Sentry in the United States
(section 5). The update check goes to GitHub, also in the United States.
Nothing else leaves your machine to us.

---

## 9. Changes to this policy

We will update the *Effective date* at the top whenever this policy changes.
Material changes, such as any new analytics backend, will be made here before
the code ships and called out in that release's notes in
[`CHANGELOG.md`](CHANGELOG.md). Previous versions remain in the git history
of this file.

---

## 10. Contact

- Privacy questions: `privacy@tinkernorth.com`
- Security disclosures: see [`SECURITY.md`](SECURITY.md)
- General contact and bug reports: open an issue in this repository
