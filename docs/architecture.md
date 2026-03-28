# Server Architecture

## Discovery Beacon

The server broadcasts a UDP beacon every 2 seconds on the discovery port
(default 9879) to enable LAN auto-discovery by clients.

### Beacon Format

```json
{"service":"satellite","name":"MyPC","udpPort":9876,"pairPort":9878,"httpPort":9877}
```

| Field      | Type   | Description                                      |
|------------|--------|--------------------------------------------------|
| `service`  | string | Always `"satellite"` — identifies the protocol   |
| `name`     | string | Computer hostname (from `GetComputerNameA`)       |
| `udpPort`  | int    | Port for encrypted gamepad UDP packets            |
| `pairPort` | int    | Port for TCP PIN pairing handshake                |
| `httpPort` | int    | Port for the HTTP API and web UI                  |

Clients listen on the discovery port for these broadcasts. The `httpPort`
field tells the client where to send HTTP requests (`POST /api/connections`,
etc.) without hardcoding or guessing.

### Default Ports

| Port | Purpose       | Config key |
|------|---------------|------------|
| 9876 | UDP gamepad   | `udpPort`  |
| 9877 | HTTP API/UI   | `webPort`  |
| 9878 | TCP pairing   | `pairPort` |
| 9879 | UDP discovery | `discPort` |

## Data Model

### Connection

One connection per paired device. Owns zero or more controllers.

```cpp
struct Controller {
    uint8_t     index = 0;       // 0-based index within the connection
    ULONG       serialNo = 0;    // ViGEm serial number (1–16)
    bool        active = false;
    XUSB_REPORT lastReport{};
    HANDLE      submitEvent = nullptr; // pre-allocated overlapped event
};

struct Connection {
    uint32_t    token = 0;           // 4-byte token for UDP routing
    std::string deviceId;            // paired device that owns this
    std::string deviceName;
    std::string clientIP;
    uint8_t     sharedKey[32] = {};   // ChaCha20-Poly1305 key (from pairing)
    uint32_t    lastCounter = 0;      // highest counter seen (replay protection)
    std::chrono::steady_clock::time_point lastPacketTime;
    std::chrono::steady_clock::time_point connectedAt;
    std::array<Controller, 16> controllers; // fixed-size array
    int         activeControllerCount = 0;
    sockaddr_in clientAddr{};         // for sending replies (heartbeat ACK)
};
```

### Global State

```cpp
// Token → Connection (for UDP packet routing and HTTP API lookups)
std::mutex g_connMtx;
std::unordered_map<uint32_t, Connection> g_connections;

// ViGEm bus handle — opened once at receiver start
HANDLE g_busDevice;  // INVALID_HANDLE_VALUE until opened

// Tracks which ViGEm serial numbers are in use (index 0 = serial 1)
std::mutex g_serialMtx;
bool g_serialInUse[16];

// Crypto/stats counters
std::atomic<uint64_t> g_decryptFail;  // failed decryptions
std::atomic<uint64_t> g_replayDrop;   // replay drops
SOCKET g_udpSock;                     // shared UDP socket for replies
```

## Receiver Thread

The receiver thread owns the UDP socket and runs a single `recvfrom` loop.

### Packet Processing Pipeline

```
recvfrom()
  │
  ├─ n < 8 → drop (too small for header)
  │
  ├─ Extract token (bytes 0–3)
  │  └─ Lookup in g_connections → not found → drop
  │
  ├─ Extract counter (bytes 4–7)
  │  └─ counter <= lastCounter → drop (replay)
  │
  ├─ Decrypt (ChaCha20-Poly1305)
  │  ├─ Key:   connection.sharedKey
  │  ├─ Nonce: counter zero-padded to 12 bytes
  │  ├─ AAD:   token (4 bytes)
  │  └─ Fail → drop (tampered or wrong key)
  │
  ├─ Parse inner message: type (2B) + length (2B) + payload
  │
  ├─ type 0x0001 (Gamepad Data):
  │  ├─ Extract controllerIndex (payload byte 0)
  │  ├─ Lookup controller in connection.controllers
  │  │  └─ Not found → drop
  │  ├─ Submit XUSB_REPORT (payload bytes 1–12) to ViGEm
  │  └─ Update lastPacketTime
  │
  ├─ type 0x0002 (Heartbeat Ping):
  │  ├─ Encrypt 0x0003 ACK with connection key
  │  ├─ Send to clientAddr
  │  └─ Update lastPacketTime
  │
  ├─ type 0x0004 (Controller Add):
  │  ├─ Extract controllerIndex + capabilities
  │  ├─ Check: index not already in connection.controllers
  │  ├─ Check: g_serialInUse has a free slot
  │  ├─ Allocate ViGEm serial, call pluginTarget
  │  └─ Insert into connection.controllers
  │
  ├─ type 0x0005 (Controller Remove):
  │  ├─ Extract controllerIndex
  │  ├─ Lookup in connection.controllers
  │  ├─ Call unplugTarget, free ViGEm serial
  │  └─ Remove from connection.controllers
  │
  └─ unknown type → drop
```

### Reaper Check

Runs once per second (or every N loop iterations via a timer):

1. Lock `g_connMtx`
2. For each connection: if `now - lastPacketTime > HEARTBEAT_INTERVAL * HEARTBEAT_MISS_MAX`:
   - Unplug all controllers (call `unplugTarget` for each)
   - Free all ViGEm serials
   - Remove from `g_connections`
3. Unlock

## HTTP Thread

The HTTP thread handles `POST/DELETE/GET /api/connections`.

### POST /api/connections

1. Validate `deviceId` against paired devices
2. Check ViGEmBus is available (`g_busDevice`)
3. Lock `g_connMtx`
4. Check `deviceId` not already connected (scan `g_connections`)
5. Check `g_serialInUse` is not full (at least one slot available)
6. Generate random 4-byte token (ensure unique in `g_connections`)
7. Hex-decode the paired device's `sharedKeyHex` into the connection's `sharedKey`
8. Create `Connection` object (no controllers yet)
9. Insert into `g_connections`
10. Unlock
11. Return `connectionId`, `token`, `maxControllers`

> **Note:** The encryption key is **not** returned. The client already has
> the shared key from the pairing handshake.

### DELETE /api/connections/:id

1. Lock `g_connMtx`
2. Find connection in `g_connections` (parse token from `conn_XXXXXXXX`)
3. Unplug all controllers, free serials
4. Remove from `g_connections`
5. Unlock
6. Return `controllersRemoved` count

### GET /api/connections

1. Lock `g_connMtx`
2. Iterate `g_connections`, build JSON response
3. Unlock

## Thread Safety

Two threads access shared state:

| Thread   | Reads                          | Writes                              |
|----------|--------------------------------|-------------------------------------|
| Receiver | token lookup, controller lookup, key, counter | lastPacketTime, lastCounter, packets, controllers (add/remove via 0x0004/0x0005), reaper cleanup |
| HTTP     | connection list                | create/destroy connections          |

A single `std::mutex g_connMtx` protects all mutations. The receiver thread
holds the lock briefly per packet for the token lookup (read key, serial,
update timestamp). Controller add/remove (0x0004/0x0005) also lock.
The HTTP thread locks for create/destroy/list. With n ≤ 16 connections and
≤ 16 total controllers, lock contention is negligible.

### ViGEm Thread Safety

`pluginTarget` and `unplugTarget` calls are serialized by `g_connMtx`.
The ViGEm bus handle is opened once and never closed until shutdown.
`DeviceIoControl` (gamepad report submission) is thread-safe per target.

## Logging Infrastructure

### Ring Buffer

A fixed-size in-memory ring buffer stores the last 500 log entries. Each
entry has a monotonically increasing sequence number.

```cpp
struct LogEntry {
    std::chrono::system_clock::time_point timestamp;
    LogLevel level;                              // INFO, WARN, ERR
    std::string source;                          // subsystem name
    std::string message;                         // human-readable text
};

std::vector<LogEntry> g_logRing(500); // ring buffer (pre-allocated)
int g_logHead = 0;                    // next write position
uint64_t g_logSeq = 0;               // monotonic sequence counter
std::mutex g_logMtx;                  // protects all of the above
```

`logMsg(level, source, message)` appends to the ring under the mutex.

### Log Sources

| Source     | Events logged                                                |
|------------|--------------------------------------------------------------|
| `receiver` | Reaper timeouts, controller add/remove, socket bind          |
| `pairing`  | Server start, pair success, pair failure (bad PIN), re-pair  |
| `web`      | Login success/failure, setup, config changes, connection open/close, all connection error paths |

### API Integration

- `GET /api/logs?since=N` returns entries with seq > N (incremental fetch)
- SSE stream includes `logSeq` in the `status` event so the frontend knows
  when new logs are available without polling the log endpoint

