# Satellite: Privacy Policy

**Effective date:** 2026-11-22.
**Canonical copy:** this file, at
[`https://github.com/TinkerNorth/satellite/blob/main/PRIVACY.md`](https://github.com/TinkerNorth/satellite/blob/main/PRIVACY.md).
The privacy index on [dish.tinkernorth.com](https://dish.tinkernorth.com/privacy/)
points here, so the code and the policy ship together.

This document describes what data Satellite stores, what it sends, to whom,
and the choices you have over it. Satellite is the receiver half of Dish: it
runs on the gaming PC and turns controller input from your Dish clients into
a virtual gamepad. The Dish clients (Dish for Android, Dish for iOS, Dish for macOS, Dish for
Windows and Dish for Linux) each have their own policy; do not read one as describing another.

---

## 1. Short version

- Satellite runs on your own PC and receives controller input from your own
  Dish clients over your own network. The only TinkerNorth server it contacts
  is the static site that publishes the controller database (section 5), and
  only to ask whether a newer copy exists.
- **Three things can leave your PC**, and you can turn each off in Settings:
  an update check against GitHub Releases (section 4), a check for a newer
  controller database at dish.tinkernorth.com (section 5) and, when Satellite
  crashes, one crash report to Sentry (section 6).
- **A diagnostics file** for support is something you save and email
  yourself (section 7). Satellite never sends it.
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
treat it as sensitive before attaching it to a public issue. A few kinds of
crash (a stack-protection or control-flow check firing, or an `abort()`)
never reach Satellite's own handler; for those, Windows Error Reporting
records an *Application Error* event in Event Viewer and writes its own
minidump into the same `dumps\` folder, under the same cap of ten files.
With crash reporting on, Windows hands such a crash to the Sentry SDK's
handler instead (section 6) and writes no minidump of its own.
Windows may also send Microsoft its own report of such a crash, exactly as it
does for any other program on the PC; that is governed by the Windows
diagnostics settings, not by Satellite's switch. With crash reporting on,
the Sentry SDK keeps its run state and any report it has not yet delivered
under `%LOCALAPPDATA%\TinkerNorth\Satellite\sentry\` on Windows and
`~/.config/satellite/sentry/` on Linux; on Windows the handler's minidumps
stay there after delivery too. Delete either folder at any time; Satellite
recreates it only when it needs it again.

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
Settings, Updates, and nothing update-related leaves your PC. On Windows the
installer asks about the check on first install; a silent install can pass
`/UPDATECHECK=no`.

---

## 5. Controller database updates

Satellite ships a list of the controllers the Dish apps know (their USB and
Bluetooth ids, which protocol each speaks, how well each is verified) and uses
it to tell Dish apps which physical pads each virtual controller stands for.
A newer copy of that list is published at `https://dish.tinkernorth.com/controllers/v1/`,
a static site in an Amazon S3 bucket behind Amazon CloudFront, operated for
TinkerNorth by Amazon Web Services.

**What is fetched, and how often.** Ten seconds after Satellite starts, and
then on the update check's schedule (Settings, Updates; once a day by
default), Satellite asks for the list's manifest, a small file naming the
published version. Only when it names a newer version does Satellite download
the controller list and its strings in each language, check each file against
the manifest's SHA-256, and keep them beside its configuration
(`controllerdb-*.json`). The request carries no query and no identifier, and
on Windows and Linux no user-agent at all (the macOS build, a stub released
without virtual gamepads, sends the system's default one, which names the app and its
version); Satellite sends nothing about you,
your PC or your controllers.

**What the host sees.** Like any web server, CloudFront's access logs see your
public IP address and the time of the request. TinkerNorth does not combine
them with anything.

**Your choice.** Settings, Updates, *Check for controller updates
automatically*, on by default. Off means no request for the list leaves your
PC, and *Check now* is off with it. Satellite keeps using the copy it has.

**The mirror.** Satellite also serves the copy it uses to Dish apps on your
own network at `/api/controllers/v1/` on its client port, so a Dish app on a
network without internet access can read it. That traffic stays on your
network.

---

## 6. Crash reports (Sentry)

**What is sent, and when.** Settings, Diagnostics, has a *Share crash reports*
switch, on by default; on Windows the installer asks about it on first install
(`/CRASHREPORTS=no` for a silent install). With it on, an official release
build that crashes
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
  name and version, the CPU architecture, and a random event id;
- when the crash went through `abort()` or an uncaught exception, one line
  naming the thread and, where the runtime can tell, the exception type and
  message.

On Windows the SDK's handler is a separate program, `crashpad_handler.exe`,
and its Windows Error Reporting module, `crashpad_wer.dll`, is registered
with Windows (a value under the current user's registry hive) so that a
crash Windows catches first is still handed to the same handler and reported
the same way, with the switch honoured.

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

## 7. Diagnostics by email

The Debug page has *Report a problem*. You pick what it is about, write what
happened, and press *Save the file and write the email*. Satellite builds one
file, `dish-diagnostics-satellite-<date>-<time>.json`, with your category and
your words in it, and your browser saves it. Beside the button, *Email
support@tinkernorth.com* opens your own mail app with a subject, your words
and a short summary filled in. Nothing is sent unless you attach the file
yourself and press Send.

**What the file holds:** your category and your words, the Satellite version
and build, the operating system and CPU architecture, the controller
database's versions and last check, the virtual controllers (their type, the
backend that runs them, which features the Dish app said its controller has,
how many input frames arrived and at what rate, how many times a game set
each feedback lane and the last value it set, and the kind and time of the
last output a game sent), the connected Dish apps as "Host 1", "Host 2" with
their protocol version and link state, the counters the Debug page shows, the
backends' status, your settings, and the last 500 log lines.

**What it never holds:** pairing keys, pairing secrets or PINs (a pairing key
in a log line is replaced by "[redacted]"), IP addresses (log lines have IPv4
and IPv6 addresses masked), the network interface you chose, the names or ids
of your Dish apps and devices (log lines have them replaced by "Host 1",
"Host 2": every device connected or paired, and every one that asked to pair
or opened a session since Satellite started, paired now or not), this PC's
name (log lines have it replaced by "this PC"), and nothing from the
microphone or the speaker. Read the file before you send it;
it is plain JSON.

- **A Dish app may ask Satellite about its own connection.** When you report
  a problem from a paired Dish app, the app may ask Satellite, over the
  paired, encrypted connection and with the same pairing proof every session
  request carries, for a document about that app's own connection, and put
  it in the file the app saves for you. Satellite answers only the app that
  owns the connection, never a stranger and never another paired app. The
  document holds the Satellite version, the platform and how long Satellite
  has run, the backends' status, that connection's virtual controllers (their
  type, backend, features, input counts and the feedback a game set), the
  audio switches this PC grants, the counters the Debug page shows, the
  controller database's versions, and up to 200 log lines about that
  connection: the lines Satellite wrote naming that connection or the app's
  own device id, never a line picked because the name the app chose appears
  in it. It never holds another app's name, id or address (every app that
  asked to pair or opened a session since Satellite started included), this
  PC's name, an IP address, a pairing key, a PIN or any other secret: the
  asking app is called "this connection", every other app "another client"
  and the PC "this PC". Satellite sends it to nobody else; the app, like Satellite,
  sends nothing until you press Send.

---

## 8. Your choices

- **Stop crash reports being sent.** Settings, Diagnostics, *Share crash
  reports*, off; or untick it on the Windows installer's Privacy page.
- **Stop the update check.** Settings, Updates; or untick it on the Windows
  installer's Privacy page.
- **Stop the controller database check.** Settings, Updates, *Check for
  controller updates automatically*, off.
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

## 9. Children's privacy

Satellite is suitable for general audiences. It collects nothing from anyone,
of any age, beyond the crash report described above, so there is no
children's data for us to hold. If you believe that is wrong in some way we
have not anticipated, contact `privacy@tinkernorth.com`.

---

## 10. International transfers

Crash reports, when enabled, are stored by Sentry in the United States
(section 6). The update check goes to GitHub, also in the United States. The
controller database check goes to dish.tinkernorth.com, served by Amazon
CloudFront from the edge location nearest you, from a bucket in the United
States. Nothing else leaves your machine to us.

---

## 11. Changes to this policy

We will update the *Effective date* at the top whenever this policy changes.
Material changes, such as any new analytics backend, will be made here before
the code ships and called out in that release's notes in
[`CHANGELOG.md`](CHANGELOG.md). Previous versions remain in the git history
of this file.

---

## 12. Contact

- Privacy questions: `privacy@tinkernorth.com`
- Security disclosures: see [`SECURITY.md`](SECURITY.md)
- General contact and bug reports: open an issue in this repository
