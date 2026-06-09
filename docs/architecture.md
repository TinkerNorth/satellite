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

| Port  | Purpose                        | Config key      |
|-------|--------------------------------|-----------------|
| 9876  | UDP gamepad                    | `udpPort`       |
| 9877  | HTTP API/UI                    | `webPort`       |
| 9878  | TCP pairing                    | `pairPort`      |
| 9879  | UDP discovery (legacy beacon)  | `discPort`      |
| 5353  | mDNS (`_satellite._udp.local.`) | (RFC 6762 fixed) |

## mDNS / Bonjour Service Discovery

In parallel with the legacy UDP broadcast beacon (above), the server
advertises a Bonjour service so senders on subnets that block broadcast
(corporate VLANs, many IoT segments) and Apple devices using the native
Bonjour stack can still find it.

* **Service type:** `_satellite._udp.local.`
* **Multicast group / port:** 224.0.0.251:5353 (per RFC 6762)
* **TXT records:** `udp=<udpPort>`, `pair=<pairPort>`, `http=<webPort>`
* **SRV target:** `<host-label>.local.` on `<udpPort>`

On startup the responder first **probes** for its instance name per
RFC 6762 §8.1: after a random 0-250 ms delay it multicasts three ANY
probe queries 250 ms apart, each carrying its proposed unique records
(SRV/TXT/A) in the authority section. A conflicting response means
another host already owns the name, so the instance label is
disambiguated (`<host>`, `<host> (2)`, `<host> (3)`, …, RFC 6762 §9)
and the probe sequence restarts; conflict rate-limiting (15 conflicts
within 10 s → a 5 s backoff before each probe) is applied, and renaming
is capped at ten attempts. A *simultaneous* probe from a peer for the
same name is not treated as an outright conflict — it is resolved by
the RFC 6762 §8.2 / §8.2.1 lexicographic record-set tiebreak; the loser
defers one second and re-probes. Once three probes complete cleanly the
name is claimed and the §8.3 announcement (below) uses that final name.

After probing, the responder answers PTR / ANY queries for the service
type with PTR + SRV + TXT (+ A) records, and also answers ANY/SRV/TXT/A
queries for its own instance and host names — the latter is how it
*defends* its name against a later peer probe (RFC 6762 §8.1). It
multicasts the full answer set three times ~1 s apart at startup
(RFC 6762 §8.3 unsolicited announcement). On shutdown it multicasts a
goodbye announcement (every record at TTL 0, RFC 6762 §10.1) so resolver
caches on the segment drop the entry immediately. Its live state is
published read-only as `mdnsResponderActive` in `GET /api/status` and
shown in the web UI (Settings → Discovery).

Senders SHOULD prefer the mDNS path when available. The legacy UDP
broadcast beacon stays in place as a fallback for senders that predate
the mDNS responder; it is gated behind `Config::discoveryBroadcastEnabled`
(default `true`, toggleable at runtime from the web UI Settings →
Discovery panel — `POST /api/config`) and is slated for removal in 2027.
Disabling it does not stop the `discoveryThread` worker; the thread keeps
running and simply skips the broadcast send, so re-enabling hot-resumes
the beacon without a restart.

The encoder / parser surface for the mDNS protocol records lives in
`src/net/mdns_protocol.{h,cpp}` and is exercised by
`tests/test_mdns_protocol.cpp` (DNS name encoding, compression-pointer
decoding, query-packet parsing, response building, cache-flush bits, the
TTL-0 goodbye path, §8.1 probe-query shape with the authority-section
proposed records, inbound-probe authority parsing, the §8.2.1
lexicographic tiebreak comparator, and the §9 rename-suffix increment).
The probing state machine, multicast group join / socket bind + recv
loop live in `src/net/mdns_responder.{h,cpp}`.

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

- **`core/`** contains no Win32, Winsock, ViGEm, or `httplib` `#include`s —
  the invariant is enforced literally. Process-wide infrastructure globals
  (config, log ring, telemetry atomics, the HTTP server handle, sockets) live
  in **`src/app/app_state.h`**, a separate infra layer, never in `core/`.
- **SessionService** is the sole owner of connection state, serial pool,
  and controller lifecycle — no duplicated teardown logic
- **Adapters** are injected via constructor (dependency injection)
- **`main.cpp`** is the Composition Root — instantiates adapters and
  wires them to the service

### Where a port belongs

A port exists **for what the domain core needs**, not for every piece of
infrastructure. `SessionService` needs gamepad, client, and log I/O, so those
are ports (`IGamepadPort` / `IClientPort` / `ILogPort`); `UpdateService` adds
`IUpdaterPort`. Configuration is **not** a port: the core never reads it. The
webserver and `UpdateService` are themselves adapters/infrastructure and touch
`g_config` (+ `loadConfig`/`saveConfig`) directly — inverting that behind a port
would add a seam with no domain consumer. (An earlier unused `IConfigPort` /
`ConfigAdapter` pair was removed for exactly this reason.)

### Pattern: a pure codec, separate from its I/O

Every wire format or derived value is split into a **pure, socket-free core**
and a thin I/O shell, so the format is unit-testable without a socket or driver:

| Pure core (tested directly)            | I/O shell                          |
|----------------------------------------|------------------------------------|
| `net/mdns_protocol.{h,cpp}` (encode/parse) | `net/mdns_responder.cpp` (multicast loop) |
| `net/inner_dispatch.cpp` (length guards)   | `net/receiver.cpp` (recvfrom loop) |
| `net/discovery_beacon.h` (`buildDiscoveryBeacon`) | `net/discovery.cpp` (broadcast loop) |
| `net/machine_id.h` (`isValidMachineId`)    | `net/machine_id.cpp` (file + RNG)  |
| `vigem_submit_policy.h` (`ds4ExSubmitLanded`) | `vigem.cpp` (`DeviceIoControl`)  |

When adding a wire format, follow this split — put the byte-shaping in a pure
function and give it a `tests/test_*.cpp` suite.

## Testing

`ctest` (driven by `cmake`) is the single entry point; `build-tests.bat` is a
thin wrapper around it. Each pure unit gets its own portable suite under
`tests/` using the in-repo `TEST`/`EXPECT` macros (no external framework).
Domain logic is tested against **mock ports** (e.g. `MockViGem : IGamepadPort`,
`MockUpdater : IUpdaterPort`), so the core is exercised with zero platform code.
Socket/`httplib` loops and OpenSSL cert generation are intentionally not
unit-tested — their pure cores (above) are the tested seams.

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

### Hot-path discipline

The gamepad packet path runs at controller-polling rate, so the receive
loop is kept allocation-free and minimally locked. If you touch
`receiver.cpp` or `SessionService::handleGamepadDataAndUpdate`, preserve:

- **No heap on the hot path.** `recvfrom` writes into a stack buffer;
  decryption runs **in place** (`ciphertext == plaintext`, relying on
  libsodium's overlapping src/dst support), so the plaintext reuses the
  ciphertext bytes minus the 16-byte tag.
- **One lock for gamepad packets.** A gamepad packet takes exactly one
  `SessionService` mutex acquisition via `handleGamepadDataAndUpdate`
  (decrypt-info lookup, post-decrypt update, and apply fused). Cold paths
  (motion, touchpad, heartbeat, controller add/remove) still take two
  locks — they're rare enough that fusing them isn't worth the API churn.
- **No per-packet string work.** The sender's IPv4 address is threaded
  down as a `uint32` in network byte order; `SessionService` refreshes
  the human-readable cache only when the numeric address changes (no
  `inet_ntop` / `std::string` allocation per packet).
- **Lock-free loop telemetry.** `g_maxLoopUs` uses a thread-local
  high-water-mark so the atomic CAS loop is skipped on the ~99% of
  packets below the running per-second peak.

The conceptual pipeline above lists `getDecryptInfo` /
`updatePostDecrypt` / `handleGamepadData` as distinct steps; on the
gamepad hot path these are the single fused call.

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

