# Changelog

All notable connection-model and protocol changes are recorded here.
The protocol itself is specified in [`docs/contract.md`](docs/contract.md).

## Unreleased: control-plane rewrite (protocol 1)

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
