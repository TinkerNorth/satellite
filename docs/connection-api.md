# Connection Management API

## Overview

A **connection** represents a single paired device (client) communicating with
the server. One connection can manage **multiple virtual controllers** (up to
the global 16-controller ViGEm limit shared across all connections).

No controllers exist until explicitly created. Controllers are created and
removed via UDP messages (0x0004 / 0x0005) or automatically cleaned up when
the connection closes.

## Client Flow

```
1. Listen for discovery beacon    (UDP broadcast on discPort, default 9879)
   → parse httpPort, udpPort, pairPort from beacon JSON
2. Pair device (if needed)        (TCP PIN handshake on pairPort — includes
                                    X25519 key exchange; client stores shared key)
3. POST http://<ip>:<httpPort>/api/connections → get connectionId, token
4. Send 0x0002 Heartbeat Ping     → server responds with 0x0003 ACK + 0x0007 Status
5. Send 0x0004 Controller Add     → server responds with 0x0006 ACK
6. Send 0x0001 Gamepad Data       → controller input (encrypted UDP on udpPort)
7. Send 0x0005 Controller Remove  → server responds with 0x0006 ACK
8. DELETE /api/connections/:id     → all controllers removed, connection closed
```

The encryption key is established during pairing (step 2) and stored by both
sides. It is **not** returned in the connection response — the client must
persist it from the pairing handshake.

**Connection is decoupled from ViGEm.** Step 3 always succeeds if the device
is paired and not already connected — regardless of whether the ViGEm bus
driver is available. The POST response contains **only** connection metadata
(connectionId, token, maxControllers). It does **not** include any real-time
state like backend availability.

**Real-time state comes from the UDP layer.** After connecting, the client
receives **0x0007 Server Status** messages over the encrypted UDP channel.
These are sent with every heartbeat response (step 4), after every
controller add/remove, and whenever the server's backend state changes.
The payload includes `backendAvailable` (1B) and `activeControllerCount`
(1B), so the client can show real-time controller state in its UI.

**Backend lifecycle is client-driven.** The backend bus is **not** opened at
server start. It opens lazily when the first controller is added (0x0004)
and closes automatically when the last controller is removed. When no
controllers are active, `backendAvailable` will be `0x00`.

> The backend is ViGEmBus on Windows and `/dev/uinput` on Linux. macOS has
> no backend, so all `0x0004` requests are answered with `ACK_ERR_BACKEND_UNAVAIL`
> and `backendAvailable` is always `0x00`.

Steps 5–7 happen over the encrypted UDP channel using the token from step 3.
A client can repeat steps 5–7 as controllers are connected/disconnected.
Each controller add/remove gets a **0x0006 Controller ACK** from the server
with a result code indicating success or failure (see [protocol.md](protocol.md)).

## Authentication

All endpoints require authentication via one of:

1. **Session cookie** — web UI login sets an `HttpOnly` cookie (`session=<token>`)
2. **`deviceId` in request body** — the `deviceId` field in the JSON body is
   matched against `g_config.pairedDevices`
3. **`X-Device-Id` header** — same lookup, useful when the body doesn't contain
   `deviceId` (e.g. GET/DELETE requests)

If the server has no credentials configured yet (first-time setup), all
endpoints are open without authentication.

---

## Pairing

Pairing establishes the shared key for a device and runs over the HTTPS client
API. There are **two directions, and either completes a pairing** — whichever
screen the user is in front of:

- **Path A — server PIN.** The operator generates a PIN on the satellite
  (`POST /api/pin/generate`, web dashboard) and the user types it into the
  dish. The dish proves it in `POST /api/pair` and pairs immediately.
- **Path B — dish PIN.** The dish shows *its own* PIN and submits it as a
  request; the operator accepts it — either on the web dashboard (by typing the
  PIN back), **from the native OS notification** the satellite raises (Windows:
  an actionable toast with inline Accept/Reject; macOS: notification → alert;
  Linux: libnotify actions), **from the tray right-click menu**, or **from the
  dashboard's Pairing Requests panel** (a header badge flags pending ones). All
  show the PIN to confirm by sight. The dish polls `GET /api/pair/status` until
  the operator acts. The PIN is never sent over the HTTP API; confirming it is
  what authenticates the device.

### `POST /api/pair`

Client API (HTTPS), no device auth (the device is not paired yet).

**Request body:**
```json
{ "deviceId": "uuid", "deviceName": "Pixel 8", "pin": "1234", "clientPin": "4821", "publicKey": "…", "touchpadMode": "ds4" }
```

| Field          | When         | Description                                              |
|----------------|--------------|----------------------------------------------------------|
| `deviceId`     | always       | Stable per-install dish id                               |
| `deviceName`   | optional     | Shown on the dashboard                                   |
| `pin`          | Path A       | The PIN the operator generated on the satellite          |
| `clientPin`    | Path B       | The PIN the dish is **showing**; submits an approval request |
| `publicKey`    | optional     | Dish X25519 public key; when present the key is derived rather than server-minted |
| `touchpadMode` | optional     | Initial touchpad routing (`ds4` \| `mouse` \| `off`)     |

**Responses (always HTTP 200; classify on the body):**

| Body                                                        | Meaning                                  |
|-------------------------------------------------------------|------------------------------------------|
| `{"ok":true,"sharedKey":"<64hex>"}`                         | Paired (Path A, or already-paired). Persist the key. |
| `{"ok":true,"serverPublicKey":"<64hex>"}`                   | Paired via X25519 (client sent `publicKey`); derive the key locally. |
| `{"ok":false,"pending":true,"message":"…"}`                 | Path B request registered — poll `GET /api/pair/status`. |
| `{"ok":false,"error":"invalid or expired PIN"}`             | No valid server PIN and no `clientPin` to request with. |

### `GET /api/pair/status?deviceId=<id>`

Client API (HTTPS), no device auth. The dish polls this after a Path-B request.

| Body                                                | Meaning                                           |
|-----------------------------------------------------|---------------------------------------------------|
| `{"ok":false,"status":"pending"}`                   | Awaiting the operator's decision.                 |
| `{"ok":true,"status":"approved","sharedKey":"…"}`   | Accepted. The key is handed back **exactly once** (the request is then cleared); persist it. |
| `{"ok":false,"status":"none"}`                      | No request (expired, already consumed, or denied). |

### `GET /api/pair/requests`

Admin API (localhost). Lists in-flight Path-B requests for the dashboard's
accept/deny panel. Deliberately **omits the PIN** — accepting requires typing
the PIN shown on the dish.

```json
[ { "deviceId": "uuid", "deviceName": "Pixel 8", "clientIP": "192.168.1.42", "secondsRemaining": 96 } ]
```

### `POST /api/pair/respond`

Admin API (localhost). The operator's accept/deny action.

**Request body:** `{ "deviceId": "uuid", "pin": "4821", "accept": true }`

On `accept:true` the `pin` must match the PIN the dish is showing. On success
the device is persisted as paired and the key is staged for the dish's next
`GET /api/pair/status`. `accept:false` dismisses the request.

| Body                                                  | Condition                          |
|-------------------------------------------------------|------------------------------------|
| `{"ok":true,"accepted":true}`                         | Accepted (PIN matched).            |
| `{"ok":true,"accepted":false}`                        | Dismissed.                         |
| `{"ok":false,"error":"pin mismatch or no pending request"}` | Wrong PIN, or the request expired. |

---

### `POST /api/connections`

Opens a new connection for a paired device.

**Request body:**
```json
{
  "deviceId": "paired-device-uuid"
}
```

**Success response (201):**
```json
{
  "connectionId": "conn_a1b2c3d4",
  "token": "3a1f8b0c",
  "maxControllers": 16
}
```

| Field            | Type   | Description                                          |
|------------------|--------|------------------------------------------------------|
| `connectionId`   | string | Unique ID for this connection (used in DELETE/GET)   |
| `token`          | string | 4-byte token, hex-encoded. Prepended to every UDP packet |
| `maxControllers` | int    | Max controllers this connection may create           |

The `maxControllers` value accounts for the global 16-slot limit minus
controllers already in use by other connections.

**Connection succeeds independently of ViGEm.** The server does not require
the ViGEm bus to be available to create a connection. ViGEm opens lazily on
the first controller add (0x0004) and closes when the last controller is
removed. Real-time ViGEm/controller state is delivered over the UDP layer
via **0x0007 Server Status** messages — not in this HTTP response. The
client should handle the case where controller add ACKs return
`ACK_ERR_VIGEM_UNAVAIL`.

The encryption key is **not** included in this response. The client must
use the shared key established during the pairing handshake (X25519 key
exchange or trusted-network fallback). See [protocol.md](protocol.md) for
key exchange details.

**Error responses:**

| Status | Body                                         | Condition                      |
|--------|----------------------------------------------|--------------------------------|
| 400    | `{"error": "missing deviceId"}`              | No `deviceId` in body          |
| 403    | `{"error": "device not paired"}`             | Unknown `deviceId`             |
| 409    | `{"error": "device already connected"}`      | `deviceId` already has conn    |
| 500    | `{"error": "invalid shared key"}`            | Stored key is corrupt/missing  |

---

### `DELETE /api/connections/:id`

Closes a connection. **All controllers** owned by this connection are
unplugged.

**URL parameter:** `id` — the `connectionId` from the POST response.

**Request body (optional, for sender auth):**
```json
{
  "deviceId": "paired-device-uuid"
}
```

Either a valid session cookie **or** a matching `deviceId` is required.

**Success response (200):**
```json
{
  "ok": true,
  "controllersRemoved": 2
}
```

**Error responses:**

| Status | Body                                 | Condition             |
|--------|--------------------------------------|-----------------------|
| 404    | `{"error": "connection not found"}`  | Unknown connectionId  |
| 403    | `{"error": "forbidden"}`             | Not owner / not authed|

---

### `GET /api/connections`

Lists all active connections and their controllers. Requires session cookie.

**Success response (200):**
```json
{
  "connections": [
    {
      "connectionId": "conn_a1b2c3d4",
      "deviceId": "paired-device-uuid",
      "deviceName": "Living Room Phone",
      "connectedAtEpoch": 1711547400,
      "senderIP": "192.168.1.42",
      "activeControllerCount": 2,
      "touchpadMode": "ds4",
      "controllers": [
        {
          "controllerIndex": 0,
          "serialNo": 1,
          "pluggedIn": true,
          "controllerType": "playstation",
          "controllerTypeLabel": "PlayStation",
          "battery": { "level": 80, "status": "discharging" },
          "motionCapable": true,
          "motionActive": true,
          "motionSink": true,
          "touchpadActive": true,
          "lightbarCapable": true,
          "lightbar": "#1040ff"
        },
        {
          "controllerIndex": 1,
          "serialNo": 4,
          "pluggedIn": true,
          "controllerType": "xbox",
          "controllerTypeLabel": "Xbox",
          "battery": null,
          "motionCapable": false,
          "motionActive": false,
          "motionSink": false,
          "touchpadActive": false,
          "lightbarCapable": false,
          "lightbar": null
        }
      ]
    }
  ],
  "totalControllers": 5,
  "maxControllers": 16,
  "backendAvailable": true
}
```

Each connection lists its active controllers with **per-device backend state**.
`connectedAtEpoch` is seconds since Unix epoch (steady clock).
`activeControllerCount` is the number of active controllers for that connection.
`touchpadMode` is the connection's current touchpad routing mode (see
[`POST /api/devices/touchpad-mode`](#post-apidevicestouchpad-mode)).
`totalControllers` is the sum across all connections. `maxControllers` is the
global backend limit (16). `backendAvailable` indicates whether the backend
bus handle is currently open.

| Field                          | Type   | Description                                             |
|--------------------------------|--------|---------------------------------------------------------|
| `connections[].activeControllerCount` | int | Number of active controllers for this connection |
| `connections[].touchpadMode`   | string | Touchpad routing for the connection: `"ds4"`, `"mouse"`, or `"off"` |
| `controllers[].controllerIndex`| int    | 0-based controller index within the connection          |
| `controllers[].serialNo`       | int    | Backend serial number (1–16), 0 if not plugged in       |
| `controllers[].pluggedIn`      | bool   | Whether this controller is plugged into the backend     |
| `controllers[].controllerType` | string | Stable type id: `"playstation"`, `"xbox"`, etc.          |
| `controllers[].controllerTypeLabel` | string | Human-readable controller type label               |
| `controllers[].battery`        | object\|null | Battery report, or `null` if the sender reported none |
| `controllers[].battery.level`  | int\|null | Charge percentage 0–100, or `null` when unknown      |
| `controllers[].battery.status` | string | `"charging"`, `"discharging"`, `"full"`, `"wired"`, or `"unknown"` |
| `controllers[].motionCapable`  | bool   | Controller has a gyro/accelerometer (IMU)                |
| `controllers[].motionActive`   | bool   | Motion packets are currently being received from the controller |
| `controllers[].motionSink`     | bool   | Motion is being delivered to the OS-level virtual gamepad. `false` while `motionActive` is `true` means the virtual device exposes no IMU surface (Xbox-typed device, ViGEmBus older than 1.22, or macOS) |
| `controllers[].touchpadActive` | bool   | Touchpad samples are currently flowing for this controller |
| `controllers[].lightbarCapable`| bool   | Controller has an addressable RGB lightbar               |
| `controllers[].lightbar`       | string\|null | Last lightbar colour as `"#rrggbb"`, or `null` until the host game sets one |
| `backendAvailable`             | bool   | Whether the backend bus handle is currently open         |

The same per-controller fields appear in the SSE `connections` event.

---

### `POST /api/devices/touchpad-mode`

Sets a paired device's touchpad routing mode. The mode is persisted to config
**and** hot-applied to any live connection for that device, so the change takes
effect without re-pairing or reconnecting. Requires a session cookie or a
matching `deviceId` (paired-sender auth).

**Request body:**
```json
{
  "id": "paired-device-uuid",
  "mode": "ds4"
}
```

| Field  | Type   | Description                                                        |
|--------|--------|--------------------------------------------------------------------|
| `id`   | string | The paired device id (as returned by `GET /api/devices`)           |
| `mode` | string | `"ds4"` — forward into the virtual DualShock 4 touchpad surface (PlayStation-typed virtual controllers only); `"mouse"` — relative mouse pointer on the host; `"off"` — ignore touchpad input |

**Success response (200):**
```json
{
  "ok": true,
  "hotApplied": true
}
```

`hotApplied` is `true` when the new mode was pushed to a currently-live
connection for the device, `false` when the device is paired but not currently
connected (the mode is still persisted and will apply on the next connection).

> **Note:** `"ds4"` only produces a touchpad surface on a PlayStation-typed
> virtual controller. For an Xbox-typed controller the samples have nowhere to
> land and are dropped — the web dashboard flags this mismatch on the
> per-controller touchpad chip.

**Error responses:**

| Status | Body                                            | Condition                          |
|--------|-------------------------------------------------|------------------------------------|
| 400    | `{"error": "missing id or mode"}`               | `id` or `mode` absent/empty        |
| 400    | `{"error": "mode must be ds4, mouse, or off"}`  | `mode` not one of the three values |
| 404    | `{"error": "device not paired"}`                | Unknown `id`                       |

---

### `POST /api/config`

Updates server configuration. Requires a session cookie or a matching
`deviceId`. Every field is optional — only the keys present in the body are
applied, so a partial POST cannot silently reset an omitted setting.

**Request body:**
```json
{
  "udpPort": 9876,
  "autoStart": false,
  "discoveryBroadcastEnabled": true
}
```

| Field                       | Type | Description                                                          |
|-----------------------------|------|----------------------------------------------------------------------|
| `udpPort`                   | int  | UDP listen port. Accepted range **1024–65535**; a value outside the range is rejected (the stored port is left unchanged). |
| `autoStart`                 | bool | Start the receiver automatically at OS login                         |
| `discoveryBroadcastEnabled` | bool | Legacy UDP broadcast discovery beacon. `true` keeps the beacon on as a fallback for senders that predate the mDNS responder; `false` disables it (mDNS / Bonjour then becomes the only discovery path). Omit the key to leave the current value untouched. |

**Success response (200):**
```json
{
  "ok": true,
  "udpPort": 9876,
  "udpPortRejected": false
}
```

`udpPort` echoes the **effective** port after the update — clients should
re-seed their port field from this value rather than assuming the requested
port took effect. `udpPortRejected` is `true` when a `udpPort` was supplied but
fell outside the accepted range (the previous port was kept).

---

### `GET /api/logs`

Returns server log entries from the in-memory ring buffer (last 500 entries).

**Query parameters:**

| Param  | Type   | Default | Description                                   |
|--------|--------|---------|-----------------------------------------------|
| `since`| uint64 | 0       | Return only entries with sequence > this value |

**Success response (200):**
```json
{
  "seq": 42,
  "entries": [
    {
      "seq": 41,
      "ts": 1711547400123,
      "level": "info",
      "source": "receiver",
      "message": "Controller #0 added (serial 1) for Living Room Phone"
    },
    {
      "seq": 42,
      "ts": 1711547401456,
      "level": "warn",
      "source": "web",
      "message": "Failed login attempt for user: admin"
    }
  ]
}
```

| Field            | Type   | Description                                       |
|------------------|--------|---------------------------------------------------|
| `seq`            | uint64 | Current global log sequence number                |
| `entries[].seq`  | uint64 | Sequence number of this entry                     |
| `entries[].ts`   | uint64 | Timestamp in milliseconds since epoch             |
| `entries[].level`| string | `"info"`, `"warn"`, or `"error"`                  |
| `entries[].source`| string| Subsystem that produced the log (`receiver`, `web`, `pairing`) |
| `entries[].message`| string| Human-readable log message                      |

The `since` parameter enables incremental polling — the client tracks the
last `seq` it received and only requests newer entries. The SSE stream
(below) also broadcasts the current `logSeq` so the frontend knows when
new logs are available without polling.

---

### `GET /api/events` (Server-Sent Events)

Real-time event stream. Emits two event types every ~1 second:

**`status` event:**
```json
{
  "listening": true,
  "packets": 12345,
  "senderIP": "192.168.1.42",
  "udpPort": 9876,
  "autoStart": false,
  "backendAvailable": true,
  "backend": {
    "id": "uinput",
    "supported": true,
    "available": true,
    "errorCode": null
  },
  "submitOk": 12300,
  "submitFail": 0,
  "lastLoopUs": 45,
  "decryptFail": 0,
  "replayDrop": 0,
  "logSeq": 42
}
```

| Field               | Type   | Description                                                              |
|---------------------|--------|--------------------------------------------------------------------------|
| `backendAvailable`  | bool   | Whether the backend bus handle is currently open (controllers active)    |
| `backend.id`        | string | Stable backend identifier: `"vigem"`, `"uinput"`, or `"none"` (macOS)    |
| `backend.supported` | bool   | `false` on macOS — clients should hide backend-specific UI when false    |
| `backend.available` | bool   | Whether the backend is *probeable* right now (can a controller be added) |
| `backend.errorCode` | string\|null | Per-backend remediation tag when not available — see [`/api/backend/status`](#get-apibackendstatus) |

> **Note:** The SSE `status` event does **not** include `webPort`. Use
> `GET /api/status` to retrieve `webPort` (the full status endpoint also
> returns `webPort`).

**`connections` event:** Same format as `GET /api/connections` response,
including per-controller `pluggedIn` status and per-connection
`activeControllerCount`.

---

### `GET /api/backend/status`

Returns the structured backend probe — the server-side data that the web UI
keys its remediation copy off. The web UI maintains a copy table indexed by
`(backend.id, errorCode)` so all user-facing strings live in JS, not in C++.

**Success response (200):**
```json
{
  "id": "uinput",
  "supported": true,
  "available": false,
  "errorCode": "PERMISSION_DENIED"
}
```

| Field       | Type   | Description |
|-------------|--------|-------------|
| `id`        | string | Backend identifier — `"vigem"` (Windows), `"uinput"` (Linux), `"none"` (macOS) |
| `supported` | bool   | `false` when there's no backend at all (macOS) — UI should hide the panel    |
| `available` | bool   | Whether the backend is currently usable                                      |
| `errorCode` | string\|null | When `available` is `false`, names the specific failure mode           |

**Per-backend error codes:**

| Backend  | Code                | Meaning                                                          |
|----------|---------------------|------------------------------------------------------------------|
| `vigem`  | `DRIVER_MISSING`    | ViGEmBus device interface not registered                         |
| `vigem`  | `BUS_OPEN_FAILED`   | Device interface present, but `CreateFile` / version check failed |
| `uinput` | `DEVICE_MISSING`    | `/dev/uinput` does not exist (uinput module likely not loaded)   |
| `uinput` | `PERMISSION_DENIED` | `/dev/uinput` exists but the running user can't write to it      |

Adding a new code on the server is non-breaking — clients render a generic
fallback message until a matching entry is added to the web copy table.

## UI Concepts

The web dashboard and client UI separate two distinct concepts:

1. **Connections** — network sessions between a paired client device and
   the server. Each connection has a device name, IP address, and may own
   zero or more controllers. Connections are managed via the HTTP API.

2. **Virtual Controllers** — individual backend gamepads plugged into the
   system. Each controller belongs to exactly one connection and has its
   own backend state (`pluggedIn`, `serialNo`). Controllers are created/
   removed via UDP messages (0x0004 / 0x0005) and each gets an independent
   ACK with a per-device result code.

The server dashboard shows these in two separate sections: "Connections"
(showing device name, IP, and controller count) and "Virtual Controllers"
(showing each controller with its per-device status dot). The
client (Dish) shows "VIGEM BUS" for the global bus state and "DEVICE #N"
for each controller's individual ACK result.
