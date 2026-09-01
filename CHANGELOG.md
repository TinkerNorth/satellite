# Changelog

All notable connection-model and protocol changes are recorded here.
The protocol itself is specified in [`docs/contract.md`](docs/contract.md).

## Unreleased

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

Controller audio, wire contract and server plumbing (protocol 2, unreleased,
extended in place): the emulated pad's OWN audio endpoints now have a protocol.
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

Controller-feedback return paths (protocol 2, unreleased, extended in place):
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
