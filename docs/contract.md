# Dish ↔ Satellite protocol contract

Version: protocol 1 · 2026-06-09. This document is the single source of truth for the
client ↔ server contract. It replaces the former `protocol.md`, `connection-api.md`
(this repo) and `wire-format.md` (dish-android). Both ends implement against this file.

Principle: **control plane and data plane are split.**

- Control = HTTPS REST on **9443**, declarative full-state. The client PUTs its complete
  desired state; the server converges and returns the applied state. Retries are free,
  ordering is irrelevant. UDP never mutates topology.
- Data = UDP streams on **9876**: input, heartbeat, motion, battery, touchpad,
  controller mic audio up; heartbeat ack, rumble, lightbar, trigger effects,
  player LEDs, controller speaker audio, mic LED, session-close notify down.

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
      "caps": { "rumble": true, "motion": true, "analogTriggers": true, "lightbar": false,
                "triggerEffects": false, "playerLeds": false,
                "mic": false, "speaker": false },
      "touchpadMode": "off",
      "preferredBackend": null
    }
  ],
  "hostFeatures": { "mouseControl": true }
}
```

- `controllers` is the COMPLETE desired set. Slots present on the server but absent
  from the array are unplugged. An empty array (or absent key) means "zero controllers".
- `type` is a catalog id (see Catalog). `touchpadMode` ∈ `"ds4" | "mouse" | "off"`.
- `preferredBackend` (additive; `null` or absent = the host chooses) names a backend id
  from `capabilities.backends[]`. It is a **hint, not a mandate**: the host tries that
  backend first when it materializes the requested `type`, and otherwise falls through
  its normal preference order. There is deliberately no result code for "that backend
  refused" — `backend` in the response reports which one actually took the pad.
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
      "backend": "vigem",
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

`backend` (additive) is the id of the backend that materialized this pad, or `null`
when the slot isn't plugged. This is the ONLY place the routing decision is observable:
the host picks per plug, so two slots in one session can legitimately sit on different
backends, and a client that sent `preferredBackend` learns here whether it got it.

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
    { "ctrlIdx": 0, "active": true, "appliedType": 0, "backend": "vigem",
      "caps": { "rumble": true, "motion": true, "analogTriggers": true, "lightbar": false,
                "triggerEffects": false, "playerLeds": false,
                "mic": false, "speaker": false },
      "touchpadMode": "off", "preferredBackend": null,
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
  "backends": [
    { "id": "vigem", "vendor": "Nefarius Software Solutions", "displayName": "ViGEmBus",
      "kernelMode": true, "audio": false, "available": true, "errorCode": null,
      "lifecycle": "eol", "eolDate": "2023-11-02", "driverVersion": null,
      "controllers": [
        { "type": 0, "name": "xbox", "latency": "lowest", "latencyRank": 0,
          "motion": false, "touchpad": false, "lightbar": false, "motionRequires": null,
          "submitLatency": { "tier": "lowest", "rank": 0, "score": 2,
                             "nominalUs": 2, "tailUs": 5, "submitPath": "kernel-direct",
                             "facts": { "kernelCrossings": 1, "threadWakeups": 0,
                                        "brokerHops": 0, "managedRuntime": false,
                                        "pollIntervalUs": 0 } } },
        { "type": 1, "name": "playstation", "latency": "lowest", "latencyRank": 0,
          "motion": true, "touchpad": true, "lightbar": true,
          "motionRequires": "vigembus>=1.17", "submitLatency": { "...": "…" } }
      ] },
    { "id": "hidmaestro", "vendor": "hifihedgehog", "displayName": "HIDMaestro",
      "kernelMode": false, "audio": true, "available": true, "errorCode": null,
      "lifecycle": "supported", "eolDate": null, "driverVersion": null,
      "controllers": [
        { "type": 0, "name": "xbox", "latency": "medium", "latencyRank": 2,
          "motion": false, "touchpad": false, "lightbar": false, "motionRequires": null,
          "submitLatency": { "tier": "medium", "rank": 2, "score": 87,
                             "nominalUs": 36, "tailUs": 515, "submitPath": "usermode-shm",
                             "facts": { "kernelCrossings": 3, "threadWakeups": 2,
                                        "brokerHops": 0, "managedRuntime": false,
                                        "pollIntervalUs": 0 } } },
        { "type": 1, "...": "…" }, { "type": 2, "...": "…" }, { "type": 3, "...": "…" }
      ] }
  ],
  "motion": { "available": true },
  "host": {
    "catalog": { "supported": true },
    "mouseControl": { "supported": true, "available": true },
    "keyboardControl": { "supported": false },
    "rumble": { "supported": true, "available": true }
  },
  "controllerAudio": { "enabled": true, "mic": true, "speaker": true }
}
```

`motion.available` reflects the motion backend right now (e.g. ViGEmBus new enough for
the DS4 EX report). `backend.errorCode` ∈ `DRIVER_MISSING`, `BUS_OPEN_FAILED`,
`HELPER_MISSING`, `MODULE_NOT_LOADED`, `DEVICE_MISSING`, `PERMISSION_DENIED`, or null.

`backends` (additive; absent on older servers) is the host's full backend option list,
most-preferred first — a host can carry more than one (Windows: `vigem` AND
`hidmaestro`; controllers route to the first available backend supporting the
requested type). The singular `backend` object stays the preferred-available backend
(or the most-preferred backend's error when none is available), so existing clients
keep working unchanged. Each entry's `controllers` lists the types that backend can
materialize, with its feature surface for that type (`motionRequires` is the structured
requires code for THAT backend, which is not necessarily the one the catalog quotes —
see the catalog layering rule below) and a derived submit-latency estimate. Ids `vigem`
/ `hidmaestro` / `uinput` / `machid` / `none` and all error codes are protocol
constants, never localized.

`kernelMode` describes that backend's INPUT submit path and nothing else. It stays
`false` for `hidmaestro` even when `audio` is true: input still goes through a user-mode
UMDF2 driver. `audio` (additive; absent on older servers) is the one runtime-switched
field on a backend entry, and it is what tells a reader a kernel component is in play.
It is true only when the backend has a controller type with audio endpoints AND the
host's `controllerAudio` setting is on, because materializing an audio-carrying
(composite) persona means the backend installs a bundled signed kernel USB transport on
first use. The per-controller `mic` / `speaker` columns are the static counterpart: what
that backend COULD materialize, unaffected by the setting (the catalog is cached on
server version + locale, so an install-time switch must not move it). A client that
reads only the columns would offer a microphone on a host that has switched audio off.

`controllerAudio` (top-level; additive, absent on older servers) is the host-level view
of the same switch, split by direction, so a client learns what will ACTUALLY flow
rather than what is merely configured. `enabled` is the host's `controllerAudio`
setting ANDed with whether ANY enumerated backend can carry audio at all, so a host
whose only backend has no audio-capable type reports false however the switches are
set. The per-backend `audio` field stays the place to learn WHICH backend can. `mic`
and `speaker` are the two per-direction settings ANDed with `enabled`, since neither
direction can flow through a persona that was never given audio endpoints. Those two gate the WIRE and not
the persona — the emulated pad keeps both Windows endpoints either way — which is why
they can change under a live stream, while `enabled` only takes effect at the next
plug. The block lives here and deliberately NOT in `/api/catalog`: that response's ETag
is server version + locale and must stay static identity, so no runtime setting may
reach it.

`lifecycle` ∈ `supported` | `maintenance` | `eol` is the UPSTREAM maintenance state of
the driver, carried with `eolDate` (ISO date, or null) and `driverVersion` (the version
the host actually read, or **null when it cannot read one** — never inferred, never
back-filled from a shipped installer's version). All three are protocol constants; the
copy a client renders for them is the client's own. A backend being `eol` says nothing
about whether it works — `available` is the runtime truth, and an `eol` backend may
well be the preferred one.

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

#### Submit-path latency

`submitLatency` — and the flat `latency` / `latencyRank` it backs — is **derived,
never typed.** Those two flat fields keep their original string/integer types, so an
older client parses this payload unchanged; everything new rides alongside them in
`submitLatency`. The registry stores primitive facts about each
backend × type submit path and the server scores them, so a new backend is a row of
facts rather than a judgement call — and a type whose path differs from its siblings'
scores differently with no special case. HIDMaestro signals two events per frame for
Xbox (the input section plus the GIP companion) and one for every Sony/Switch pad, so
its `xbox` row ranks a tier worse than its own `playstation` row.

| Fact | Meaning |
|------|---------|
| `kernelCrossings` | user→kernel transitions per submitted frame |
| `threadWakeups` | scheduler dispatches the frame waits on (the dominant tail term) |
| `brokerHops` | extra user-mode process hops on the hot path (0 for every current backend) |
| `managedRuntime` | GC/JIT anywhere on the per-frame path |
| `pollIntervalUs` | 0 = the consumer is signalled; otherwise its sampling period |

Weights, in microseconds, applied to a typical frame and to a bad one:

| Unit | → `nominalUs` | → `tailUs` |
|------|--------------|-----------|
| `kernelCrossings` | 2 | 5 |
| `threadWakeups` | 15 | 250 |
| `brokerHops` | 40 | 500 |
| `managedRuntime` | 5 | 2000 |
| `pollIntervalUs` | T / 2 | T |

`score = nominalUs + tailUs / 10`, in integer arithmetic (so HIDMaestro's Xbox row is
36 + 515/10 = 36 + 51 = 87). `tier` bands the score — `lowest` < 10 ≤ `low` < 60 ≤
`medium` < 250 ≤ `high` — and `rank` is that tier's ordinal (0–3), mirrored into the
flat `latencyRank`, so a client that only wants ordering keeps sorting on it exactly as
before. `submitPath` ∈ `kernel-direct` | `usermode-shm` | `usermode-broker` is the
structural shorthand, for display; it is derived from the same facts and so can never
disagree with the score.

**Stability rule.** `facts` describe the implementation and change only when the
implementation changes. `nominalUs`, `tailUs`, `score`, `tier`, `rank` and the flat
`latency` / `latencyRank` are DERIVED and MAY shift between server versions as the
weights are recalibrated against measurement. A client that needs a stable comparison sorts on `rank`, or re-derives
from `facts` with its own weights. It MUST NOT persist the absolute microsecond figures
or present them as measured on this host: they are a model of the code path, not a
benchmark of the machine.

For context when presenting them: a physical USB pad polls at ~1 ms, so every figure
these weights can produce for a signalled (non-polling) backend sits inside a single
poll interval. `lifecycle` and `available` are usually the more decision-relevant
fields, and a client that ranks backends by speed alone will be precisely comparing a
quantity the user cannot feel.

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
        "touchpad": { "supported": false },
        "triggerEffects": { "supported": false },
        "playerLeds": { "supported": false },
        "mic": { "supported": false },
        "speaker": { "supported": false }
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
        "touchpad": { "supported": true, "modes": ["ds4"] },
        "triggerEffects": { "supported": false },
        "playerLeds": { "supported": false },
        "mic": { "supported": false },
        "speaker": { "supported": false }
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
- The offered set is the union of the host's configured backends — only identities
  the receiver can natively materialize. Linux/uinput offers all four; Windows offers
  all four (xbox360 + ds4 prefer ViGEm's kernel path, dualsense + switchpro
  materialize via HIDMaestro, which also covers xbox360 + ds4 when ViGEmBus is
  absent); macOS/IOHIDUserDevice offers ds4 (drops the fake Xbox; DualSense pending
  its report codec). The catalog is static per server version (its ETag is keyed on
  version + locale), so a type stays offered even while its backend's driver is
  missing — the plug then fails per-controller (`backendUnavailable` when no backend
  bus opened, else `pluginFailed`) and `capabilities.backends` names which driver to
  fix. A type the server does not offer at all returns per-controller `invalidType`.
- `requires` is a structured code (`"vigembus>=1.17"`, `"hidmaestro>=1.7"`), not
  prose. With multiple backends it names the requirement of the type's PREFERRED
  materializer.
- `controllerTypes[].emulates` is an OPTIONAL physical-pad identity hint —
  `{ "sdlType": …, "usb": [ "vid:pid", … ] }` — naming the physical controller this
  virtual type is the natural default for (e.g. `ds4` ← a `ps4` pad / USB `054c:05c4`).
  It lets a client later match a detected physical pad to a default emulation type with
  NO protocol change: the mapping policy lives on the server, not in each client's
  switch. `sdlType` mirrors the clients' `SDL_GameControllerType` vocabulary (`xbox360`,
  `ps4`, `ps5`, `switchpro`); `usb` is lowercase `vendor:product`, an array so more
  hardware revisions can be added later. Additive within protocolVersion 1; interim
  clients IGNORE it and default to the first offered type. Rides only offered types.
- Type-feature slugs are protocol constants: `rumble`, `analogTriggers`, `motion`,
  `lightbar`, `touchpad`, `triggerEffects` (DualSense adaptive-trigger passthrough,
  RECEIVE), `playerLeds` (player-indicator LEDs, RECEIVE), `mic` (the pad's own
  microphone endpoint, SEND) and `speaker` (the pad's own speaker/headset endpoint,
  RECEIVE). `triggerEffects` and `playerLeds` reflect whether the type's preferred
  materializer surfaces the game's raw output reports; `mic`/`speaker` reflect whether
  it can materialize a pad that carries real audio endpoints (see Controller audio).
  A client advertises the matching descriptor caps only when it can actuate them on
  the physical pad.
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
- **A datagram is at most 1500 bytes**, both directions: one Ethernet MTU, so a full
  packet crosses a LAN without fragmenting (a fragmented audio frame would be an
  all-or-nothing loss anyway). Overhead is header(8) + tag(16), leaving 1476 bytes for
  `msgType | msgLen | payload` and so 1472 bytes of payload. Everything but audio sits
  under 30 bytes; a 20 ms Opus packet is ~80 bytes (mic) to ~240 bytes (speaker), so
  the ceiling exists to absorb a VBR spike rather than to be approached. Senders MUST
  stay inside it; a larger datagram is truncated on read and fails the AEAD.
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
| 0x000C | TOUCHPAD (v1, 16B) | ctrlIdx(1) + flags(1: b0 f0 active, b1 f1 active, b2 button) + f0 id(1)+x(i16 LE)+y(i16 LE) + f1 id(1)+x(i16 LE)+y(i16 LE) + eventTimeMs(u32 LE) |
| 0x000C | POINTER (v2, 19B) | ctrlIdx(1) + fingerFlags(1: b0 f0 active, b1 f1 active) + buttons(1: b0 left/click, b1 right, b2 middle) + f0 id(1)+x(i16 LE)+y(i16 LE) + f1 id(1)+x(i16 LE)+y(i16 LE) + eventTimeMs(u32 LE) + scrollV(i16 LE, 120 per wheel notch, an event: resends carry 0) |
| 0x0012 | MIC_AUDIO | ctrlIdx(1) + seq(u16 BE) + opusPacket(rest, >= 1 byte): one 20 ms Opus packet from the pad's microphone, mono 48 kHz. Accepted only from senders whose descriptor advertised `mic`. See Controller audio. |

Down (server → client):

| Opcode | Name | Payload |
|--------|------|---------|
| 0x0003 | HEARTBEAT_ACK | backendAvailable(1) + totalActiveControllers(1) + epoch(u16 BE) + activeBitmap(u16 BE) |
| 0x0009 | RUMBLE | ctrlIdx(1) + strong(u16 BE) + weak(u16 BE) + durationMs(u16 BE) |
| 0x000D | LIGHTBAR | ctrlIdx(1) + r(1) + g(1) + b(1) |
| 0x000F | SESSION_CLOSE | reason(1: 0 shutdown, 1 kicked, 2 replaced, 3 unpaired) |
| 0x0010 | TRIGGER_EFFECTS | ctrlIdx(1) + left block(11) + right block(11): raw DualSense trigger-effect fields (mode byte + 10 params each), forwarded verbatim from the game's output report. Sent only to senders whose descriptor advertised `triggerEffects`. Coalesced; both blocks always ride together (the server merges per-trigger writes). |
| 0x0011 | PLAYER_LEDS | ctrlIdx(1) + ledMask(1): player-indicator bitmask, bit 0 = leftmost LED (DualSense bits 0-4, Switch Pro bits 0-3). Sent only to senders whose descriptor advertised `playerLeds`. |
| 0x0013 | SPEAKER_AUDIO | ctrlIdx(1) + seq(u16 BE) + opusPacket(rest, >= 1 byte): one 20 ms Opus packet for the pad's speaker/headset output, stereo 48 kHz. Sent only to senders whose descriptor advertised `speaker`. See Controller audio. |
| 0x0014 | MIC_LED | ctrlIdx(1) + state(1: 0 off, 1 on, 2 pulse): the mic-mute lamp state the game asked for. Coalesced last-value-wins like LIGHTBAR. Sent only to senders whose descriptor advertised `mic` (a mute lamp with no microphone behind it has nothing to report). |

Opcodes 0x0004 (ADD), 0x0005 (REMOVE), 0x0006 (ACK), 0x0007 (SERVER_STATUS) and
0x0008 (TYPE), 0x000E (CAPS_UPDATE) are **deleted**: topology mutation is REST-only,
and server status rides in every heartbeat ack. Deleted opcodes are never reused;
new messages claim fresh ids (0x0010+).

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

### Controller audio (0x0012 / 0x0013 / 0x0014)

Scope first, because the name invites the wrong reading: this carries the EMULATED
PAD's OWN audio endpoints, never the host's game audio. A DualSense (or DualShock 4
v2) materialized through a composite persona presents real Windows endpoints, named
after that persona's USB product string — the DualSense composite is
`Speakers (DualSense Wireless Controller)` out and `Headset Microphone (DualSense
Wireless Controller)` in; the DualShock 4 v2 composite carries the plain "Wireless
Controller" string, and its out terminal is a headset rather than a speaker, so
Windows may not label it "Speakers" at all. Whatever a game writes to that speaker
endpoint rides 0x0013 to the client; whatever the client's microphone captures rides
0x0012 back into that mic endpoint. General audio streaming is out of scope and
always will be.

These three messages EXTEND protocol 2 IN PLACE. Version 2 has never been released, so
there is no deployed client to migrate and no version bump: `protocolVersion` stays 2.
Every addition is caps-gated, so an interim protocol-2 client that advertises neither
audio cap sees exactly the traffic it saw before.

| Property | MIC_AUDIO 0x0012 (up) | SPEAKER_AUDIO 0x0013 (down) |
|---|---|---|
| Payload | ctrlIdx(1) + seq(u16 BE) + one Opus packet | same |
| Rate | 48 kHz | 48 kHz |
| Frame | 20 ms = 960 samples per channel | 20 ms = 960 samples per channel |
| Channels | mono | stereo |
| Opus application | VOIP | AUDIO |
| Bitrate | ~32 kbps VBR, in-band FEC on | ~96 kbps VBR, in-band FEC on |
| Silence | Opus DTX on (encoder-side VAD) | all-zero windows not sent, `seq` not advanced |
| Cap gate | `mic` | `speaker` |

- The format is FIXED on the wire, never negotiated. Opus resamples to 48 kHz
  internally regardless, so pinning the rate costs nothing and spares both ends a
  resampler; 20 ms is also the pad's USB-audio service interval, so no side re-windows.
- One message carries exactly one Opus packet. Opus packets are self-delimiting, so
  the frame length IS the packet length; a frame with the 3-byte header and no Opus
  byte is malformed (a silence frame is a 1-byte DTX packet, not an empty one, and the
  mic encoder really does emit those — see the silence bullet below).
- `seq` is u16 and WRAPS. It exists only for gap detection and for late-drop inside the
  receiver's 2-frame (40 ms) reorder window. There are NO acks and NO retransmits: the
  lossy-telemetry rule above applies, and Opus conceals loss itself with in-band FEC
  (the next packet re-carries a coarse copy of the previous one) plus PLC on a gap.
  Adding a reliability layer here would trade a 20 ms hole for unbounded latency.
- Silence is handled differently in each direction, on purpose. The mic encoder runs
  Opus DTX: a live microphone never goes digitally silent, so a VAD is the only thing
  that can collapse a quiet room (measured on libopus 1.6.1: digital silence
  8.4 → 1.1 kbps; -50 dBFS room noise after speech gated 123 of 250 frames,
  30.0 → 16.4 kbps). The speaker encoder declines DTX — that gate cuts anything
  ~26-30 dB below the recent peak, which on game audio replaces a reverb tail or quiet
  ambience with comfort noise at -2.3 dB SNR — and the server instead drops any 20 ms
  window that is EXACTLY all-zero, which is what Windows renders into the endpoint
  whenever nothing is playing to it. That saves ~28 kbps of Opus and ~52 kbps on the
  wire, once the 59 bytes of per-packet framing are counted (IPv4 20 + UDP 8 +
  token/counter 8 + Poly1305 16 + inner header 4 + audio header 3).
- A suppressed speaker window does NOT advance `seq`, and a receiver MUST NOT read the
  resulting cadence as loss. A suppressed window is not a hole: nothing was lost, and
  concealing it would have Opus invent noise where the game wrote none. The opposite
  case, a window whose encode failed, DOES advance `seq` — that 20 ms really happened,
  so the client conceals it rather than playing the stream short and drifting against
  the game's clock.
- Speaker content is channels 1/2 of the DualSense 4-channel OUT stream (its speaker
  and headset jack). Channels 3/4 are the HD-haptics lanes and deliberately never cross
  the wire. DualShock 4 v2 headset stereo rides as-is.
- Server-side sanity limit: MIC_AUDIO beyond ~75 packets/s per controller is dropped
  (nominal is 50). Audio for an unknown session, an unbound slot, or a slot that never
  advertised the cap is likewise dropped, silently on the wire, with one server log
  line per session per cause.

**Mic-mute button.** `GamepadReport.wButtons` is XINPUT-shaped and has exactly one free
bit left, `0x0800`, which is now the DualSense mic-mute button. Only the DualSense
identity maps it into the emulated pad's input report; every other identity ignores it
(no other emulated pad has a mute button). The bit is state, not an edge, like every
other button in the word.

**Mute is the client's to enforce (privacy invariant).** Muted means ZERO MIC_AUDIO
packets on the wire, not silent ones: the client stops delivering capture, and it also
sets `wButtons` bit `0x0800` so host-side software can see the state. A host cannot mute
a client's microphone, and a client that is muted cannot be talked into streaming.
MIC_LED 0x0014 is advisory in the other direction: it reports which lamp state the game
asked for, and the client renders it (last writer wins against its own local mute
state). Because the LED describes a microphone, it rides `mic`, not a cap of its own.

**Caps and catalog.** `mic` and `speaker` follow the house rule that a cap advertises
the CLIENT's actuator/source: the client claims a microphone it can capture from and a
speaker it can play to, and the server sends or accepts accordingly. They are
independent directions; one does not imply the other. The catalog advertises the
type-feature slugs `mic`/`speaker` only for the types whose preferred materializer can
build a pad with audio endpoints (the two Sony types via HIDMaestro's composite
personas); every other type and backend reports `false`. The host's own per-direction
switches are a third, separate thing and live in `/api/server/capabilities`'s
`controllerAudio` block: a cap says what the CLIENT can do, the catalog says what the
host COULD build, and that block says what the host will actually carry right now.

### Host-input streams (reserved)

MIC_AUDIO 0x0012 is the first inhabitant of this slot: a stream that flows INTO the
host from the client, beyond the input reports themselves. It follows the rules below,
with the per-controller cap standing in for a hostFeature grant (audio belongs to one
emulated pad, not to the host).

When host features ship beyond touchpad-mouse (0x000C), their streams get new opcodes
and follow the loss-safety rule: state that must not stick goes FULL-STATE per frame
(keyboard = the complete pressed-key set every packet); deltas only where loss is
benign (mouse moves, audio frames). Streams are valid only for session-granted
hostFeatures; the server drops streams for ungranted features.

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

`protocolVersion` (integer, currently **2**) rides in every pairing/session request and
response. The server ACCEPTS the whole `[supportedMin, supported]` range (currently
**[1, 2]**), settles each session on the client's offer, and echoes that settled
version in the pairing/session responses; absent means 1 (a pre-versioning client).
The settled version keys the session's wire frames: a v1 session streams the 16-byte
TOUCHPAD frame, a v2 session the 19-byte POINTER frame (the receiver also disambiguates
by length, so mixed generations coexist).

An offer OUTSIDE the range is rejected with 409
`{"error":"protocol version unsupported","supported":2,"supportedMin":1}`. `supported`
is the newest version the server speaks: a client seeing `supported` above its own
current version must tell the user to update the app; below its own minimum, to update
the satellite. Version negotiation happens ONLY on pairing and the session PUT;
sub-resource writes (`/controllers/{idx}`) inherit their session's settled version and
carry no `protocolVersion` field.

An in-range but older session works fully at its own version; both ends surface a soft
"update for the newest features" hint (the app's satellite chip, the dashboard's
Update Dish chip + a server log line), never an error.

Version history:

- **1**: initial contract.
- **2**: 0x000C reshaped into the POINTER frame: the click moved out of the finger
  flags into a buttons byte (left/right/middle) and a signed vertical wheel was added.
  Added the controller-feedback return paths: TRIGGER_EFFECTS 0x0010 and PLAYER_LEDS
  0x0011 with their descriptor caps (`triggerEffects`, `playerLeds`) and catalog
  type-feature slugs. Both are caps-gated, so a client that never advertises them
  never sees them; no frame shape changed.
  Then controller audio: MIC_AUDIO 0x0012, SPEAKER_AUDIO 0x0013 and MIC_LED 0x0014 with
  the caps `mic`/`speaker`, the matching catalog slugs, and `wButtons` bit 0x0800 as the
  DualSense mic-mute button. The datagram ceiling rose to 1500 bytes to fit an Opus
  packet. Version 2 was never released, so all of this EXTENDS 2 IN PLACE rather than
  claiming a 3: there is no deployed client to migrate, and every addition is caps-gated
  or additive, so an interim protocol-2 client that advertises neither audio cap sees
  exactly the traffic it saw before.
  Catalog/capabilities documents advertise the server's newest version so clients can
  offer it directly.

## Error model

- 400 malformed request (missing/invalid fields).
- 401 `{"error":"unauthorized","code":"NOT_PAIRED"|"BAD_PROOF"}`: terminal; re-pair.
- 404 unknown resource (or a session that isn't yours).
- 409 protocol version mismatch.
- 503 shutting down; retry later.
- Per-controller failures are 200s with per-controller result codes (partial success
  is normal, not an error).
