# Dish ↔ Satellite protocol contract

Version: protocol 1 · 2026-06-09. This document is the single source of truth for the
client ↔ server contract. It replaces the former `protocol.md`, `connection-api.md`
(this repo) and `wire-format.md` (dish-android). Both ends implement against this file.

Principle: **control plane and data plane are split.**

- Control = HTTPS REST on **9443**, declarative full-state. The client PUTs its complete
  desired state; the server converges and returns the applied state. Retries are free,
  ordering is irrelevant. UDP never mutates topology.
- Data = UDP streams on **9876**: input, heartbeat, motion, battery, touchpad up;
  heartbeat ack, rumble, lightbar, session-close notify down.

One user action = one call. Partial success rides in the response body (per-controller
results), never in HTTP error codes.

## Ports

| Port | Transport | Audience | Purpose |
|------|-----------|----------|---------|
| 9443 | HTTPS (TOFU-pinned self-signed TLS) | LAN clients | Pairing, sessions, controllers, catalog, capabilities |
| 9876 | UDP (ChaCha20-Poly1305) | LAN clients | Input/telemetry streams + authenticated notifications |
| 9877 | HTTP, loopback only | Operator | Admin web UI + admin API (never client-facing) |
| 9879 | UDP broadcast | LAN clients | Legacy discovery beacon (fallback; passive on the client) |
| 5353 | mDNS `_satellite._udp` | LAN clients | Primary discovery; TXT carries `mid` (machineId), `pair` (9443) |

There is no client-facing TCP pairing port and no client-facing HTTP API on 9877.

## Identity

- **Satellite identity (as seen by clients): `machineId` only.** The stable id the
  satellite broadcasts in mDNS TXT / beacon. Clients MUST key remembered satellites on
  machineId alone, never on ip/port.
- **Client identity: `deviceId`.** A stable per-install UUID. Pairing plus `hmacProof`
  (below) make it more than a bearer id.

## Pairing: PairedDevice (trust relationship)

A PairedDevice is `{deviceId, deviceName, pairingKey}` persisted on the satellite.
The 32-byte `pairingKey` is minted by the server and handed to the client exactly once
over TLS.

### Create: `POST /api/pair`

Two paths, both PIN-gated. **There is no PIN-free path: a request for an
already-paired deviceId without a valid `hmacProof` is rejected** (the historical
"already paired → hand back the key" short-circuit allowed any LAN actor who learned a
deviceId to exfiltrate the key, and is deleted).

Path A, operator PIN (server-generated, typed into the client):

```json
{ "deviceId": "...", "deviceName": "...", "pin": "1234", "protocolVersion": 1 }
→ 200 { "ok": true, "message": "paired successfully", "sharedKey": "<64-hex>", "protocolVersion": 1 }
```

Path B, client PIN (client-shown, operator approves on the satellite):

```json
{ "deviceId": "...", "deviceName": "...", "clientPin": "5678", "protocolVersion": 1 }
→ 200 { "ok": false, "pending": true, "message": "awaiting approval on the satellite" }
```

The client then polls until the operator acts.

### Optional extension: X25519 pair-key agreement (Path A)

A Path-A request MAY additionally carry the client's X25519 public key; the
pairing key is then derived on both ends instead of being transported:

```json
{ "deviceId": "...", "deviceName": "...", "pin": "1234", "publicKey": "<64-hex>", "protocolVersion": 1 }
→ 200 { "ok": true, "message": "paired successfully", "serverPublicKey": "<64-hex>", "protocolVersion": 1 }
```

- Keypairs are libsodium `crypto_kx` (X25519). The server computes
  `crypto_kx_server_session_keys(rx, tx, serverPk, serverSk, clientPk)` and
  persists `rx` as the 32-byte `pairingKey`; the client computes
  `crypto_kx_client_session_keys(rx, tx, clientPk, clientSk, serverPk)` from
  the response's `serverPublicKey` and uses `tx` (client `tx` equals server
  `rx` by construction).
- On success the response carries `serverPublicKey` INSTEAD of `sharedKey`:
  the long-lived trust root itself never transits the wire.
- An unusable `publicKey` (malformed hex, wrong length, low-order point) fails
  the pair with `{"ok":false,"error":"invalid public key"}` without consuming
  the operator PIN; it is never silently downgraded to a server-minted key.
- Scope: honored on the operator-PIN path only. Path-B approval
  (`GET /api/pair/status`) and proof-based rotation always return `sharedKey`
  (a `publicKey` field is ignored there).
- Baseline unchanged: this extension is OPTIONAL and additive. Every shipping
  client pairs via the server-minted `sharedKey` over TLS (above), and the
  server serves exactly that whenever no `publicKey` is present.

### Read: `GET /api/pair/status?deviceId=...`

Path-B poll. Responses: `{"ok":true,"status":"approved","sharedKey":"<64-hex>"}` exactly
once (the staged key is single-use), else `{"ok":false,"status":"pending"|"none"}`.
There is no `denied` status: an operator deny erases the pending request, so a denied
client polls straight to `none` and must treat it as terminal for that attempt.

### Update (key rotation / re-pair): `POST /api/pair` with `hmacProof`

```json
{ "deviceId": "...", "deviceName": "...", "hmacProof": "<64-hex>", "protocolVersion": 1 }
→ 200 { "ok": true, "message": "key rotated", "sharedKey": "<64-hex new key>" }
```

The proof MUST verify against the **current** key. A failed proof falls through to the
PIN paths (i.e. behaves exactly like a fresh pairing attempt). Rotation closes any live
session for the device (close-notify reason `replaced` first).

### Delete

- Client self-unpair: **`DELETE /api/pair`** with headers `X-Device-Id` and
  `X-Hmac-Proof` (body `{"deviceId","hmacProof"}` also accepted). 200 `{"ok":true}`;
  401 on bad proof.
- Admin: **`DELETE /api/devices/{deviceId}`** (loopback 9877).

**Both close any live session for the device**, sending close-notify 0x000F
(reason `unpaired`) before teardown.

## hmacProof

```
hmacProof = hex( HMAC-SHA256( key = pairingKey, message = "satellite-proof:" + deviceId ) )
```

(libsodium `crypto_auth_hmacsha256`.) Sent on every authenticated REST call, in the
`X-Hmac-Proof` header (alongside `X-Device-Id`) or the JSON body field `hmacProof`
(header wins when both are present). TLS keeps it confidential; its purpose is proof of
key possession, so a client whose key diverged from the server's fails **at REST time**
with 401 instead of producing a silently-undecryptable UDP session.

401 body carries a machine-readable code the client maps to UI:

```json
{ "error": "unauthorized", "code": "NOT_PAIRED" | "BAD_PROOF" }
```

Either code is terminal: the client MUST stop retrying and surface "re-pair needed".
503 means the server is shutting down (retry later is acceptable).

## Session (connection)

At most one session per deviceId, enforced by construction (the session row is keyed on
deviceId). **Zero-controller sessions are valid:** a user sitting in menus is connected
with no pads.

### Create + Update: `PUT /api/connections` (idempotent upsert keyed on deviceId)

Connect and declare the full topology in ONE call; re-PUT converges. The connection row
never churns: `connectionId` is stable across reconnects, `token` rotates per PUT.

Request:

```json
{
  "deviceId": "...",
  "deviceName": "Pixel 9",
  "protocolVersion": 1,
  "hmacProof": "<64-hex>",
  "controllers": [
    {
      "ctrlIdx": 0,
      "type": 0,
      "caps": { "rumble": true, "motion": true, "analogTriggers": true, "lightbar": false },
      "touchpadMode": "off"
    }
  ],
  "hostFeatures": { "mouseControl": true }
}
```

- `controllers` is the COMPLETE desired set. Slots present on the server but absent
  from the array are unplugged. An empty array (or absent key) means "zero controllers".
- `type` is a catalog id (see Catalog). `touchpadMode` ∈ `"ds4" | "mouse" | "off"`.
- `hostFeatures` is the requested set; absent = `{}`. Requests are client-owned desired
  state; grants are server policy. Different fields, not a two-master conflict.

Response (200):

```json
{
  "connectionId": "conn_ab12cd34",
  "token": "0007a1b2",
  "sessionSalt": "<16-hex (8 bytes)>",
  "epoch": 3,
  "maxControllers": 16,
  "protocolVersion": 1,
  "controllers": [
    {
      "ctrlIdx": 0,
      "result": "ok",
      "appliedType": 0,
      "motion": { "sinkSupportedForType": true, "backendOk": true }
    }
  ],
  "hostFeatures": {
    "mouseControl": { "granted": true }
  }
}
```

Per-controller `result` codes (protocol constants, never localized):
`ok`, `noSlots`, `pluginFailed`, `replugFailed`, `backendUnavailable`, `invalidType`,
`invalidIndex`. On `replugFailed` the previous pad is left untouched and `appliedType`
reports the type still in force. Host-feature denials carry a structured reason the
client translates if it knows it: `{"granted": false, "reason": "notSupported" |
"backendUnavailable" | "denied"}`.

Host-input grants are a real privilege step (a phone that can move the mouse owns the
PC). v1 grants `mouseControl` whenever the backend supports it; per-device policy can
land later with zero protocol change.

Descriptor rules:

- **ControllerDescriptor is always sent WHOLE.** A rumble/motion/touchpad toggle is a
  re-send of the descriptor with one field changed; the server converges (replug only
  on a materialization-identity change, e.g. Xbox ↔ DS4 ↔ DualSense ↔ Switch Pro).
- **Single-writer: descriptor fields are client-owned.** The admin UI displays them but
  never sets them. Admin intent is session-level (kick) or trust-level (unpair) only.

### Read: `GET /api/connections/{connectionId}`

Client-scoped to its OWN session (`X-Device-Id` + `X-Hmac-Proof`; 404 for someone
else's id). This is the **reconcile endpoint**: applied descriptors, epoch, liveness.

```json
{
  "connectionId": "conn_ab12cd34",
  "deviceId": "...",
  "epoch": 3,
  "protocolVersion": 1,
  "maxControllers": 16,
  "controllers": [
    { "ctrlIdx": 0, "active": true, "appliedType": 0,
      "caps": { "rumble": true, "motion": true, "analogTriggers": true, "lightbar": false },
      "touchpadMode": "off",
      "motion": { "sinkSupportedForType": true, "backendOk": true } }
  ],
  "hostFeatures": { "mouseControl": { "granted": true } }
}
```

### Delete: `DELETE /api/connections/{connectionId}`

- From the client (authed): graceful close. No notify (the closer already knows).
- From the admin API (loopback): **kick**, close-notify reason `kicked` first.
  Transient by design: a retrying client may re-PUT and reconnect. To keep a device
  out, unpair it.

## Controller (sub-resource; slot ≠ session)

- Create: via the session PUT `controllers[]`, or standalone upsert below.
- Read: via the session GET (covers all slots).
- Update: **`PUT /api/connections/{id}/controllers/{ctrlIdx}`** with the FULL
  descriptor (same JSON as one `controllers[]` element, `ctrlIdx` in the path wins).
  Converges exactly like the session PUT; response is that one controller's apply
  result plus the session `epoch`.
- Delete: **`DELETE /api/connections/{id}/controllers/{ctrlIdx}`** removes the SLOT
  only; the session lives on. Deleting the SESSION is what closes the connection.

Both standalone routes are authed (`X-Device-Id` + `X-Hmac-Proof`) and scoped to the
caller's own session.

## ServerInfo & Catalog (read-only; unauthenticated on 9443, so the UI renders BEFORE pairing)

### `GET /api/server/capabilities`: current DYNAMIC state

```json
{
  "protocolVersion": 1,
  "serverVersion": "1.6.0",
  "maxControllers": 16,
  "backend": { "id": "vigem", "supported": true, "available": true, "errorCode": null },
  "motion": { "available": true },
  "host": {
    "catalog": { "supported": true },
    "mouseControl": { "supported": true, "available": true },
    "keyboardControl": { "supported": false },
    "rumble": { "supported": true, "available": true }
  }
}
```

`motion.available` reflects the motion backend right now (e.g. ViGEmBus new enough for
the DS4 EX report). `backend.errorCode` ∈ `DRIVER_MISSING`, `BUS_OPEN_FAILED`,
`MODULE_NOT_LOADED`, `DEVICE_MISSING`, `PERMISSION_DENIED`, or null.

`host` is the receiver's OWN capability inventory, readable before pairing or any
catalog round-trip so a client reflects the real receiver instead of an optimistic
default. Each entry's `supported` is the static fact (mirrors the catalog
`hostFeatures` / per-type features); `available` is a coarse runtime read. It means the
backend is up enough to accept controllers (bus open), NOT a per-feature delivery
probe, so a client can show a feature present-but-currently-down (e.g. driver
missing). `catalog.supported` is the presence signal: a server emitting `host` always
sets it true, so a client treats its absence as "older satellite, fall back to the
default". The block is additive: an older server omits it and the client degrades
gracefully. (`host.rumble` is the host return-channel; the per-type `rumble` feature
is a different layer, whether the emulated pad has a motor.)

### `GET /api/catalog`: STATIC per server version, localized

Three-layer rule (a field lives in exactly one layer): **catalog** = what exists & how
to present it (static, localized) → **capabilities** = what is true right now
(dynamic) → **PUT/GET session responses** = what was actually applied per controller.

```json
{
  "locale": "en",
  "protocolVersion": 1,
  "serverVersion": "1.6.0",
  "catalogVersion": 2,
  "controllerTypes": [
    {
      "id": 0,
      "slug": "xbox360",
      "name": "Xbox 360 Controller",
      "shortName": "Xbox",
      "description": "Best compatibility. Works with virtually every PC game.",
      "image": { "href": "/api/catalog/images/xbox360", "etag": "\"1.6.0\"" },
      "features": {
        "rumble": { "supported": true },
        "analogTriggers": { "supported": true },
        "motion": { "supported": false },
        "lightbar": { "supported": false },
        "touchpad": { "supported": false }
      },
      "emulates": { "sdlType": "xbox360", "usb": ["045e:028e"] }
    },
    {
      "id": 1,
      "slug": "ds4",
      "name": "DualShock 4",
      "shortName": "PlayStation",
      "description": "PlayStation controller with motion, touchpad and light bar.",
      "image": { "href": "/api/catalog/images/ds4", "etag": "\"1.6.0\"" },
      "features": {
        "rumble": { "supported": true },
        "analogTriggers": { "supported": true },
        "motion": { "supported": true, "requires": "vigembus>=1.17" },
        "lightbar": { "supported": true },
        "touchpad": { "supported": true, "modes": ["ds4"] }
      },
      "emulates": { "sdlType": "ps4", "usb": ["054c:05c4"] }
    }
  ],
  "hostFeatures": {
    "mouseControl": { "supported": true, "modes": ["off", "ds4", "mouse"] },
    "keyboardControl": { "supported": false },
    "rumble": { "supported": true }
  }
}
```

- `catalogVersion` is the catalog SCHEMA version (integer), distinct from `protocolVersion`
  (the wire protocol) and `serverVersion` (the build). It increments when the payload shape
  evolves in a way a client may branch on: v2 offers up to four types per backend and adds
  per-type `emulates`; a response OMITTING the field is the legacy **v1** catalog (xbox360 +
  ds4, no emulates) — a client reads absent as 1. Additive within protocolVersion 1; a client
  MAY substitute its own known representation for a recognized legacy version.
- `controllerTypes[].id` is the wire enum value used as descriptor `type` (0 xbox360,
  1 ds4, 2 dualsense, 3 switchpro). The client renders its "Emulate" picker from this
  list instead of hardcoding the enum.
- The offered set is per-backend — only identities the receiver can natively
  materialize. Linux/uinput offers all four; Windows/ViGEm offers xbox360 + ds4 only
  (no DualSense/Switch target); macOS/IOHIDUserDevice offers ds4 (drops the fake Xbox;
  DualSense pending its report codec). A type the server does not offer but a client
  requests anyway returns per-controller `invalidType`.
- `requires` is a structured code (`"vigembus>=1.17"`), not prose.
- `controllerTypes[].emulates` is an OPTIONAL physical-pad identity hint —
  `{ "sdlType": …, "usb": [ "vid:pid", … ] }` — naming the physical controller this
  virtual type is the natural default for (e.g. `ds4` ← a `ps4` pad / USB `054c:05c4`).
  It lets a client later match a detected physical pad to a default emulation type with
  NO protocol change: the mapping policy lives on the server, not in each client's
  switch. `sdlType` mirrors the clients' `SDL_GameControllerType` vocabulary (`xbox360`,
  `ps4`, `ps5`, `switchpro`); `usb` is lowercase `vendor:product`, an array so more
  hardware revisions can be added later. Additive within protocolVersion 1; interim
  clients IGNORE it and default to the first offered type. Rides only offered types.
- A type-feature MAY carry an explicit `modes` array of protocol-constant mode slugs so
  the client reads the offered modes rather than inferring them from the type id. The
  DS4 `touchpad` advertises `["ds4"]` (its pad-render mode); the relative-mouse path is
  host injection and lives under `hostFeatures.mouseControl`, not the type. A type with
  a touchpad but no `ds4` mode therefore gates the pad off while keeping host mouse.
  Absent `modes` = a pre-modes catalog; the client falls back to its prior assumption.
- `hostFeatures` is PURE capability data: what the HOST can be driven to do,
  independent of any controller slot. Inventory: `mouseControl` (the touchpad
  relative-mouse path; `modes` enumerates valid descriptor `touchpadMode` values),
  `keyboardControl` (host keystroke injection, SEND), and `rumble` (the host streams
  feedback back to the client, RECEIVE). A feature the backend cannot do reports
  `supported: false` (e.g. `keyboardControl` until an injection backend ships) and the
  client leaves it unoffered. Future slugs when implemented: `mediaKeys`, …
- Scope: the catalog describes what the SATELLITE can create or do on the host. Which
  physical pads the client can read stays client-side knowledge.

**Localization boundary rule:** translate only what the client merely DISPLAYS.

- Localized: controller-type `name` / `shortName` / `description` (+ images). These are
  server-owned emulation targets; new types must render on old apps from
  server-provided content.
- NEVER localized (protocol constants): feature slugs (`rumble`, `motion`, …),
  host-feature slugs, touchpad modes, `requires` codes, `emulates` values (`sdlType` /
  `usb`), denial reasons, apply results.
  A client can only offer what it has code for and carries its own translations.
- Consequently: an unknown feature/hostFeature slug is NOT OFFERED (ignored
  gracefully, never an error); an unknown controller-TYPE id/slug DOES render, from
  server-provided name/description/image. Clients never show blank UI for a type newer
  than the app.
- The client MAY override type slugs it recognizes with bundled art/translations.

**Localization mechanics**: standard `Accept-Language`; fallback chain → `en`; the
response echoes the resolved `locale`. Locale set is kept in lockstep with dish-android
(`en`, `es`, `fr`, `de`, `bs`, `pt-BR`) with a CI completeness gate.

**Caching**: `ETag = "<serverVersion>+<locale>"`; `If-None-Match` → 304. Content changes
only on server upgrade, so a client fetches once per satellite version per language.

### `GET /api/catalog/images/{slug}`: SVG, ETag'd

Theme-neutral SVG served by the satellite itself (works fully offline). `ETag =
"<serverVersion>"`, `If-None-Match` → 304. 404 for unknown slugs.

## Crypto

### Keys

- `pairingKey` (32 bytes): long-lived trust root, minted at pairing, stored both ends.
  **Never used to encrypt traffic.**
- `sessionKey` (32 bytes): per-session, derived on both ends after each session PUT:

```
sessionSalt = 8 random bytes, minted by the server per PUT (hex in the response)
sessionKey  = HKDF-SHA256( ikm  = pairingKey,
                           salt = sessionSalt,
                           info = "satellite-session-v1" || token(4 bytes BE) )
```

HKDF-SHA256 is RFC 5869 extract-then-expand built on HMAC-SHA256 (one 32-byte output
block: `T1 = HMAC(PRK, info || 0x01)`).

Because the key rotates with the token every PUT, counters restart at 1 with no
keystream reuse across sessions (the historical flaw: long-lived key + counter reset).

### Packet format (UDP 9876, both directions)

```
cleartext header : token(4 BE) | counter(4 BE)
ciphertext       : ChaCha20-Poly1305-IETF( sessionKey, nonce, AAD = token(4 BE) )
nonce (12 bytes) : dir(1) | 0×7 | counter(4 BE)
   dir = 0x00 client → server, 0x01 server → client
inner plaintext  : msgType(2 BE) | msgLen(2 BE) | payload
```

- Each direction keeps its own monotonically increasing counter, starting at 1. The
  direction byte ensures the two directions never share a nonce even under one key.
- Replay guard (receiver side, per direction): drop when `counter <= lastCounter`
  (first packet exempt while `lastCounter == 0`).
- **Counter exhaustion is impossible to wrap**: a counter can never go backwards, so a
  session that exhausts the 2^32 space goes silent and self-heals via re-PUT
  (fresh token, salt, key). Clients SHOULD proactively re-PUT when their send counter
  crosses 0xF0000000.

## UDP messages

Up (client → server):

| Opcode | Name | Payload |
|--------|------|---------|
| 0x0001 | INPUT | ctrlIdx(1) + GamepadReport(12, XUSB layout, LE) |
| 0x0002 | HEARTBEAT | empty |
| 0x000A | MOTION | ctrlIdx(1) + gyroX/Y/Z(3×i16 LE) + accelX/Y/Z(3×i16 LE) + timestampDeltaUs(u32 LE) |
| 0x000B | BATTERY | ctrlIdx(1) + level(1: 0..100 or 0xFF unknown) + status(1: 0 unknown,1 discharging,2 charging,3 full,4 wired) |
| 0x000C | TOUCHPAD | ctrlIdx(1) + flags(1: b0 f0 active, b1 f1 active, b2 button) + f0 id(1)+x(i16 LE)+y(i16 LE) + f1 id(1)+x(i16 LE)+y(i16 LE) + eventTimeMs(u32 LE) |

Down (server → client):

| Opcode | Name | Payload |
|--------|------|---------|
| 0x0003 | HEARTBEAT_ACK | backendAvailable(1) + totalActiveControllers(1) + epoch(u16 BE) + activeBitmap(u16 BE) |
| 0x0009 | RUMBLE | ctrlIdx(1) + strong(u16 BE) + weak(u16 BE) + durationMs(u16 BE) |
| 0x000D | LIGHTBAR | ctrlIdx(1) + r(1) + g(1) + b(1) |
| 0x000F | SESSION_CLOSE | reason(1: 0 shutdown, 1 kicked, 2 replaced, 3 unpaired) |

Opcodes 0x0004 (ADD), 0x0005 (REMOVE), 0x0006 (ACK), 0x0007 (SERVER_STATUS) and
0x0008 (TYPE), 0x000E (CAPS_UPDATE) are **deleted**: topology mutation is REST-only,
and server status rides in every heartbeat ack.

Corrected stream semantics (errors in the former docs, preserved here):

- INPUT frames are full-state snapshots per packet, loss-safe by construction. Never
  delta-encode them.
- Heartbeat cadence is **2000 ms** (not 250 ms), dead at 5 misses, "not responding"
  display state at 2 misses. Heartbeats stay UDP: their job is proving the DATA path
  works; REST can't.
- RUMBLE `durationMs` is stamped by the server (500 ms when the host API has no
  duration; refresh arrives before expiry). Stop = magnitudes 0,0.
- MOTION scale: gyro ±2000 deg/s, accel ±4 g over int16; right-handed frame, +X right,
  +Y up, +Z toward player. Senders apply the rotation; receivers do not.
- Telemetry streams stay lossy/incremental by design; do NOT add reliability layers.

### Enriched heartbeat ack: the reconcile loop

Every 0x0003 carries the session `epoch` (u16, wraps) and a 16-bit bitmap of active
controller indices. The client compares both against its last-known applied state
(from the latest PUT/GET response): on mismatch → `GET /api/connections/{id}` →
re-PUT desired state. Involuntary server-side losses (replug failure, bus death, reap
of a sibling, kick racing a packet) are therefore SELF-HEALING within one heartbeat
(≤2 s) without REST polling. `epoch` increments on every applied-topology change
regardless of initiator.

### Session-close notify 0x000F: best-effort only

Sent encrypted with the session key BEFORE teardown on: server shutdown (broadcast,
reason `shutdown`), admin kick (`kicked`), session replacement by a new PUT
(`replaced`, to the OLD token), and unpair of a device with a live session
(`unpaired`). Best-effort ONLY: the reliable truth is ack silence → heartbeat death →
REST PUT → status code (401 unpaired / 503 shutting down).

**Security rule for server → client control signals**: they exist ONLY while the
session (and its key) exists, because they must be authenticated: an unauthenticated
"you're unpaired" datagram would be a trivially spoofable DoS. After teardown, silence
plus REST reason codes are the only safe channel; "unpaired" therefore CANNOT be
pushed to an un-keyed client and surfaces as 401 on the next REST contact.

### Host-input streams (reserved)

When host features ship beyond touchpad-mouse (0x000C), their streams get new opcodes
and follow the loss-safety rule: state that must not stick goes FULL-STATE per frame
(keyboard = the complete pressed-key set every packet); deltas only where loss is
benign (mouse moves). Streams are valid only for session-granted hostFeatures; the
server drops streams for ungranted features.

## Liveness

- A fresh/rotated session has a **REST-open grace window of 15 s**: the PUT counts as
  provisional liveness so a half-open session (UDP blocked) doesn't flap while the
  client notices and surfaces the failure.
- After the grace window: a session is "not responding" after 4 s without a decrypted
  packet (2 heartbeat intervals) and reaped after 10 s (5 intervals). Reaping unplugs
  the session's pads (epoch/bitmap of other sessions unaffected).
- HTTPS requests do NOT refresh UDP liveness (they prove the wrong path).

## Admin API (loopback 9877, no auth, operator surface)

| Route | Purpose |
|-------|---------|
| `GET /api/devices` | Paired devices with link state (`paired` / `active` / `notResponding`) |
| `DELETE /api/devices/{deviceId}` | Unpair; closes any live session (notify `unpaired`) |
| `GET /api/connections` | Live sessions + per-controller truth (`pluggedIn` reflects the adapter, not `serialNo > 0`) |
| `DELETE /api/connections/{connectionId}` | Kick (notify `kicked`); transient, client may reconnect |

The admin surface never sets descriptor fields (single-writer rule). The former
`POST /api/devices/touchpad-mode` (both surfaces) and `POST /api/devices/remove` are
deleted; `connectedAtEpoch` in connection JSON is **steady-clock seconds (boot-
relative)**, not Unix epoch.

## Versioning

`protocolVersion` (integer, currently **1**) rides in every pairing/session request and
response. A server MUST reject a major version it doesn't speak with 409
`{"error":"protocol version unsupported","supported":1}`; absent means 1.

## Error model

- 400 malformed request (missing/invalid fields).
- 401 `{"error":"unauthorized","code":"NOT_PAIRED"|"BAD_PROOF"}`: terminal; re-pair.
- 404 unknown resource (or a session that isn't yours).
- 409 protocol version mismatch.
- 503 shutting down; retry later.
- Per-controller failures are 200s with per-controller result codes (partial success
  is normal, not an error).
