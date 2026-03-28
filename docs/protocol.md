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

| Type   | Name              | Payload                          | Inner size | Total on wire | Direction       |
|--------|-------------------|----------------------------------|------------|---------------|-----------------|
| 0x0001 | Gamepad Data      | controller_index(1B) + XUSB_REPORT(12B) | 4+13 = 17 B | 45 B          | client → server |
| 0x0002 | Heartbeat Ping    | (none)                           | 4 B        | 32 B          | client → server |
| 0x0003 | Heartbeat ACK     | (none)                           | 4 B        | 32 B          | server → client |
| 0x0004 | Controller Add    | controller_index(1B) + caps(2B)  | 4+3 = 7 B  | 35 B          | client → server |
| 0x0005 | Controller Remove | controller_index(1B)             | 4+1 = 5 B  | 33 B          | client → server |

### 0x0001 — Gamepad Data

```
[controller_index (1B)] [XUSB_REPORT (12B)]
```

| Field              | Size | Description                                    |
|--------------------|------|------------------------------------------------|
| `controller_index` | 1 B  | 0-based index of the controller on this client |
| `XUSB_REPORT`      | 12 B | Standard Xbox 360 gamepad report               |

The server maps `(token, controller_index)` → ViGEm controller. A client can
control up to 16 gamepads per connection (indices 0–15), limited by the global
16-controller ViGEm cap shared across all connections.

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
server responds by plugging in a new virtual Xbox 360 controller and mapping
it to `(token, controller_index)`. If the index is already in use or no
slots are available, the packet is dropped (the client can detect this via
the connection list API).

Capability flags (reserved for future use):

| Bit  | Meaning              |
|------|----------------------|
| 0x01 | Has analog triggers  |
| 0x02 | Supports rumble      |

### 0x0005 — Controller Remove

```
[controller_index (1B)]
```

Requests the server to unplug the ViGEm controller at this index. If no
controller exists at this index, the packet is dropped.

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

| Scenario                      | Packet BW  | Wire BW (+28B UDP/IP) |
|-------------------------------|------------|-----------------------|
| 1 gamepad @ 250 Hz            | 11.25 KB/s | 18.25 KB/s            |
| 1 gamepad @ 1000 Hz           | 45 KB/s    | 73 KB/s               |
| 4 gamepads @ 250 Hz           | 45 KB/s    | 73 KB/s               |
| Heartbeat (0.5 pps)           | ~16 B/s    | ~44 B/s               |

