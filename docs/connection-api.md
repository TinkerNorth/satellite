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
state like ViGEm availability.

**Real-time state comes from the UDP layer.** After connecting, the client
receives **0x0007 Server Status** messages over the encrypted UDP channel.
These are sent with every heartbeat response (step 4), after every
controller add/remove, and whenever the server's ViGEm state changes.
The payload includes `vigemAvailable` (1B) and `activeControllerCount`
(1B), so the client can show real-time controller state in its UI.

**ViGEm lifecycle is client-driven.** The ViGEm bus is **not** opened at
server start. It opens lazily when the first controller is added (0x0004)
and closes automatically when the last controller is removed. When no
controllers are active, `vigemAvailable` will be `0x00`.

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
      "controllers": [
        {
          "controllerIndex": 0,
          "vigemSerialNo": 1,
          "vigemPluggedIn": true
        },
        {
          "controllerIndex": 1,
          "vigemSerialNo": 4,
          "vigemPluggedIn": true
        }
      ]
    }
  ],
  "totalControllers": 5,
  "maxControllers": 16,
  "vigemAvailable": true
}
```

Each connection lists its active controllers with **per-device ViGEm state**.
`connectedAtEpoch` is seconds since Unix epoch (steady clock).
`activeControllerCount` is the number of active controllers for that connection.
`totalControllers` is the sum across all connections. `maxControllers` is the
global ViGEm limit (16). `vigemAvailable` indicates whether the ViGEm bus
handle is currently open.

| Field                          | Type   | Description                                             |
|--------------------------------|--------|---------------------------------------------------------|
| `connections[].activeControllerCount` | int | Number of active controllers for this connection |
| `controllers[].controllerIndex`| int    | 0-based controller index within the connection          |
| `controllers[].vigemSerialNo`  | int    | ViGEm serial number (1–16), 0 if not plugged in        |
| `controllers[].vigemPluggedIn` | bool   | Whether this controller is plugged into ViGEm           |
| `vigemAvailable`               | bool   | Whether the ViGEm bus handle is currently open          |

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
  "vigemInstalled": true,
  "vigemAvailable": true,
  "submitOk": 12300,
  "submitFail": 0,
  "lastLoopUs": 45,
  "decryptFail": 0,
  "replayDrop": 0,
  "logSeq": 42
}
```

| Field            | Type | Description                                          |
|------------------|------|------------------------------------------------------|
| `vigemInstalled` | bool | Whether the ViGEm bus driver is installed on the system |
| `vigemAvailable` | bool | Whether the ViGEm bus handle is currently open (controllers active) |

> **Note:** The SSE `status` event does **not** include `webPort`. Use
> `GET /api/status` to retrieve `webPort` (the full status endpoint also
> returns `webPort`).

**`connections` event:** Same format as `GET /api/connections` response,
including per-controller `vigemPluggedIn` status and per-connection
`activeControllerCount`.

## UI Concepts

The web dashboard and client UI separate two distinct concepts:

1. **Connections** — network sessions between a paired client device and
   the server. Each connection has a device name, IP address, and may own
   zero or more controllers. Connections are managed via the HTTP API.

2. **Virtual Controllers** — individual ViGEm gamepads plugged into the
   system. Each controller belongs to exactly one connection and has its
   own ViGEm state (`vigemPluggedIn`, `vigemSerialNo`). Controllers are
   created/removed via UDP messages (0x0004 / 0x0005) and each gets an
   independent ACK with a per-device result code.

The server dashboard shows these in two separate sections: "Connections"
(showing device name, IP, and controller count) and "Virtual Controllers"
(showing each controller with its per-device ViGEm status dot). The
client (Dish) shows "VIGEM BUS" for the global bus state and "DEVICE #N"
for each controller's individual ACK result.
