# UDP Wire Protocol

## Overview

All communication between client and server over UDP uses a single socket and
a unified packet format. Packets are authenticated and encrypted using
ChaCha20-Poly1305 (libsodium). The token remains plaintext for routing; the
rest is opaque without the session key.

The client discovers the server's `udpPort` (and `httpPort`, `pairPort`)
via the LAN discovery beacon broadcast on `discPort` (default 9879).
See [architecture.md](architecture.md) for the beacon format.

## Packet Format

### Unencrypted (plaintext header)

```
[token (4B)] [counter (4B)] [encrypted payload + auth tag (16B)]
```

| Field     | Size  | Description                                              |
|-----------|-------|----------------------------------------------------------|
| `token`   | 4 B   | Connection token (from `POST /api/connections`)          |
| `counter` | 4 B   | Monotonic nonce, unsigned 32-bit big-endian              |
| encrypted | var   | ChaCha20-Poly1305 ciphertext of the inner message        |
| auth tag  | 16 B  | Poly1305 authentication tag (appended by libsodium)      |

### Encrypted inner message

After decryption the plaintext has this structure:

```
[type (2B, big-endian)] [length (2B, big-endian)] [payload (length bytes)]
```

| Field    | Size  | Description                                             |
|----------|-------|---------------------------------------------------------|
| `type`   | 2 B   | Message type (0x0000–0xFFFF)                            |
| `length` | 2 B   | Payload length in bytes (excludes type+length header)   |
| payload  | 0+ B  | Type-specific data                                      |

**Total packet size** = 8 (header) + 4 (type+length) + N (payload) + 16 (tag)
                      = **28 + N bytes**

## Encryption Details

### Algorithm

ChaCha20-Poly1305 AEAD (libsodium `crypto_aead_chacha20poly1305_ietf_*`).

- **Key**: 256-bit (32-byte) symmetric key, established during pairing via
  X25519 key exchange. Stored as a 64-char hex string, hex-decoded for use.
- **Nonce**: 12 bytes. Constructed by zero-padding the 4-byte counter to 12
  bytes (big-endian, left-padded with zeroes).
- **AAD** (additional authenticated data): the 4-byte `token`. This means the
  token is tamper-proof even though it is not encrypted.
- **Auth tag**: 16 bytes, appended to the ciphertext by libsodium.

### Key Exchange

During the TCP PIN pairing handshake, both sides perform an X25519 key
exchange (libsodium `crypto_scalarmult`):

1. Client sends its X25519 public key (hex-encoded, 64 chars) in the
   `publicKey` field of the pairing request.
2. Server generates an ephemeral X25519 key pair, computes the shared secret
   via `computeSharedKey(clientPk, serverSk, serverPk)`, and returns its
   public key as `serverPublicKey` (hex-encoded) in the pairing response.
3. Both sides independently derive the same 32-byte shared secret. The
   server stores it as `sharedKeyHex` (64-char hex string) in the paired
   device record.

If the client does not send a `publicKey`, the server falls back to
generating a random 32-byte key and returning it as `sharedKey` (hex-encoded)
in the pairing response. This trusted-network fallback sends the key in
plaintext over the TCP pairing socket.

All keys are **hex-encoded** (not Base64) throughout the protocol — pairing
responses, config storage, and internal representations all use 64-character
hex strings for 32-byte keys.

The encryption key is **not** returned in the `POST /api/connections`
response. The client must persist the shared key from the pairing handshake
and use it for all subsequent UDP encryption.

### Counter / Replay Protection

- The `counter` field is a monotonically increasing uint32 per connection.
- The server tracks the highest counter seen per connection.
- Any packet with `counter <= lastSeenCounter` is silently dropped.
- At 1000 Hz, a uint32 counter wraps after ~49 days. The client should
  gracefully re-connect before overflow (or the server can force a
  re-connection at a threshold like 0xFFFFF000).

## Message Types

Types 0x0000–0x00FF are reserved for protocol-level messages.
Application/extension types should use 0x0100+.

| Type   | Name              | Payload                                       | Inner size  | Total on wire | Direction       |
|--------|-------------------|-----------------------------------------------|-------------|---------------|-----------------|
| 0x0001 | Gamepad Data      | controller_index(1B) + XUSB_REPORT(12B)       | 4+13 = 17 B | 45 B          | client → server |
| 0x0002 | Heartbeat Ping    | (none)                                        | 4 B         | 32 B          | client → server |
| 0x0003 | Heartbeat ACK     | (none)                                        | 4 B         | 32 B          | server → client |
| 0x0004 | Controller Add    | controller_index(1B) + caps(2B)               | 4+3 = 7 B   | 35 B          | client → server |
| 0x0005 | Controller Remove | controller_index(1B)                          | 4+1 = 5 B   | 33 B          | client → server |
| 0x0006 | Controller ACK    | requestType(2B) + ctrlIdx(1B) + result(1B)    | 4+4 = 8 B   | 36 B          | server → client |
| 0x0007 | Server Status     | backendAvailable(1B) + activeControllers(1B)  | 4+2 = 6 B   | 34 B          | server → client |
| 0x0008 | Controller Type   | controller_index(1B) + type(1B)               | 4+2 = 6 B   | 34 B          | client → server |
| 0x0009 | Rumble            | controller_index(1B) + rumble fields (7..10B) | 4+8..11 B   | 40..43 B      | server → client |
| 0x000A | Motion (IMU)      | controller_index(1B) + MotionReport(16B)      | 4+17 = 21 B | 49 B          | client → server |
| 0x000B | Battery           | controller_index(1B) + level(1B) + status(1B) | 4+3 = 7 B   | 35 B          | client → server |

### 0x0001 — Gamepad Data

```
[controller_index (1B)] [XUSB_REPORT (12B)]
```

| Field              | Size | Description                                    |
|--------------------|------|------------------------------------------------|
| `controller_index` | 1 B  | 0-based index of the controller on this client |
| `XUSB_REPORT`      | 12 B | Standard Xbox 360 gamepad report               |

The server maps `(token, controller_index)` → backend virtual controller
(ViGEmBus on Windows, /dev/uinput on Linux). A client can control up to 16
gamepads per connection (indices 0–15), limited by the global 16-controller
backend cap shared across all connections.

### 0x0002 — Heartbeat Ping

No payload. The server replies with 0x0003. Any valid packet (including
gamepad data) resets the server's liveness timer for this connection.

### 0x0003 — Heartbeat ACK

No payload. Sent by the server in response to a 0x0002 ping.

### 0x0004 — Controller Add

```
[controller_index (1B)] [capabilities (2B, big-endian)]
```

Requests the server to create a new ViGEm controller for this index. The
server replies with a **0x0006 Controller ACK** indicating success or
failure. On success, the server plugs in a new virtual Xbox 360 controller
and maps it to `(token, controller_index)`.

If the ViGEm bus is not yet open, the server will attempt to open it lazily.
If ViGEm is still unavailable, the ACK reports `ACK_ERR_VIGEM_UNAVAIL`.

Capability flags (reserved for future use):

| Bit  | Meaning              |
|------|----------------------|
| 0x01 | Has analog triggers  |
| 0x02 | Supports rumble      |

### 0x0005 — Controller Remove

```
[controller_index (1B)]
```

Requests the server to unplug the ViGEm controller at this index. The
server replies with a **0x0006 Controller ACK** indicating success or
failure.

### 0x0006 — Controller ACK

```
[requestType (2B, big-endian)] [controller_index (1B)] [result (1B)]
```

Sent by the server in response to a 0x0004 (Controller Add) or 0x0005
(Controller Remove) request.

| Field              | Size | Description                                    |
|--------------------|------|------------------------------------------------|
| `requestType`      | 2 B  | The message type being acknowledged (0x0004 or 0x0005) |
| `controller_index` | 1 B  | The controller index from the original request |
| `result`           | 1 B  | Result code (see below)                        |

**Result codes:**

| Code | Name                   | Description                              |
|------|------------------------|------------------------------------------|
| 0x00 | `ACK_OK`                  | Success                                  |
| 0x01 | `ACK_ERR_BACKEND_UNAVAIL` | Virtual-gamepad backend not available    |
| 0x02 | `ACK_ERR_NO_SLOTS`        | All 16 controller slots are in use       |
| 0x03 | `ACK_ERR_ALREADY_EXISTS`  | Controller already active at this index  |
| 0x04 | `ACK_ERR_NOT_FOUND`       | No active controller at this index       |
| 0x05 | `ACK_ERR_PLUGIN_FAIL`     | Backend plugin call failed               |

> Wire values are stable across platforms. Only the C++ identifier changed
> (`ACK_ERR_VIGEM_UNAVAIL` → `ACK_ERR_BACKEND_UNAVAIL`); existing clients that
> match on the byte value `0x01` continue to work unchanged.

### 0x0007 — Server Status

```
[backendAvailable (1B)] [activeControllerCount (1B)]
```

Sent by the server to inform the client of real-time server state. The
server sends this message:

1. **On every heartbeat response** — alongside the 0x0003 ACK, the server
   sends a 0x0007 with the current backend/controller state.
2. **On every controller add/remove** — after a successful 0x0004 or 0x0005,
   the server broadcasts 0x0007 to **all** connected clients so every client
   sees the updated controller count in real time.
3. **On backend state change** — when the backend bus is lazily opened or
   closed, the server broadcasts 0x0007 to all connected clients.

| Field                    | Size | Description                                    |
|--------------------------|------|------------------------------------------------|
| `backendAvailable`       | 1 B  | `0x01` = backend bus is open and ready, `0x00` = idle/unavailable |
| `activeControllerCount`  | 1 B  | Total number of active virtual controllers across all connections |

**Backend lifecycle is client-driven.** The backend bus is **not** opened at
receiver start. It opens lazily on the first `0x0004 Controller Add` and
closes automatically when the last controller is removed (via `0x0005`,
connection timeout, or explicit disconnect). When `activeControllerCount`
is 0, `backendAvailable` will be `0x00` because the bus is closed.

The client should use this to update the **global** backend status in
its UI (e.g., "BACKEND: OPEN · 3 active"). For **per-device** state
(whether a specific controller was successfully plugged in), the client
should use the **0x0006 Controller ACK** result code. The web dashboard
uses the `GET /api/connections` response which includes `pluggedIn` per
controller.

### 0x0008 — Controller Type

```
[controller_index (1B)] [controller_type (1B)]
```

Sent by the client to declare the cosmetic "kind" of a controller (Xbox vs
PlayStation). The receiver uses this hint to pick a virtual device profile:
PlayStation maps to a ViGEm DS4 / `uhid` DualSense device when available,
otherwise it falls back to an Xbox 360 virtual device.

| Field              | Size | Description                                    |
|--------------------|------|------------------------------------------------|
| `controller_index` | 1 B  | Controller index on this client (0..15)        |
| `controller_type`  | 1 B  | `0x00` = Xbox, `0x01` = PlayStation            |

This message is cosmetic and lossy: the receiver may ignore the request and
keep an already-plugged-in device's type unchanged. Future controller types
extend the byte; clients should treat anything outside the documented set as
"Xbox" to keep older clients forward-compatible.

### 0x0009 — Rumble

```
[controller_index (1B)]
[strongMagnitude (2B, big-endian)]
[weakMagnitude (2B, big-endian)]
[durationMs (2B, big-endian)]
[flags (1B)]
[lightbarR (1B)] [lightbarG (1B)] [lightbarB (1B)]   // present only when flags bit 0 is set
```

Emitted by the **server** when the host game writes to the virtual device's
vibration channel (`XInputSetState` on Windows, `EVIOCSFF` on Linux). The
receiver coalesces identical back-to-back updates so the wire is not flooded
when a game holds both motors at constant magnitudes across frames.

| Field              | Size | Description                                       |
|--------------------|------|---------------------------------------------------|
| `controller_index` | 1 B  | Controller index on the target client             |
| `strongMagnitude`  | 2 B  | Low-frequency / large motor (0..65535)            |
| `weakMagnitude`    | 2 B  | High-frequency / small motor (0..65535)           |
| `durationMs`       | 2 B  | Wire-stamped duration; 0 = continuous             |
| `flags`            | 1 B  | Bit 0 = lightbar present                          |
| `lightbarR/G/B`    | 3 B  | DS4 / DualSense lightbar color (when flag is set) |

Senders that don't recognise the trailing lightbar bytes simply parse the
leading 8 bytes and stop there — the wire is forward-compatible.

### 0x000A — Motion (IMU)

```
[controller_index (1B)]
[gyroX (2B, host LE)] [gyroY (2B, host LE)] [gyroZ (2B, host LE)]
[accelX (2B, host LE)] [accelY (2B, host LE)] [accelZ (2B, host LE)]
[timestampDeltaUs (4B, host LE)]
```

Sent by the client whenever a motion-capable controller emits an IMU sample.
The receiver caches the most recent sample per `(token, controller_index)`
for the web UI's debug pane and forwards it to the virtual device's motion
channel via `IGamepadPort::submitMotion`. The default `IGamepadPort` impl
ignores motion; Windows DS4 / Linux DualSense backends override it to wire
samples into `DS4_REPORT_EX` / `uhid` motion fields (future work — senders
should keep streaming motion regardless).

| Field               | Size | Description                                    |
|---------------------|------|------------------------------------------------|
| `controller_index`  | 1 B  | Controller index on this client (0..15)        |
| `gyroX/Y/Z`         | 6 B  | Three signed int16, scale `2000 / 32767` deg/s |
| `accelX/Y/Z`        | 6 B  | Three signed int16, scale `4 / 32767` g        |
| `timestampDeltaUs`  | 4 B  | Microseconds since the previous MOTION for the same `(token, ctrlIdx)`; 0 on the first packet |

**Axis convention.** Right-handed coordinates: `+X` = right, `+Y` = up,
`+Z` = toward the player. This matches the Cemuhook DSU convention so the
receiver can re-emit on `udp://127.0.0.1:26760` (a future task) without
per-axis renormalisation. **Senders** apply any manufacturer rotation
matrix (DualSense, Joy-Con, etc.) before encoding; **the receiver does not
rotate.**

**Scale.** A gyro reading of `0x7FFF` ≈ `+2000 deg/s`. An accel reading of
`0x7FFF` ≈ `+4 g`. Receivers convert back to deg/s and g by multiplying by
`MOTION_GYRO_SCALE_DEG_S` and `MOTION_ACCEL_SCALE_G` from `core/types.h`.

**Byte order.** Multi-byte fields are written in **host native order** (the
struct is `memcpy`'d onto the wire by the receiver). All current sender and
receiver platforms are little-endian; senders SHOULD write little-endian.

**Rate limiting.** Senders MUST rate-limit motion packets to ≤ 250 Hz per
controller by default; configurable up to the gamepad poll rate. Receivers
MUST tolerate bursts up to the per-controller cap implied by 1000 Hz.

### 0x000B — Battery

```
[controller_index (1B)] [level (1B)] [status (1B)]
```

Periodic battery report from the client. Sent on connection, every
`BATTERY_REPORT_INTERVAL_SEC` (30 s by default), and whenever the charging
state transitions. The receiver caches the most recent values on the
`Controller` and surfaces them in the web UI's `GET /api/connections`.
Windows DS4 backends additionally forward `level` to the virtual device's
battery byte so Steam Big Picture and the host OS can display charge level.

| Field              | Size | Description                                            |
|--------------------|------|--------------------------------------------------------|
| `controller_index` | 1 B  | Controller index on this client (0..15)                |
| `level`            | 1 B  | Percent 0..100, or `0xFF` (`BATTERY_LEVEL_UNKNOWN`)    |
| `status`           | 1 B  | `0`=Unknown, `1`=Discharging, `2`=Charging, `3`=Full, `4`=Wired |

Receivers reject `level` values in `101..254` and `status` values outside
the documented set. Senders that can only read the charging state but not
the percentage SHOULD send `level = 0xFF` plus the known status; senders
that have no battery information at all SHOULD NOT send 0x000B at all.

## Heartbeat / Keepalive

| Parameter            | Value | Description                                    |
|----------------------|-------|------------------------------------------------|
| `HEARTBEAT_INTERVAL` | 2 s   | Client sends a ping every N seconds            |
| `HEARTBEAT_MISS_MAX` | 3     | Missed heartbeats before connection is dead     |

**Client:** Send a 0x0002 ping every `HEARTBEAT_INTERVAL` seconds. Gamepad
data also counts as liveness. If no ACK is received for `HEARTBEAT_MISS_MAX`
consecutive pings, the connection is considered dead.

**Server:** On receiving any valid-token packet, reset `lastPacketTime`. On
receiving 0x0002, reply with 0x0003. A reaper check runs once per second: if
`now - lastPacketTime > HEARTBEAT_INTERVAL * HEARTBEAT_MISS_MAX`, all
controllers for that connection are unplugged and the connection is removed.

## Bandwidth

| Scenario                          | Packet BW  | Wire BW (+28B UDP/IP) |
|-----------------------------------|------------|-----------------------|
| 1 gamepad @ 250 Hz                | 11.25 KB/s | 18.25 KB/s            |
| 1 gamepad @ 1000 Hz               | 45 KB/s    | 73 KB/s               |
| 4 gamepads @ 250 Hz               | 45 KB/s    | 73 KB/s               |
| 1 motion (IMU) @ 250 Hz           | 12.25 KB/s | 19.25 KB/s            |
| 1 motion (IMU) @ 1000 Hz          | 49 KB/s    | 77 KB/s               |
| Battery (1 ctrl, default 0.033 Hz)| ~1.2 B/s   | ~2.5 B/s              |
| Heartbeat (0.5 pps)               | ~16 B/s    | ~44 B/s               |

