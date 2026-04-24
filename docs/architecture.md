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

## Architecture — Hexagonal (Ports & Adapters)

The server follows **Hexagonal Architecture**. All business logic lives
in `SessionService` (the domain core). External infrastructure (ViGEm
driver, UDP sockets, config files) is accessed through port interfaces,
implemented by concrete adapters.

```
Inbound Adapters          Core Domain           Outbound Adapters
─────────────────     ──────────────────     ──────────────────────
receiver.cpp  ─────►                    ────► ViGEmAdapter (IGamepadPort)
  (UDP recv)         SessionService          pluginDevice, submitReport
                     openSession()
webserver.cpp ─────►  closeSession()    ────► ClientAdapter (IClientPort)
  (HTTP API)         handleGamepadData()     sendHeartbeatAck, sendControllerAck
                     handleHeartbeat()
                     handleControllerAdd()  ──► LogAdapter (ILogPort)
                     getConnectionsSnapshot()   logMsg → ring buffer
```

### Key Design Principles

- **`core/`** contains no Win32, Winsock, or ViGEm `#include`s
- **SessionService** is the sole owner of connection state, serial pool,
  and controller lifecycle — no duplicated teardown logic
- **Adapters** are injected via constructor (dependency injection)
- **`main.cpp`** is the Composition Root — instantiates adapters and
  wires them to the service

## Data Model

### Connection vs. Controller (Device)

**Connections** and **controllers** are separate concepts:

- A **connection** is a network session between a paired client and the
  server. It has a token, encryption key, and IP address. One connection
  can own zero or more controllers.
- A **controller** (device) is an individual virtual gamepad plugged into
  ViGEm. Each controller has its own state (`active`, `serialNo`).
  Controllers are created/removed independently via UDP messages
  (0x0004 / 0x0005) and each receives its own ACK with a per-device
  result code.

```cpp
// core/types.h — pure data, no platform dependencies
struct Controller {
    uint8_t  index    = 0;       // 0-based within connection
    uint32_t serialNo = 0;       // ViGEm serial (1–16), 0 = not plugged
    bool     active   = false;
    GamepadReport lastReport{};
};

struct Connection {
    uint32_t    token       = 0;
    std::string deviceId;
    std::string deviceName;
    std::string clientIP;
    uint8_t     sharedKey[32] = {};
    uint32_t    lastCounter = 0;      // replay protection
    std::chrono::steady_clock::time_point lastPacketTime;
    std::chrono::steady_clock::time_point connectedAt;
    std::array<Controller, 16> controllers;
    int activeControllerCount = 0;
};
```

### State Ownership

All connection and controller state is owned by `SessionService` behind
a single `std::mutex`. There are no global connection maps or serial
arrays. The remaining globals are:

- `g_config` / `g_configMtx` — application configuration
- Atomic telemetry counters (`g_packetCount`, `g_submitOk`, etc.)
- Log ring buffer (`g_logRing`, `g_logMtx`)
- Win32 plumbing (`g_hwnd`, `g_httpServer`, `g_appRunning`)

## Receiver Thread (Inbound UDP Adapter)

`receiver.cpp` is a thin infrastructure layer that owns the UDP socket
and runs a `recvfrom` loop. It delegates all business logic to
`SessionService`.

### Packet Processing Pipeline

```
recvfrom()
  │
  ├─ n < 8 → drop (too small for header)
  │
  ├─ Extract token (bytes 0–3), counter (bytes 4–7)
  │  └─ svc.getDecryptInfo(token) → not found → drop
  │  └─ counter <= lastCounter → drop (replay)
  │
  ├─ Decrypt (ChaCha20-Poly1305)
  │  └─ Fail → drop
  │
  ├─ svc.updatePostDecrypt(token, counter, ip, port)
  │
  ├─ Parse inner message: type (2B) + length (2B) + payload
  │
  ├─ 0x0001 → svc.handleGamepadData(token, ctrlIdx, report)
  ├─ 0x0002 → svc.handleHeartbeat(token)
  ├─ 0x0004 → svc.handleControllerAdd(token, ctrlIdx)
  ├─ 0x0005 → svc.handleControllerRemove(token, ctrlIdx)
  └─ unknown → drop
```

### Reaper

Runs once per second via `svc.reapTimedOut()` — teardown logic is
inside SessionService, not in the receiver.

## HTTP Thread (Inbound HTTP Adapter)

`webserver.cpp` handles `POST/DELETE/GET /api/connections` by calling
SessionService. No connection or controller state is managed here.

### POST /api/connections

1. Validate `deviceId` against paired devices (from `g_config`)
2. Auto-start receiver if needed (`g_wantListen = true`)
3. Hex-decode shared key
4. `svc.openSession(deviceId, name, ip, key)` — handles stale cleanup,
   token generation, slot counting internally
5. Return `connectionId`, `token`, `maxControllers`

> **Note:** Connection succeeds independently of ViGEm. The ViGEm bus
> is only needed at controller-add time (0x0004).

### DELETE /api/connections/:id

1. Parse token from `conn_XXXXXXXX`
2. `svc.closeSession(token)` — handles all teardown internally
3. Return `controllersRemoved` count (or 404 if not found)

### GET /api/connections

`svc.getConnectionsSnapshot()` returns a thread-safe copy of all
connections, controllers, and ViGEm status — no locking needed by the
caller.

## Thread Safety

Both the receiver and HTTP threads call into `SessionService`, which
protects all connection/controller state with a single `std::mutex`.
Lock contention is negligible with ≤ 16 connections and ≤ 16 controllers.

The ViGEm bus handle is owned by `ViGEmAdapter` (with its own mutex).
Report submission (`DeviceIoControl`) is thread-safe per target.

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

