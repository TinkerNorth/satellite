# Server Architecture

## Discovery Beacon

The server broadcasts a UDP beacon every 2 seconds on the discovery port
(default 9879) to enable LAN auto-discovery by clients.

### Beacon Format

```json
{"service":"satellite","name":"MyPC","udpPort":9876,"pairPort":9443,"httpPort":9443,"machineId":"<32-hex>"}
```

| Field       | Type   | Description                                          |
|-------------|--------|------------------------------------------------------|
| `service`   | string | Always `"satellite"` (identifies the protocol)       |
| `name`      | string | Computer hostname                                    |
| `udpPort`   | int    | Port for the encrypted UDP data streams              |
| `pairPort`  | int    | HTTPS client API (pairing), always 9443              |
| `httpPort`  | int    | HTTPS client API (sessions, catalog), always 9443    |
| `machineId` | string | Stable per-install id; clients key remembered satellites on it |

`pairPort` and `httpPort` both carry the single HTTPS client-API port:
everything client-facing rides HTTPS 9443. There is no client-facing TCP
pairing port, and the admin UI on 9877 is loopback-only and never advertised.

### Default Ports

| Port  | Purpose                                   | Config key      |
|-------|-------------------------------------------|-----------------|
| 9876  | UDP data streams                          | `udpPort`       |
| 9877  | Admin web UI + admin API (loopback only)  | `webPort`       |
| 9443  | HTTPS client API (pairing, sessions, catalog) | (fixed `DEFAULT_CLIENT_PORT`) |
| 9879  | UDP discovery (legacy beacon)             | `discPort`      |
| 5353  | mDNS (`_satellite._udp.local.`)           | (RFC 6762 fixed)|

## mDNS / Bonjour Service Discovery

In parallel with the legacy UDP broadcast beacon (above), the server
advertises a Bonjour service so senders on subnets that block broadcast
(corporate VLANs, many IoT segments) and Apple devices using the native
Bonjour stack can still find it.

* **Service type:** `_satellite._udp.local.`
* **Multicast group / port:** 224.0.0.251:5353 (per RFC 6762)
* **TXT records:** `udp=<udpPort>`, `pair=9443`, `http=9443`,
  `mid=<machineId>` (the stable per-install id clients dedupe on)
* **SRV target:** `<host-label>.local.` on `<udpPort>`

On startup the responder first **probes** for its instance name per
RFC 6762 §8.1: after a random 0-250 ms delay it multicasts three ANY
probe queries 250 ms apart, each carrying its proposed unique records
(SRV/TXT/A) in the authority section. A conflicting response means
another host already owns the name, so the instance label is
disambiguated (`<host>`, `<host> (2)`, `<host> (3)`, …, RFC 6762 §9)
and the probe sequence restarts. Conflict rate-limiting (15 conflicts
within 10 s triggers a 5 s backoff before each probe) is applied, and
renaming is capped at ten attempts. A simultaneous probe from a peer for
the same name is not treated as an outright conflict; it is resolved by
the RFC 6762 §8.2 / §8.2.1 lexicographic record-set tiebreak, and the
loser defers one second and re-probes. Once three probes complete cleanly the
name is claimed and the §8.3 announcement (below) uses that final name.

After probing, the responder answers PTR / ANY queries for the service
type with PTR + SRV + TXT (+ A) records, and also answers ANY/SRV/TXT/A
queries for its own instance and host names. The latter is how it
defends its name against a later peer probe (RFC 6762 §8.1). It
multicasts the full answer set three times ~1 s apart at startup
(RFC 6762 §8.3 unsolicited announcement). On shutdown it multicasts a
goodbye announcement (every record at TTL 0, RFC 6762 §10.1) so resolver
caches on the segment drop the entry immediately. Its live state is
published read-only as `mdnsResponderActive` in `GET /api/status` and
shown in the web UI (Settings → Discovery).

Senders SHOULD prefer the mDNS path when available. The legacy UDP
broadcast beacon stays in place as a fallback for senders that predate
the mDNS responder. It is gated behind `Config::discoveryBroadcastEnabled`
(default `true`, toggleable at runtime from the web UI Settings →
Discovery panel via `POST /api/config`) and is slated for removal in 2027.
Disabling it does not stop the `discoveryThread` worker: the thread keeps
running and skips the broadcast send, so re-enabling hot-resumes the
beacon without a restart.

The encoder / parser surface for the mDNS protocol records lives in
`src/net/mdns_protocol.{h,cpp}` and is exercised by
`tests/test_mdns_protocol.cpp` (DNS name encoding, compression-pointer
decoding, query-packet parsing, response building, cache-flush bits, the
TTL-0 goodbye path, §8.1 probe-query shape with the authority-section
proposed records, inbound-probe authority parsing, the §8.2.1
lexicographic tiebreak comparator, and the §9 rename-suffix increment).
The probing state machine, multicast group join / socket bind + recv
loop live in `src/net/mdns_responder.{h,cpp}`.

## Architecture: Hexagonal (Ports & Adapters)

The server follows hexagonal architecture. All business logic lives
in `SessionService` (the domain core). External infrastructure (ViGEm
driver, UDP sockets, config files) is accessed through port interfaces,
implemented by concrete adapters.

```
Inbound Adapters          Core Domain           Outbound Adapters
─────────────────     ──────────────────     ──────────────────────
receiver.cpp  ─────►                    ────► ViGEmAdapter (IGamepadPort)
  (UDP recv)         SessionService          pluginDevice, unplugDevice,
                     upsertSession()         submitReport, isDevicePlugged
webserver.cpp ─────►  applyController() ────► ClientAdapter (IClientPort)
  (HTTPS/admin)      removeController()      sendHeartbeatAck, sendSessionClose
                     closeSessionById()
                     handleGamepadData()  ──► LogAdapter (ILogPort)
                     handleHeartbeat()        logMsg → ring buffer
                     getConnectionsSnapshot()
```

### Key Design Principles

- `core/` contains no Win32, Winsock, ViGEm, or `httplib` `#include`s, and
  the invariant is enforced literally. Process-wide infrastructure globals
  (config, log ring, telemetry atomics, the HTTP server handle, sockets) live
  in `src/app/app_state.h`, a separate infra layer, never in `core/`.
- `SessionService` is the sole owner of connection state, serial pool,
  and controller lifecycle, so teardown logic lives in one place.
- Adapters are injected via constructor (dependency injection).
- `main.cpp` is the composition root: it instantiates adapters and
  wires them to the service.

### Where a port belongs

A port exists for what the domain core needs, not for every piece of
infrastructure. `SessionService` needs gamepad, client, and log I/O, so those
are ports (`IGamepadPort` / `IClientPort` / `ILogPort`); `UpdateService` adds
`IUpdaterPort`. Configuration is not a port: the core never reads it. The
webserver and `UpdateService` are themselves adapters/infrastructure and touch
`g_config` (plus `loadConfig`/`saveConfig`) directly. Inverting that behind a
port would add a seam with no domain consumer. (An earlier unused `IConfigPort` /
`ConfigAdapter` pair was removed for exactly this reason.)

### Pattern: a pure codec, separate from its I/O

Every wire format or derived value is split into a pure, socket-free core
and a thin I/O shell, so the format is unit-testable without a socket or driver:

| Pure core (tested directly)            | I/O shell                          |
|----------------------------------------|------------------------------------|
| `net/mdns_protocol.{h,cpp}` (encode/parse) | `net/mdns_responder.cpp` (multicast loop) |
| `net/inner_dispatch.cpp` (length guards)   | `net/receiver.cpp` (recvfrom loop) |
| `net/discovery_beacon.h` (`buildDiscoveryBeacon`) | `net/discovery.cpp` (broadcast loop) |
| `net/machine_id.h` (`isValidMachineId`)    | `net/machine_id.cpp` (file + RNG)  |
| `vigem_submit_policy.h` (`ds4ExSubmitLanded`) | `vigem.cpp` (`DeviceIoControl`)  |
| `platform/macos/ds4_report.h` (DS4 v2 pack/parse/descriptor) | `platform/macos/mac_hid_gamepad_adapter.cpp` (IOHIDUserDevice) |

When adding a wire format, follow this split: put the byte-shaping in a pure
function and give it a `tests/test_*.cpp` suite.

## Testing

`ctest` (driven by `cmake`) is the single entry point; `build-tests.bat` is a
thin wrapper around it. Each pure unit gets its own portable suite under
`tests/` using the in-repo `TEST`/`EXPECT` macros (no external framework).
Domain logic is tested against mock ports (e.g. `MockViGem : IGamepadPort`,
`MockUpdater : IUpdaterPort`), so the core is exercised with zero platform code.
Socket/`httplib` loops and OpenSSL cert generation are intentionally not
unit-tested; their pure cores (above) are the tested seams.

## Data Model

### Connection vs. Controller (Device)

Connections and controllers are separate concepts:

- A connection is a network session between a paired client and the
  server, keyed on `deviceId` and stable across reconnects: a re-PUT
  rotates the token/salt/session-key in place, and the row (and its pads)
  never churns. It carries a stable `connectionId`, the per-session key, the
  reconcile `epoch`, and the host-feature grants. Zero-controller sessions
  are valid.
- A controller (slot) is an individual virtual gamepad plugged into the
  backend. Slots are declared via `ControllerDescriptor`s in the session PUT
  (or the standalone controller PUT/DELETE); the service converges the
  backend to the declared set and reports per-slot apply results. UDP never
  mutates this set.

```cpp
// core/types.h: pure data, no platform dependencies (abridged)
struct Controller {
    uint8_t  index    = 0;       // 0-based within connection
    uint32_t serialNo = 0;       // backend serial (1-16), 0 = not plugged
    bool     active   = false;
    uint8_t  controllerType;     // device family actually plugged
    uint16_t caps;               // CAP_* word from the descriptor
    uint8_t  touchpadMode;       // per-slot routing (client-owned)
    GamepadReport lastReport{};
};

struct Connection {
    std::string connectionId;    // stable across reconnects
    uint32_t    token = 0;       // rotates on every session PUT
    std::string deviceId;
    std::string deviceName;
    uint8_t     sessionKey[32];  // HKDF(pairingKey, salt, token)
    uint8_t     sessionSalt[8];
    uint32_t    lastCounter = 0; // replay protection (client → server)
    uint16_t    epoch = 0;       // bumps on every applied-topology change
    std::chrono::steady_clock::time_point lastPacketTime;
    std::chrono::steady_clock::time_point graceUntil; // REST-open liveness grace
    std::array<Controller, 16> controllers;
    bool mouseControlGranted = false;
};
```

### State Ownership

All connection and controller state is owned by `SessionService` behind
a single `std::mutex`. There are no global connection maps or serial
arrays. The remaining globals are:

- `g_config` / `g_configMtx`: application configuration
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
  ├─ Extract token (bytes 0-3), counter (bytes 4-7)
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
  ├─ 0x0001 → svc.handleGamepadDataAndUpdate(...)   (fused hot path)
  ├─ 0x0002 → svc.handleHeartbeat(token)            (enriched ack out)
  ├─ 0x000A/B/C → motion / battery / touchpad streams
  └─ unknown (incl. the deleted registration opcodes) → drop
```

Topology mutation is REST-only (see `contract.md`); the registration
opcodes 0x0004/0x0005/0x0008/0x000E no longer exist, so a spoofed or
stale datagram can never plug, unplug, or retype a controller.

### Hot-path discipline

The gamepad packet path runs at controller-polling rate, so the receive
loop is kept allocation-free and minimally locked. If you touch
`receiver.cpp` or `SessionService::handleGamepadDataAndUpdate`, preserve:

- No heap on the hot path. `recvfrom` writes into a stack buffer, and
  decryption runs in place (`ciphertext == plaintext`, relying on
  libsodium's overlapping src/dst support), so the plaintext reuses the
  ciphertext bytes minus the 16-byte tag.
- One lock for gamepad packets. A gamepad packet takes exactly one
  `SessionService` mutex acquisition via `handleGamepadDataAndUpdate`
  (decrypt-info lookup, post-decrypt update, and apply fused). Cold paths
  (motion, touchpad, heartbeat, controller add/remove) still take two
  locks; they're rare enough that fusing them isn't worth the API churn.
- No per-packet string work. The sender's IPv4 address is threaded
  down as a `uint32` in network byte order; `SessionService` refreshes
  the human-readable cache only when the numeric address changes (no
  `inet_ntop` / `std::string` allocation per packet).
- Lock-free loop telemetry. `g_maxLoopUs` uses a thread-local
  high-water-mark so the atomic CAS loop is skipped on the ~99% of
  packets below the running per-second peak.

The conceptual pipeline above lists `getDecryptInfo` /
`updatePostDecrypt` / `handleGamepadData` as distinct steps; on the
gamepad hot path these are the single fused call.

### Reaper

Runs once per second via `svc.reapTimedOut()`. Teardown logic is
inside SessionService, not in the receiver.

## HTTP Threads (Inbound HTTP Adapters)

`webserver.cpp` runs two servers, the loopback admin UI (9877) and the
HTTPS client API (9443), both calling into SessionService. No connection
or controller state is managed here. Route semantics, auth (`hmacProof`),
and body shapes are specified in [`contract.md`](contract.md); the notes
below are server-internal.

### PUT /api/connections (client API)

1. `clientAuthed()`: resolve the paired device by `X-Device-Id`, COPY the
   `PairedDevice` by value under `g_configMtx` (never hold a pointer across
   the unlock), verify `X-Hmac-Proof` against the pairing key
2. Parse the full descriptor set (`core/json.h`, nlohmann/json)
3. `svc.upsertSession(...)`: converges topology, rotates token/salt/key,
   derives the session key via the injected HKDF, returns applied state
4. Serialize per-controller results + host-feature grants

The session upsert succeeds independently of ViGEm. Per-controller
failures (`backendUnavailable`, `noSlots`, …) ride in the response body;
partial success is normal, not an HTTP error.

### DELETE /api/connections/:id

Client API: authed, scoped to the caller's own session, no close-notify.
Admin API kick: `svc.closeSessionById(..., CLOSE_REASON_KICKED, notify)`
sends the encrypted 0x000F before teardown.

### GET /api/connections (admin)

`svc.getConnectionsSnapshot()` returns a thread-safe copy of all
connections, controllers (with adapter-truth `pluggedIn`), and backend
status, so the caller needs no locking.

## Thread Safety

Both the receiver and HTTP threads call into `SessionService`, which
protects all connection/controller state with a single `std::mutex`.
Lock contention is negligible with ≤ 16 connections and ≤ 16 controllers.

The ViGEm bus handle is owned by `ViGEmAdapter` (with its own mutex).
Report submission (`DeviceIoControl`) is thread-safe per target.

## macOS backend: virtual DualShock 4 via IOHIDUserDevice

The macOS backend publishes each plugged slot as a kernel HID device
carrying a DualShock 4 v2 identity (VID 0x054C / PID 0x09CC). macOS's
native DS4 support and GameController.framework adopt it like real
hardware, so games get sticks/buttons/triggers plus rumble, lightbar,
touchpad, and motion with no per-game integration.

### Pure/IO split

- `src/platform/macos/ds4_report.h` — IOKit-free: the HID report
  descriptor bytes, input-report packing (reusing the core
  `ds4PackTouchFinger` 12-bit touch codec and the Windows XUSB→DS4
  button/stick mapping byte-for-byte), output-report parsing (rumble
  motors + lightbar RGB, gated by the report's valid flags), and the
  calibration / firmware / pairing feature blobs. The calibration blob
  is chosen so consumers' scaling math is the identity: wire
  `MotionReport` values pass through unscaled (the wire full-scale
  convention equals the DS4's raw sensor scale). Tested by
  `tests/test_macos_ds4_report.cpp` with no kernel or entitlement.
- `src/platform/macos/mac_hid_gamepad_adapter.{h,cpp}` — the IOKit
  shell: device lifecycle (`IOHIDUserDeviceCreateWithProperties` →
  activate → cancel + confirmed teardown), one serial dispatch queue
  per device, set-report → rumble/lightbar callback fan-out, get-report
  → feature-blob table. A DriverKit dext transport could replace this
  file without touching the pure layer.

### Entitlement story and fallback

Creating the kernel device requires the
`com.apple.developer.hid.virtual.device` entitlement (production
builds carry it). `probeBackend()` derives its answer from a one-shot
runtime probe — a minimal vendor-page device create, cached for the
process lifetime — through the pure `macHidBackendStatus` seam:

- entitled → backend id `machid`, `supported`/`available` true;
- unentitled, or an SDK without the IOHIDUserDevice header at compile
  time → **exactly** the historical stub values (`none`, unsupported,
  unavailable): the web UI hides the backend panel and controller
  descriptors apply as `backendUnavailable`, byte-identical to the
  pre-backend macOS build.

CI runners and dev machines are unentitled and take the fallback path;
`tests/test_mac_hid_smoke.cpp` asserts the fallback contract and the
callback fan-out first, then exits 77 (ctest reports "skipped") when it
cannot create real devices, mirroring the uinput smoke.

### Slot identity

Both `pluginDevice` (Xbox slot) and `pluginDeviceDS4` publish the same
DS4 hardware identity: macOS adopts no XUSB-shaped HID device, and the
DS4 is the one identity the whole mac game stack recognises. The slot
keeps its declared family to gate motion/touchpad/battery routing and
`motionBackendOk` exactly like the ViGEm and uinput adapters, and
`appliedType` semantics in `SessionService` are untouched.

### Locking

Unlike ViGEm (which drops its lock for the submit IOCTL), submits run
entirely under the adapter mutex: a slot's `IOHIDUserDeviceRef` is only
ever used while the slot is still in the map, so teardown can never
release a ref under an in-flight submit. Teardown removes the slot from
the map under the lock, then cancels the device and waits for its
cancel handler OUTSIDE the lock (the set-report block takes the mutex
briefly to snapshot callbacks — waiting while holding it would
deadlock); an unconfirmed cancel returns false so `SessionService`
quarantines the serial. The full protocol is documented in
`mac_hid_gamepad_adapter.h`.

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

| Source     | Events logged                                                  |
|------------|----------------------------------------------------------------|
| `receiver` | Socket bind/rebind, listen state                               |
| `service`  | Session create/rotate/close, controller plug/replug/remove, reaper timeouts, serial quarantine, bus open/close |
| `pairing`  | Pair success, pair failure (bad PIN), key rotation, unpair     |
| `client`   | HTTPS client-API auth failures (401s), malformed requests      |
| `web`      | Admin config changes, update preferences                       |

### API Integration

- `GET /api/logs?since=N` returns entries with seq > N (incremental fetch)
- SSE stream (`GET /api/events`, admin) multiplexes six event types
  (`status`, `connections`, `devices`, `update`, `pin`, `pairRequests`)
  pushed every second; `status` includes `logSeq` so the frontend knows
  when new logs are available without polling the log endpoint

