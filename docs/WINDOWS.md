# Satellite on Windows

What `SatelliteSetup.exe` puts on the machine, the switches for an
unattended install, what an upgrade changes, the controller audio settings,
and what uninstalling removes. The build of the installer itself is in
[CONTRIBUTING.md](../CONTRIBUTING.md) ("Cutting a release") and
[PACKAGING.md](PACKAGING.md) ("Windows: code signing and UIAccess").

## What the installer does

Download `SatelliteSetup.exe` from the [releases page](https://github.com/TinkerNorth/satellite/releases) and run it. The installer will:

- Install Satellite to `Program Files\Satellite`
- Create a Start Menu shortcut
- Optionally create a Desktop shortcut
- Optionally set Satellite to start with Windows
- Register in **Settings → Apps → Installed Apps** with an uninstaller
- Detect the ViGEmBus driver and, by default, install the bundled
  v1.22.0 if it's missing or older. That release lays down `ViGEmBus.sys`
  1.21.442.0, which is the file version the installer and the app's driver
  banner compare against; ViGEmBus 1.22.0 is the final upstream release, so
  newer installations are left untouched. The Components page shows the
  detected status before you continue.
- Deploy the HIDMaestro driver (optional-but-default component). It is a
  user-mode UMDF2 driver embedded in the bundled `satellite-hm-helper.exe`;
  deployment is idempotent and needs no reboot. It adds virtual DualSense,
  DualSense Edge and Switch Pro controller types, and covers DualShock 4 / Xbox 360 when
  ViGEmBus is absent. Setup installs no kernel driver for it; the one
  component that does is controller audio, and it installs on first use
  rather than at setup time (see [Controller audio](#controller-audio) below).

## Unattended installs

The installer accepts these switches alongside Inno Setup's standard
`/SILENT` and `/VERYSILENT` (`installer.iss` lists them in its header):

| Switch | Behavior |
|---|---|
| *(none)* / `/VIGEM=auto` | Default. Install the bundled ViGEmBus only if missing or older than the driver it carries, `ViGEmBus.sys` 1.21.442.0. |
| `/VIGEM=bundled` | Force-run the bundled installer regardless of what's already there. |
| `/VIGEM=skip` | Don't touch the driver. Use this on locked-down machines or when ViGEmBus is managed externally. |
| *(none)* / `/HIDMAESTRO=auto` | Default. Deploy/refresh the bundled HIDMaestro driver (idempotent, no reboot) and register the Satellite Controller Broker service. |
| `/HIDMAESTRO=bundled` | The same as `auto`, kept for symmetry with `/VIGEM=`. |
| `/HIDMAESTRO=skip` | Don't touch the HIDMaestro driver, and don't register the broker service (an existing one is removed). |
| `/UPDATECHECK=yes` or `no` | First install only: whether Satellite checks GitHub for new versions. Default `yes`; an attended install asks on its Privacy page. |
| `/CRASHREPORTS=yes` or `no` | First install only: whether Satellite sends crash reports. Default `yes`; an attended install asks on its Privacy page. |
| `/OTA` | Set by the in-app updater: Setup relaunches Satellite through Restart Manager, with no UI. Not for use by hand. |

A reboot is sometimes required on first ViGEmBus install or on an upgrade
over an older ViGEmBus (MSI exit code 3010). The Satellite installer surfaces
this as a "Restart now / later" prompt on the final wizard page. Until you
reboot, virtual-gamepad output may not work even though the driver is
installed. HIDMaestro never needs a reboot.

## Upgrading

Running a newer `SatelliteSetup.exe` over an existing install (or letting
the in-app updater do it) upgrades everything in place:

- The app and web UI are replaced; your config, pairings and the autostart /
  desktop-icon choices are kept.
- **ViGEmBus** is upgraded only if the installed `ViGEmBus.sys` is older than
  the 1.21.442.0 the bundled v1.22.0 carries (compared by file version; the
  installer and the driver are versioned separately upstream). Same or newer
  is left alone.
  An upgrade that replaces a loaded kernel driver can require a restart.
- **HIDMaestro** is re-deployed from the bundled helper every time the
  component is selected. The SDK compares the installed driver's manifest hash
  to the embedded one, so a same-version run is a ~50 ms no-op and a bumped
  pin is a real redeploy. No reboot.
- Component selection follows Inno Setup's memory of your previous install: a
  "Full" install picks up new components (HIDMaestro was added in 2.0.0); a
  "Custom" install keeps exactly the components you picked before, so the new
  driver is not added silently. The dashboard tells you either way.
- The in-app updater runs the installer `/VERYSILENT /NORESTART`, so it never
  reboots the PC by itself. Driver failures are written to the setup log under
  `%TEMP%` instead of raising a dialog nobody is there to dismiss.

The dashboard shows a **driver banner** on Windows: which of the two drivers is
installed, its version against the version this Satellite build bundles, and
whether ViGEmBus is waiting on a restart to finish an upgrade. It stays a small
green strip while everything is current and turns amber or red with the fix as
the action: install the pending Satellite update (which carries both drivers)
or re-run the installer for the running version.

Creating a HIDMaestro virtual device needs administrator rights, and
Satellite itself runs unelevated. Setup therefore registers the bundled
`satellite-hm-helper.exe` as the **Satellite Controller Broker** service
(`SatelliteHmBroker`, LocalSystem, demand-start). Satellite reaches the
broker over a named pipe as soon as it starts, and again after the dashboard
installs the driver, so the first HIDMaestro controller finds the connection
ready; the broker creates the device and hands the shared-memory handles
back, and no elevation prompt appears, whether or not anyone is at the PC.
The service starts on that first connection (Windows' named-pipe service
trigger, or Satellite starting it directly, which setup permits), stays up
while Satellite is connected, and exits after five idle minutes once Satellite
has gone. The broker admits only interactive logons and only the installed
`satellite.exe` beside it; Satellite in turn accepts the pipe only when its
server is the registered service's process. If the service is missing
(installed with `/HIDMAESTRO=skip`, or removed by hand), Satellite falls back
to spawning the helper elevated for the session when a HIDMaestro controller
connects, which is the one case that still shows a UAC prompt. ViGEm-only
sessions never show one. Signed releases
additionally carry Windows' UIAccess accessibility flag, so touchpad-as-mouse
input keeps reaching the foreground window after a game exits; this requires
the default `Program Files\Satellite` install.

## Controller audio

A real DualSense (or DualShock 4 v2) is not only a gamepad: it is also a USB
audio device, presenting Windows a speaker/headset output and a headset
microphone input. Satellite emulates those endpoints too, so a game's chat
audio reaches the player's headset on the client device and the client's
microphone is recorded on the PC from the pad's own mic endpoint. The
DualSense mute button and its mute lamp work end to end.

Windows names those endpoints after the persona's USB product string, so which
device to pick in the sound mixer depends on the pad Satellite created. The
DualSense composite reports "DualSense Wireless Controller" and appears as
`Speakers (DualSense Wireless Controller)` and `Headset Microphone (DualSense
Wireless Controller)`. The DualShock 4 v2 composite reports plain "Wireless
Controller"; its output terminal is a headset rather than a speaker, so the
leading noun may differ: look for the product string, not the prefix.

This is the one part of Satellite that is not user-mode. HIDMaestro serves an
audio-carrying ("composite") controller over a bundled, WHLK-certified
usbip-win2 kernel USB transport, which HIDMaestro installs the **first time**
such a controller is created, not at setup time, and never for an input-only
pad. Everything else, including every controller type without audio endpoints,
stays on the user-mode UMDF2 path described above.

Controller audio is five settings, all under **Settings → Controller audio** in
the dashboard, all persisted in `config.json`, and all on by default:

| Setting | What it gates | Applies |
|---|---|---|
| `controllerAudio` | Whether an audio-carrying persona is created at all, and so whether the kernel transport is ever installed | Next controller connected |
| `controllerAudioMic` | The client's microphone reaching the pad's mic endpoint | Immediately |
| `controllerAudioSpeaker` | The pad's speaker output reaching the client | Immediately |
| `controllerAudioHaptics` | The DualSense HD-haptics lanes reaching the client, as the waveform or reduced to rumble | Immediately |
| `controllerAudioKeepDefaultDevice` | Whether Satellite puts the previous default playback device back after the pad's endpoint appears | Next controller connected |

The master switch is the one with teeth: with `controllerAudio` off, Satellite
only ever creates input-only personas, so the kernel transport is never
installed. It applies to the next controller connected; a pad already carrying
audio keeps it until it is reconnected.

The three lane switches gate the wire, not the persona. HIDMaestro has no
mic-only audio function, every composite profile it ships declares both an
input and an output streaming interface, so "microphone without speaker"
cannot be expressed at the persona level at all. Both Windows endpoints
therefore keep existing either way and only the network traffic stops, which is
also why these reach a stream that is already playing: nothing has to be
replugged for them to take effect. Turning them all off is not the same as
`controllerAudio` off, which declines the persona and with it the kernel
transport.

`controllerAudioHaptics` covers channels 3/4 of the DualSense's output
stream, the two voice-coil actuators. A DualSense game authors its vibration
as audio on those lanes; titles built on Sony's own pad library use nothing
else, so a pad receiving only HID motor bytes never rumbles in them. A client
that can play the waveform into its own DualSense receives it as such; any
other client receives each 20 ms window reduced to motor strength on the
ordinary rumble path, mixed with the game's HID rumble. The switch stops both
at once. The DualShock 4 v2 audio function has no haptics lanes, so this only
ever applies to a DualSense.

`controllerAudioKeepDefaultDevice` is there because Windows promotes a newly
arrived endpoint to the default playback device: an endpoint with no persisted
`Level` value lands in the "newest device" bucket, where USB bus type and
Speakers form factor both rank at the top ([default audio endpoint
selection](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/default-audio-endpoint-selection)).
That, and not the feature itself, is what makes controller audio look like it
forwards everything: every sound on the desktop is being rendered into the
pad's endpoint. With this on, Satellite puts the previous default back
afterwards; turn it off to let the pad become the default the way Windows
intended.

A key missing from `config.json` reads as its default, so a config written
before the switch was split keeps behaving exactly as it did. The live state
is published two ways: `GET /api/server/capabilities` reports `audio` per
backend, and additively a top-level `controllerAudio` object whose `enabled` /
`mic` / `speaker` / `hapticAudio` say what will actually flow; `GET /api/status`
carries all five settings.

## Uninstalling

Use **Settings → Apps → Installed Apps → Satellite → Uninstall**, or run
the uninstaller from the Start Menu. The uninstaller will:

- Stop `satellite.exe` if it's running.
- Remove the Windows Firewall rules the installer added (UDP input 9876,
  discovery 9879, mDNS 5353, HTTPS client API 9443), plus the stale
  "Satellite HTTP" (TCP 9877; the admin server binds loopback, so no rule
  is added anymore) and "Satellite Pairing" (TCP 9878) rules if an older
  install left them behind.
- Delete the program files, Start Menu and Desktop shortcuts, and the
  HKCU "Run at login" registry entry.
- Ask whether to also uninstall the ViGEmBus driver. The default is
  No, because other apps (DS4Windows, BetterJoy, MoonDeck-Buddy, etc.)
  commonly share it and removing it would break them silently.
- Ask the same, separately, for the HIDMaestro driver (other HIDMaestro
  consumers such as PadForge may share it; default No).

User configuration (`%APPDATA%\satellite\config.json` and the pairing
keyfile) is left in place, so a reinstall preserves your paired
clients. Delete that folder by hand if you want a clean wipe.

The uninstaller accepts `/REMOVEVIGEM=` and `/REMOVEHIDMAESTRO=` switches
alongside Inno Setup's standard `/SILENT`:

| Switch | Behavior |
|---|---|
| *(none)* / `/REMOVEVIGEM=auto` | Default. Prompt the user; on `/SILENT` uninstall, do not touch the driver. |
| `/REMOVEVIGEM=yes` | Always uninstall the ViGEmBus driver as part of removing Satellite. |
| `/REMOVEVIGEM=no` | Never uninstall the ViGEmBus driver. Suppresses the prompt. |
| *(none)* / `/REMOVEHIDMAESTRO=auto` | Default. Prompt the user; on `/SILENT` uninstall, do not touch the driver. |
| `/REMOVEHIDMAESTRO=yes` | Always remove the HIDMaestro driver as part of removing Satellite. |
| `/REMOVEHIDMAESTRO=no` | Never remove the HIDMaestro driver. Suppresses the prompt. |
