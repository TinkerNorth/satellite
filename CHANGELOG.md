# Changelog

All notable connection-model and protocol changes are recorded here.
The protocol itself is specified in [`docs/contract.md`](docs/contract.md).

## Unreleased

No protocol changes. Feedback on a ViGEm pad, a relaunched client's pad
state, a crash a paired client could cause, the vendored components, the
HIDMaestro SDK a locally built installer ships, and the dashboard's dish
glyph.

The dashboard's dish glyphs aimed up and to the left while their signal
arcs sat at the upper right. The reflector now faces the signal in every
state, and the Dish icon the dashboard shows for a paired phone follows.

Two of the three ViGEm personas never carried a game's feedback right. The
DS4 persona asked the driver for its notifications with a read-write request
code where the driver dispatches a write-only one, so the very first wait
failed and the worker returned: no rumble and no light bar ever came back
from a ViGEm DS4 pad. The Xbox 360 persona read the driver's notification
with the player-LED slot first, so a game's small motor arrived as the strong
one and the LED slot as the weak one, and the DS4 layout had its two motors
the other way round as well. The codes and layouts now match the driver's own
source, and the tests drive each notification through the real adapter as
the driver's bytes.

A session's first authenticated datagram now brings each pad's light bar,
trigger effects and player LEDs again. The satellite coalesces those states
and re-sent one only when the game changed it, so a client that relaunched
(Android, once the system had killed the app in the background) or a state a
game set before the client's first datagram was routable stayed dark until
the game moved. The mic lamp is deliberately left out: on the desktop clients
it also mutes the pad's microphone amplifier, and a replay would undo the
user's own unmute. RUMBLE is a command with its own refresh and audio is a
stream, so neither is replayed. `docs/contract.md` says which states come
again and why.

A paired client could crash the satellite with a deeply nested JSON body:
nlohmann/json copies, compares and dumps recursively, and a 30 KB body nested
about 5000 deep overflowed the stack once `PUT /api/connections` copied its
`hostFeatures` out. Bodies nested past 32 levels are now refused before
anything copies them, by a linear pre-pass that runs before the document
exists; the deepest document the satellite reads is 5 levels (GitHub's
release list) and no client body is deeper than 4. A garbled or non-object
body on that route is also refused with 400, as the contract says, where it
used to read as the empty desired set: 200, and every pad unplugged.

Vendored components: cpp-httplib 0.56.0 to 0.58.0, which carries 0.57's
fixes for chunk-line request smuggling and unbounded trailers. The OSV gate
had scanned nothing since it was written (OSV-Scanner finds no package source
in vendored C++); it now scans each component by the upstream commit its pin
names, fails on a pin upstream does not have or a lockfile that yields no
packages, and CONTRIBUTING and SECURITY.md describe the gate that actually
runs, including its one blind spot (libsodium tags releases on a branch its
fixes do not descend from, so the hand review stays).

The bundled HIDMaestro SDK moves from 1.9.0 to 1.9.2 (driver INF 1.8.1.2248,
so a machine on the 1.9.0 driver sees the outdated banner until the
installer's helper reinstalls once). Its shared-memory contract and the four
profiles the satellite plugs are byte for byte what 1.9.0 shipped; what
moved is the USB transport under the composite personas: usbip-win2 0.9.8.1
is bundled, a machine HIDMaestro put on 0.9.7.5 or 0.9.7.7 moves to it by
itself in a Windows session where the host controller has not carried a
device yet, and the SDK now speaks the 0.9.7.x, 0.9.8.0 and 0.9.8.1 request
formats, so a usbip-win2 0.9.8.0 that another program installed (Handheld
Companion's, for one) no longer refuses the DualSense and DualShock 4
composite personas.

A locally built installer could ship an older HIDMaestro SDK than its pins
named. `scripts/fetch-redist.ps1` unpacked the SDK into `redist/hidmaestro/`
only while that directory was empty, so a checkout that staged 1.7.0 in
August kept it through the 1.9.0 and 1.9.2 bumps: the helper deployed driver
1.4.7.12 while satellite.exe, pinned to 1.8.1.2248, reported `update
available, this Satellite ships 1.8.1.2248` on the machine the installer had
just set up, and the banner's install button, which runs that same helper,
could not change it. The staging is now stamped with the zip it came from and
re-staged when the pin moves; after staging, the script reads the
`hidmaestro.inf` `DriverVer` out of the SDK assembly and fails when
`driver_pins.h` disagrees; `satellite-hm-helper.exe sdk-version` prints the
SDK and driver a built helper carries, and `scripts/verify-helper-sdk.ps1`
holds the published helper to the same pins after every publish, locally and
in CI. That last gate caught a second way to ship the old SDK: an incremental
`dotnet publish` over a re-staged SDK copies the new assembly beside the
helper but keeps the previous compile and single-file bundle, because the
staged file's own timestamp is older than them; the local scripts now publish
from a clean `bin/` and `obj/`. The setup log records each driver's detected
version, the decision taken and the exit codes, which the log of the install
that surfaced this did not. Releases built in CI were not affected: a fresh
runner stages from the pinned zip and publishes from nothing.

Smaller: one hex encoder in core spells every key, salt, digest, id and MAC
address the satellite writes as text (six hand-rolled copies retired, session
tokens and connection ids included, byte for byte what they wrote before);
the speaker stream's Opus mode and in-band FEC are pinned by tests (it
encodes as Hybrid because in-band FEC and the loss hint are set together, and
turning either off would hand it to CELT and delete that FEC); and the
comments that described those two things wrongly are corrected.

## 2.1.1

No protocol changes. Windows crash visibility, and an updater fix.

Four times in one evening the app died with a pad plugged in and nothing
recorded it: no minidump, no Crashpad report, no Sentry event, no Event
Viewer entry. The process switched Windows Error Reporting off for itself
at startup, and the crashes a hardened build produces (a stack-protection
or control-flow check firing, or an `abort()`) never reach the handler that
writes the local dump and hands it to Crashpad. Windows Error Reporting now
stays on with only its dialog suppressed, so every crash leaves an
*Application Error* event; Crashpad's Windows Error Reporting module,
`crashpad_wer.dll`, is built, shipped and registered, so those crashes reach
Sentry under the same *Share crash reports* switch; the installer adds a
per-app dump policy so they also leave a minidump in the usual `dumps\`
folder; `abort()` is hooked and chained into Crashpad with a minidump of its
own; the local minidump is written off the crashing thread, so a stack
overflow still gets one; and the log keeps its last lines through a crash
and says why the process is leaving on a normal exit, at logoff, or when the
installer closes it. `satellite.exe /crash-test`, `/crash-test=abort` and
`/crash-test=fastfail` prove each path on any machine.

The updater could end the process on its own. Queueing a download and then
skipping that version in the moment before the worker picked it up made the
worker lock a mutex it already held, which the Windows runtime turns into an
exception nothing catches. It now broadcasts after releasing the lock.

## 2.1.0

Protocol 3, additive: the accepted range is now [1, 3], so every shipped Dish
keeps working at its own version and a Dish that speaks 3 gains the DualSense
HD-haptics lanes. Also the Windows driver SDK bump and vendored-library
updates below.

The bundled HIDMaestro SDK moves from 1.7.0 to 1.9.0 (driver INF 1.4.7.12 to
1.8.1.958; the installer's helper reinstalls it, no reboot). Three things
change for a user. A virtual Xbox 360 pad no longer tells XInput it runs on an
empty battery: 1.7.0's reply to `IOCTL_XUSB_GET_BATTERY_INFO` put WIRED/FULL
one word early, so every game that surfaces battery state warned the moment
the pad appeared (SDL read it as 10 %). A virtual controller now keeps the
same device paths, container id and USB serial across recreation, restarts,
reboots and driver upgrades, so a program that keys a binding on the device
path keeps it; the paths change once, on the first run after this upgrade.
And the composite personas' USB transport is usbip-win2 0.9.7.5 (0.9.7.7
before): a machine an earlier Satellite put on 0.9.7.7 is moved in place the
first time the transport is idle, with nothing to do. The shared-memory
contract Satellite reads is byte-for-byte the same, and Satellite still passes
no identity key, so the 1.8.x boot-loop bug (an `index:N` key handed back to
the SDK) never applied here.

Protocol 3. A DualSense game that vibrates through Sony's own pad library
authors the effect as audio on the pad's HD-haptics lanes and never writes a
motor byte, so a streamed pad stayed still in it while a local one shook.
The helper now hands those lanes (channels 3/4 of the composite's output
stream) to Satellite on a ring of their own, and Satellite sends them down as
HAPTIC_AUDIO 0x0015 to a client that advertises the new `hapticAudio` cap, or
reduces each 20 ms window to motor strength on the existing RUMBLE path for a
client that only advertises `rumble`, mixed with the game's HID rumble by MAX.
A `controllerAudioHaptics` switch beside the mic and speaker ones stops both
renderings at once; `/api/server/capabilities` gains
`controllerAudio.hapticAudio`, the catalog gains the `hapticAudio` slug on the
DualSense type, and `/api/debug` gains the lane's counters. No frame shape
changed and the accepted range is still [1, 3], so every shipped Dish keeps
working at its own version and gains the rumble reduction without an update.

The satellite now owns a rumble's lifetime. A RUMBLE packet runs a client's
motors for its duration and then stops, but the satellite forwarded a game's
level once and coalesced every identical repeat, so a level a game held for
longer than half a second died on any client whose actuator runs per command
(SDL pads, the phone vibrator) while a Direct-claimed pad, whose raw path
ignores durations, ran on. A maintenance tick re-sends a held level every
200 ms, sends the stop itself when a haptic reduction's stream ends without a
silent window, and the coalesce cache is no longer trusted past the client's
duration. Clients need no change: honouring the duration was always enough.

An idle Sony pad on the HIDMaestro backend keeps reporting. Every Dish client
sends INPUT on change only, while a real DualSense reports every 4 ms whether
or not anything moved, and Sony's own pad library depends on that: it opens a
pad by waiting for its next input report, so a game built on it (007 First
Light among them) sat at controller setup until the player touched the pad,
and could read a pad whose reports stopped as gone. The same maintenance tick
now re-publishes an idle DualSense or DualShock 4 persona's last frame every
100 ms with its free-running clocks advanced, exactly as the pad itself
would. Clients need no change and must not send keepalive input.

## 2.0.4

No protocol changes. Windows driver and installer fixes.

The app window is now a hidden top-level window instead of a message-only
one. Message-only windows are excluded from broadcast messages, so
WM_QUERYENDSESSION never arrived: Windows logoff and shutdown never reached
the app, and the installer's Restart Manager could not close it, leaving
"Setup was unable to automatically close all applications" and a file it could
not replace. The installer also force-closes an app that never answers, and
the toast Accept/Reject buttons now reach the running instance.

An in-app update brings Satellite back up afterwards. Nothing did: the app
closes itself, so Restart Manager has nothing to restart, and the finish-page
launch entry is skipped by a silent install.

Drivers can be fixed without hunting down an installer. The HIDMaestro driver
installs straight from the dashboard using the helper Satellite already
ships, and the driver banner now downloads and runs the verified installer
itself instead of opening a release page in whichever browser is viewing the
dashboard. The ViGEmBus guidance points at the installer that carries it
rather than at upstream, and an unresponsive bus asks for the restart that
actually clears it.

## 2.0.3

No protocol changes and no changes to the shipped application. The release
workflow asserted the .rpm contents through a pipe into `grep -q`, which under
`pipefail` reports 141 when grep exits on an early match before rpm finishes
writing; 2.0.2 failed that way on a package that was correct. The assertions
now match against captured output.

## 2.0.2

No protocol changes. Fix release for the Windows driver banner, which lied
twice on a fresh install: its "Get installer" button linked
`releases/tag/v<version>` although tags have been bare since 1.0.0, and
ViGEmBus reported an update as available because the pin it compared against
was the setup bundle's version (1.22.0) rather than the driver that bundle
installs (`ViGEmBus.sys` 1.21.442.0). The installer made the same comparison
and so re-ran the bundled ViGEmBus setup on every install; that run always
exited "same-or-newer already installed", so nothing was ever downgraded.

## 2.0.1

No protocol changes. Fix release: the AppImage self-update no longer replaces
the binary underneath a running satellite. The helper script waited 30 seconds
for the old process to exit and then swapped whether or not it had, leaving a
live process to page-fault on an AppImage that had moved out from under it. It
now aborts and leaves the install untouched; the next update check offers the
release again.

## 2.0.0

Everything below ships as 2.0.0. The version number is aligned across the
Dish and Satellite family for this release: protocol 2 spans all of it
(controller feedback, controller audio, negotiated versioning), and a mixed
fleet is exactly what the negotiation in `docs/contract.md` exists to keep
working. The HIDMaestro backend, the composite audio personas and the
unified build story (one script contract shared by CI and local builds)
land here too.

The diagnostics page tells both halves of the story. `GET /api/debug` was
eight counters about inbound UDP plus one backend name, and the page built on
it showed one direction, named only the preferred backend (so a Windows box
with both drivers only ever said "ViGEm"), and painted an idle-but-healthy host
red because it read `backendAvailable` -- which means "the bus is open right
now", not "the driver works". The payload gains three blocks. `rx` counts
accepted inbound messages by type (input, heartbeat, motion, battery, pointer,
mic audio) and the four ways a datagram is refused before any decoder sees it
(`malformed` for a known opcode that failed its length guard, `unknownType`,
`runt`, `unknownToken`). `tx` is the direction that had no telemetry at all:
datagrams and bytes sent, split across heartbeat acks, rumble, lightbar,
trigger effects, player LEDs, speaker audio, the mic lamp and session closes,
plus `unroutable` / `encryptFailed` / `oversize` / `sendFailed` -- the last of
which required checking `sendto`'s return value, which the client adapter had
been discarding. `audio` reports the health of the streams this release turns
on: mic frames accepted, arrived-too-late and dropped (a partition of every
inbound frame), how many reached the pad by decode, by Opus in-band FEC and by
concealment, and on the way out how many speaker frames were sent, suppressed
as digital silence, failed to encode, or lost to lock contention. Alongside
them: client-API 401s split `notPaired` / `badProof`, reaped sessions, and the
host gauges the page previously had to infer (`webPort` -- which the old page
read but the server never sent -- `mdnsResponderActive`, `clientApiListening`,
live connection and controller counts).

None of it touches the gamepad hot path, and the split is deliberate rather
than incidental: the inbound counters live only in the receiver's rejection
branches and its cold non-gamepad dispatch branch (`DispatchResult` grew a
`handled` flag so the receiver can tell a malformed frame from an unrecognised
one without re-parsing, keeping `inner_dispatch.cpp` free of globals as its
portable test build requires), the outbound ones sit in
`ClientAdapter::sendEncryptedPacket`, which no gamepad packet reaches, and the
audio ones in `SessionService` under locks those paths already hold. Inbound
byte counting is deliberately absent: it would cost an atomic add per packet on
the accepted path. `maxLoopUs` keeps its read-and-zero window semantics for
benchmark tooling, and a new `peakLoopUs` reports the peak that field could not:
the receiver's thread-local high-water mark never resets, so a zeroed
`maxLoopUs` climbs again only on a new all-time record, which is why the page's
"peak" read 0 nearly always. The page itself is rebuilt around a bidirectional
flow diagram, six grouped sections, a mirrored in/out traffic chart and a list
of every backend the host reports with its vendor, mode, audio capability,
lifecycle and installed-versus-bundled driver version; it degrades to em dashes
against an older satellite rather than rendering `undefined`, and it stops
polling when you navigate away instead of fetching `/api/debug` twice a second
for the life of the tab.

No elevation prompt when a controller connects. Creating a HIDMaestro
virtual device needs an administrator token, and until now Satellite got
one by spawning `satellite-hm-helper.exe` with `runas` on the first
HIDMaestro plug of each session: one UAC prompt, at the exact moment
nobody is at the PC, because the pad is on the phone across the room. Setup
now registers the same helper as the LocalSystem service `SatelliteHmBroker`
("Satellite Controller Broker"), demand-start, and Satellite talks to it over
the well-known pipe `\\.\pipe\satellite-hm-broker` instead. Nothing about
the hot path changes; the broker only does what the spawned helper did
(create the SwDevice, duplicate the section and event handles into the
satellite process, tear down on disconnect). What changes is who holds the
token: the pipe's DACL admits interactive logons only, the broker admits a
connection only when the client is the installed `satellite.exe` beside it,
and Satellite accepts the pipe only when its server PID is the registered
service's PID, so a squatted pipe name is refused rather than trusted.
Setup grants interactive users SERVICE_START and installs a named-pipe
service trigger, so the service is started by whoever connects first and
exits after five idle minutes; a PC with no controller plugged runs no
broker. The `runas` path stays as the fallback when the service is absent
(`/HIDMAESTRO=skip` installs, hand-removed service, a second concurrent
session refused as `busy`), so the one remaining UAC prompt is the one an
administrator opted into. The helper gains `service` (SCM-hosted) and
`broker` (console, for debugging) modes; `hello` answers `"broker":true`
over the service so the log says which path a session took.

Driver status on the Windows dashboard, and an upgrade path that never reboots
behind your back. `GET /api/backend/status` and `GET /api/server/capabilities`
now fill each backend's `driverVersion` on Windows (the ViGEmBus.sys file
version; the HIDMaestro driver-store INF `DriverVer`) and gain three additive
fields beside it: `bundledVersion` (what this Satellite build's installer
ships for that driver, null where it ships nothing), `versionState`
(`current` / `outdated` / `newer` / `unknown`, derived server-side so no client
has to compare version strings) and `restartPending` (ViGEmBus only: the bus
device node reports `DN_NEED_RESTART`, which is what a driver upgrade that
returned 3010 leaves behind until Windows restarts). The dashboard renders a
driver banner off those fields on Windows hosts: a quiet green strip when both
drivers are installed and current, and an amber or red banner naming what is
missing, outdated, unresponsive or waiting on a restart, with the fix as the
action. If a Satellite update is already available or downloaded, the banner
routes to it, since the installer carries both drivers; otherwise it links the
installer for the running version. The upgrade path itself had one real hole:
the in-app updater ran the installer `/VERYSILENT` without `/NORESTART`, and a
bundled ViGEmBus upgrade over an older driver (1.21.x is common in the field)
can return 3010, which a very-silent Inno run answers by rebooting the PC
without asking. The updater now passes `/NORESTART`, the installer logs driver
failures instead of raising a modal when it runs under `/OTA` (there is nobody
at the keyboard to dismiss one), and the banner's restart-pending row is how
the user learns the reboot is owed. The pinned versions live in one header
(`src/platform/windows/driver_pins.h`) and `version-consistency.yml` fails the
build if `installer.iss` drifts from it; bumping the HIDMaestro SDK now also
means re-reading the INF version its `HIDMaestro.Core.dll` embeds (the recipe
is in `redist/README.md`).

Controller audio, split per direction and no longer paying for silence: one
`controllerAudio` switch turned both directions on together, so a host that
wanted the pad's microphone had to accept its speaker too, and the speaker
carries whatever Windows renders into the endpoint. `controllerAudioMic` and
`controllerAudioSpeaker` now join it and gate the WIRE rather than the persona:
HIDMaestro has no mic-only USB Audio function, so both Windows endpoints exist
either way and only the network traffic stops. That also lets those two reach
a stream already playing, which the master switch cannot: it decides whether a
kernel transport is installed at all, so it lands at the next plug.
`controllerAudioKeepDefaultDevice` is a fourth setting: Windows promotes a
newly arrived endpoint to the default playback device, which is the real reason
controller audio looked like it forwarded everything, and with it on Satellite
puts the previous default back. Absent keys read as on, so an upgraded config
keeps the behaviour its owner chose. All four ride `GET /api/status`, and
`GET /api/server/capabilities` gains a top-level `controllerAudio` block
(`enabled` / `mic` / `speaker`, each ANDed down to what will actually flow)
rather than `/api/catalog`, whose ETag is server version plus locale and must
stay static identity. The speaker path also stops sending digital silence: an
all-zero 20 ms window is neither encoded nor sent, and deliberately does not
advance `seq`, because a suppressed window is not a hole and concealing one
would have Opus invent noise where the game wrote none (~28 kbps of Opus,
~52 kbps on the wire once framing is counted). DTX goes on the mic encoder
only; the speaker declines it, since that gate cuts anything ~26-30 dB below
the recent peak and turns a reverb tail into comfort noise. Correcting the
record while here: the loss hint, not the application, picks the mode, so both
streams are Hybrid fullband and both really do carry in-band FEC (8.4 dB
recovery against -1.3 dB for blind PLC on the speaker stream); dropping the
hint to reach CELT would silently delete it.

Controller audio, the pad end: an emulated DualSense or DualShock 4 v2 now
presents Windows the microphone and speaker the physical pad does. HIDMaestro
materializes the identity's composite persona (`dualsense-composite`,
`dualshock-4-v2-composite`) instead of the plain one, and two new
shared-memory rings alongside the driver's input/output pair carry PCM between
the elevated helper and satellite: speaker up from the helper, microphone down
to it, each a 32-slot seqlock ring with a doorbell, the same shape the driver's
output ring already uses. The rings speak the wire's channel layout at the
persona's own sample rate, which splits the work where it can be tested: the
helper picks channels (dropping the DualSense's HD-haptics lanes 3/4, which
never cross the wire), and satellite rate-converts, because the DualShock 4 v2
persona runs 32 kHz out and 16 kHz in exactly like the hardware it impersonates
and a decimation without a lowpass would fold everything above 8 kHz into the
voice band. The DualSense mute button rides `wButtons` 0x0800 into input byte 9
bit 0x04, and the game's mute-lamp writes come back through the existing output
ring. The catalog's ds4 `emulates.usb` hint now lists both hardware revisions
(054c:05c4 and 054c:09cc): ViGEm materializes the v1 identity, HIDMaestro the
v2, and only the v2 carries the USB audio function the new feature slugs
describe.

Honesty, not polish: an audio-carrying persona is served over HIDMaestro's
bundled WHLK-certified usbip-win2 kernel USB transport, which installs the
first time such a controller is created. Satellite's Windows path was described
as user-mode throughout; that was true of input and is now stated as being true
of input only, in README.md, installer.iss (component description and the
components-page prompt), docs/architecture.md and SECURITY.md. The new
`controllerAudio` setting (on by default, Settings > Controller audio in the
dashboard, persisted in config.json) is the off switch, and with it off the
transport is never installed. `GET /api/server/capabilities` reports it per
backend as `audio`, next to `kernelMode`, which keeps describing the input
submit path and stays false. A refused composite falls back to the plain
persona rather than failing the plug: a pad without audio beats no pad.

Controller audio, codec and jitter window: the two audio paths the wire
contract described now carry samples. Inbound MIC_AUDIO frames go through a
2-frame reorder window keyed on the wrapping `seq` and into an Opus decoder,
which recovers a single lost packet from the in-band FEC copy the next packet
carries and conceals the rest; outbound speaker PCM is re-framed to exact 20 ms
windows (a backend hands over batch boundaries, not codec boundaries), encoded,
and sent with a per-controller wrapping sequence. Both codecs are created on a
controller's first audio frame and released with the pad, so a slot that never
carries audio never pays for one. libopus joins libsodium and OpenSSL as a
required dependency on every platform; the core stays free of it behind an
injected factory, the same arrangement that keeps HKDF out of the core.

Controller audio, wire contract and server plumbing (protocol 2, extended in
place before 2.0.0): the emulated pad's OWN audio endpoints now have a protocol.
New UDP messages MIC_AUDIO 0x0012 (client to server, mono 48 kHz, one 20 ms Opus
packet per frame), SPEAKER_AUDIO 0x0013 (server to client, stereo) and MIC_LED
0x0014 (mute-lamp state, coalesced like LIGHTBAR), each `ctrlIdx(1) + seq(u16 BE)`
framed and gated on new descriptor caps `mic` / `speaker`. The streams are lossy by
design: no acks, no retransmits, Opus in-band FEC plus PLC conceal loss, and `seq`
only marks gaps and late frames inside a 2-frame reorder window. Mute is the
client's to enforce, so muted means zero mic packets on the wire; `wButtons` bit
0x0800 (the one free bit in the XINPUT-shaped word) carries the DualSense mic-mute
button so host-side software can still see the state. The UDP datagram ceiling grew
from 256 to 1500 bytes in both directions to fit an Opus packet inside one Ethernet
MTU. Catalog types and the `backends` array advertise the `mic`/`speaker` feature
slugs for the two Sony types via HIDMaestro, which is the only backend that can
materialize a pad carrying real audio endpoints. The codec, the jitter window and
the HIDMaestro composite personas land in the following changes.

Controller-feedback return paths (protocol 2, extended in place before 2.0.0):
new UDP messages TRIGGER_EFFECTS 0x0010 (raw DualSense adaptive-trigger
blocks, forwarded verbatim from the game's output report) and PLAYER_LEDS
0x0011 (player-indicator bitmask), each gated on new descriptor caps
`triggerEffects` / `playerLeds`. HIDMaestro is the source (it hands back the
game's raw DS5/Switch output reports); the DS5 decode also gains the
documented valid_flag1 lightbar gate and the player-LED byte, and the Switch
decode picks up subcommand 0x30 player lights. Catalog types and the
`backends` array advertise the two new feature slugs (dualsense:
triggerEffects + playerLeds, switchpro: playerLeds).

Second Windows gamepad backend: HIDMaestro (user-mode UMDF2) alongside — not
replacing — ViGEmBus. Windows now offers all four controller types (DualSense
and Switch Pro materialize via HIDMaestro, with motion; Xbox 360 / DualShock 4
keep preferring ViGEm's kernel path and fall back to HIDMaestro when ViGEmBus
is absent). Satellite runs with either driver, both, or none.

Protocol (additive, protocol 1): `GET /api/server/capabilities` and
`GET /api/backend/status` gain a `backends` array (id, vendor, kernelMode,
availability, per-type feature + latency tiers); the singular `backend` object
is unchanged and now reports the preferred-available backend. New backend id
`hidmaestro` with error codes `DRIVER_MISSING` / `HELPER_MISSING`; new
catalog motion requires code `"hidmaestro>=1.7"`. The Windows catalog now
offers ids 2 (dualsense) and 3 (switchpro).

Installer: new optional-but-default "HIDMaestro driver" component
(`satellite-hm-helper.exe`, driver deploys at setup with no reboot), with
`/HIDMAESTRO=auto|bundled|skip` and `/REMOVEHIDMAESTRO=yes|no|auto` switches
mirroring the ViGEmBus ones.

Build system: local builds and CI now run the same rails. Every CI lane's
configure line lives in `CMakePresets.json` (`windows-mingw`, `linux`,
`macos`, `windows-msvc`, plus debug variants), the workflows call the presets
and the shared `scripts/check-format.sh` gate, and new scripts drive the same
presets locally: `scripts/install-deps.{ps1,sh}` (installs exactly CI's
toolchain), `scripts/build.{ps1,sh}` (`[debug|release] [test]`, `-Msvc` for
the hardened lane), `scripts/ci-local.{ps1,sh}` (every PR gate in CI's order;
`--allow-missing`/`-AllowMissing` downgrades a missing tool to a notice),
`scripts/build-installer.ps1` (now passes `/DMyAppVersion` from `/VERSION`
like the release workflow always did), `scripts/build-deb.sh` (CI's cpack
invocation) and `scripts/build-appimage.sh` (the recipe release.yml now
calls). The old root entry points forward to these. Fixes real drift:
`install-dependencies.bat` installed the UCRT64 toolchain while CI builds
MINGW64; the Windows lane now also carries warnings-as-errors like Linux and
macOS; `vcpkg.json` sat at 1.0.0 while `/VERSION` said 1.1.0 (now checked by
version-consistency.yml).

Crash reporting, with the same switch the Dish clients have. Satellite had
never transmitted anything, and on Linux and macOS it had no crash recorder
at all: a segfault died with whatever the distro's core-dump collector
happened to catch. Windows wrote a local minidump nobody was told about. This
adds Sentry behind two independent gates, because one is not enough. The
operator's switch (Settings, Diagnostics, "Share crash reports") is on by
default and turns off with one click, the same opt-out Dish for Android,
Windows and Linux ship, down to the wording, so one switch means one thing
across the family. Turning it off disarms the SDK immediately rather than at
the next restart, since withdrawing consent has to stop the next crash and
not the one after it. [`PRIVACY.md`](PRIVACY.md) is new and says exactly what
a report contains.

The second gate is the build. `SATELLITE_SENTRY_DSN` is empty in CMake and is
only ever filled in by `release.yml` from a repository secret, so a local
build, a PR build and a build from a fork (secrets are not exposed to forks)
carry no DSN and cannot transmit no matter what the switch says. The Sentry
environment is derived from `SATELLITE_RELEASE_VERSION`, which only the release
workflow sets, so `production` is not a label a developer build reaches by
accident -- and because it is only a label (`scripts/build-appimage.sh` sets
that variable when run by hand), the DSN is the gate that actually holds.
The release string uses the display version, so a `-dev` build cannot file
itself against a real release and mix unsymbolicated frames into genuine data.
`$SENTRY_DSN` still works as a developer escape hatch, and still respects the
switch.

What ships per platform. Windows takes the SDK from vcpkg with the crashpad
backend, whose handler is a separate process: CMake stages
`crashpad_handler.exe` beside `satellite.exe`, the installer ships it, and the
installer round-trip test asserts it, because without it Satellite starts,
runs, and captures nothing. Linux has no distro package for sentry-native, so
the release lanes build it from the hash-pinned 0.16.3 release bundle with the
in-process backend, and only when a DSN is present; a PR build or a source
build never pays for it, and `linux-ci.yml` compiles that path once with a
DSN that resolves nowhere so a release tag is not the first thing to try it.
Release builds keep debug information, upload it to Sentry with `sentry-cli`
when the `SENTRY_AUTH_TOKEN` secret exists, and strip it before packaging, so
nothing shipped carries symbols and the reports arrive readable.

Nothing about the local artifacts changes. On Windows the existing
`dumps\*.dmp` writer keeps running, and `dumpFilter` now chains to whatever
top-level filter was installed before it instead of swallowing the exception,
so the local dump and the Sentry report both see the crash rather than
whichever recorder armed last winning outright. Automatic session tracking is
turned off, because it defaults to on and a server meant to run unattended for
weeks should not report every start and stop to anyone; the crash is the
payload. PII is off too, but by not touching it: sentry-native does not send it
by default, and the setter that would change that exists only on Nintendo
Switch.
The status payload reports the opt-in and whether it actually armed as separate
fields, so a build with no DSN says so instead of claiming reports are going
somewhere they are not.

## 1.1.0

No protocol changes. Distribution release: every shipping platform now also
uploads its artifact under a version-less stable name (`SatelliteSetup.exe`,
`satellite-amd64.deb`, `satellite-x86_64.rpm`, `satellite-x86_64.AppImage`),
so `releases/latest/download/<name>` is a permanent link to the newest build.
The stable names are covered by SHA256SUMS and the cosign signatures like
every other asset, and they are a public API: download pages link them, so
they must not be renamed or dropped.

## 1.0.1

No protocol changes. Packaging release: distro-native .deb/.rpm builds,
install smoke tests, Windows installer round-trip in CI, a signed pacman
repository beside the APT and DNF ones, and AppImage zsync self-updates.

## 1.0.0: control-plane rewrite (protocol 1)

Clean-break rewrite of the client ↔ server control plane. Nothing of the old
wire had shipped; there are no legacy paths and no dual-protocol support.

### Protocol

- Control plane is now HTTPS REST, declarative full-state: `PUT
  /api/connections` upserts the complete desired topology keyed on deviceId;
  the server converges and returns applied state. `connectionId` is stable
  across reconnects; `token` rotates per PUT. Full CRUD: session GET
  (reconcile) / DELETE, per-controller `PUT/DELETE
  /api/connections/{id}/controllers/{idx}`.
- UDP is data-plane only. Deleted opcodes 0x0004 ADD / 0x0005 REMOVE /
  0x0006 ACK / 0x0007 SERVER_STATUS / 0x0008 TYPE / 0x000E CAPS_UPDATE and
  all ACK machinery. UDP can no longer mutate topology.
- Heartbeat ack 0x0003 enriched: backend status + session `epoch` +
  active-controller bitmap, so involuntary server-side topology changes
  self-heal within one heartbeat (client GETs + re-PUTs).
- New best-effort session-close notify 0x000F (reasons: shutdown, kicked,
  replaced, unpaired), sent encrypted before teardown.
- Per-session keys: `sessionKey = HKDF-SHA256(pairingKey, sessionSalt,
  token)`; nonce carries a direction byte; counters restart per session.
  Fixes cross-session and downstream nonce reuse on the long-lived key.
- Every authenticated REST call requires `hmacProof =
  HMAC-SHA256(pairingKey, "satellite-proof:" + deviceId)`. A diverged key
  now fails at REST time with a terminal 401 instead of silent UDP churn.
- Deleted the PIN-free already-paired re-pair short-circuit (key
  exfiltration by any LAN actor knowing a deviceId). Re-pair requires a
  fresh PIN or hmacProof of the current key (key rotation).
- New `DELETE /api/pair` (client self-unpair). Unpair (admin or self)
  closes any live session for the device.
- New read-only `GET /api/catalog` (+ `/api/catalog/images/{slug}`):
  localized controller-type catalog (en, es, fr, de, bs, pt-BR) +
  machine-readable host-feature inventory; ETag per (version, locale).
  `GET /api/server/capabilities` now documents dynamic state
  (protocolVersion, serverVersion, maxControllers, backend, motion).
- Host features: session PUT requests `hostFeatures` (v1: `mouseControl`);
  grants ride in the response; streams for ungranted features are dropped.
- Touchpad routing mode moved from the paired device to the controller
  descriptor (client-owned, single writer). The admin touchpad-mode setter
  is gone on both surfaces.
- `protocolVersion` (1) in every pairing/session request/response; 409 on
  mismatch.

### Server behaviour

- Transactional replug on controller type-family changes: the new target is
  plugged on a fresh serial before the old is unplugged; a plug failure
  leaves the old pad untouched and reports `replugFailed` in the response.
- Observable unplug: the backend reports whether removal was accepted;
  unconfirmed unplugs quarantine the serial until the bus closes.
- Serial allocation is round-robin (no instant reuse of a just-freed serial
  while its PnP removal may be in flight); `ensureBusOpen` on every
  reconfigure path.
- REST-open liveness grace (15 s) so half-open sessions surface client-side
  instead of flapping through the reaper.
- Fixed a deadlock between unplug and the backend's rumble/lightbar
  notification workers: unplug joins the worker while holding the session
  lock, so the backend→service callbacks now take it with try_lock and drop
  the frame when contended (safe: both streams are coalesced and
  re-notified, so a dropped frame self-heals).
- `PairedDevice` is copied by value under the config lock in every client
  route (fixes a use-after-unlock).
- Dashboard: one device-centric list (paired + live state chips) instead of
  parallel Connections/Paired Devices sections; virtual-controller rows are
  tagged when their session is not responding; `pluggedIn` reflects adapter
  truth, not `serialNo > 0`. Admin unpair is `DELETE /api/devices/{id}`.

### Docs

- `docs/contract.md` is the single protocol source of truth; it replaces
  `docs/protocol.md`, `docs/connection-api.md` (this repo) and
  `docs/wire-format.md` (dish-android).
