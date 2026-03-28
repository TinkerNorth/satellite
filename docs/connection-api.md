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
4. Send 0x0004 Controller Add     → server creates ViGEm controller
5. Send 0x0001 Gamepad Data       → controller input (encrypted UDP on udpPort)
6. Send 0x0005 Controller Remove  → server unplugs one controller
7. DELETE /api/connections/:id     → all controllers removed, connection closed
```

The encryption key is established during pairing (step 2) and stored by both
sides. It is **not** returned in the connection response — the client must
persist it from the pairing handshake.

Steps 4–6 happen over the encrypted UDP channel using the token from step 3.
A client can repeat steps 4–6 as controllers are connected/disconnected.

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
| 503    | `{"error": "no controller slots available"}` | All 16 ViGEm slots in use      |
| 503    | `{"error": "ViGEmBus not available"}`        | Driver not installed           |

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
      "controllers": [
        {
          "controllerIndex": 0,
          "vigemSerialNo": 1
        },
        {
          "controllerIndex": 1,
          "vigemSerialNo": 4
        }
      ]
    }
  ],
  "totalControllers": 5,
  "maxControllers": 16
}
```

Each connection lists its active controllers. `connectedAtEpoch` is seconds
since Unix epoch (steady clock). `totalControllers` is the sum across all
connections. `maxControllers` is the global ViGEm limit (16).

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
  "submitOk": 12300,
  "submitFail": 0,
  "lastLoopUs": 45,
  "decryptFail": 0,
  "replayDrop": 0,
  "logSeq": 42
}
```

> **Note:** The SSE `status` event does **not** include `webPort`. Use
> `GET /api/status` to retrieve `webPort` (the full status endpoint also
> returns `webPort`).
```

**`connections` event:** Same format as `GET /api/connections` response.
