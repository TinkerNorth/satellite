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
receiver.cpp  ─────►                    ────► GamepadMux (IGamepadPort)
  (UDP recv)         SessionService           ├─► ViGEmAdapter (Windows)
                     upsertSession()          ├─► HidMaestroAdapter (Windows)
webserver.cpp ─────►  applyController() ────► ClientAdapter (IClientPort)
  (HTTPS/admin)      removeController()      sendHeartbeatAck, sendSessionClose
                     closeSessionById()
                     handleGamepadData()  ──► LogAdapter (ILogPort)
                     handleHeartbeat()        logMsg → ring buffer
                     getConnectionsSnapshot()
```

On Windows the composition root wires BOTH gamepad backends behind one
`core/gamepad_mux.h` composite: `SessionService` keeps a single
`IGamepadPort` reference and a single serial pool, and the mux records
which child owns each serial at plug time (preference order: ViGEm's
kernel path first, HIDMaestro for the identities ViGEm cannot
materialize — DualSense, Switch Pro — and as the fallback when ViGEmBus
is absent). Linux (uinput) and macOS (IOHIDUserDevice) keep their single
adapter; the mux is pure and would take a second adapter there
unchanged. `core/backend_registry.{h,cpp}` is the data-driven identity /
vendor / per-type capability + latency-tier table both the
`/api/server/capabilities` `backends` array and the catalog traits fold
derive from.

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

A third-party library the core needs is a seam of its own, not a port: the core
declares the shape and `main.cpp` supplies the implementation. HKDF arrives that
way (`SessionService::KeyDeriver`, a `std::function` wired to
`net/session_crypto.h`), and so does the controller-audio codec
(`core/audio/audio_codec.h` declares `IAudioDecoder`/`IAudioEncoder` and a
factory; `adapters/audio/opus_codec.*` implements them over libopus). The core
purity gate is what forces this: `src/core` may include no third-party header,
so anything that links one lives outside it. A null factory is a supported
state — the audio paths still validate, gate and rate-limit, they just have
nowhere to put the samples.

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
| `core/ds4_report.h` (DS4 v2 pack/parse/descriptor; shared by two shells) | `platform/macos/mac_hid_gamepad_adapter.cpp` (IOHIDUserDevice), `platform/windows/hidmaestro_adapter.cpp` |
| `platform/windows/hidmaestro_wire.{h,cpp}` (seqlock frames + output ring) | `platform/windows/hidmaestro_adapter.cpp` (mapped sections, doorbells) |
| `platform/windows/hidmaestro_report.h` (per-profile packers + output decode) | `platform/windows/hidmaestro_adapter.cpp` |
| `core/audio/audio_jitter.h` (u16 reorder window, gap/late decisions) | `adapters/audio/opus_codec.cpp` (libopus), `core/session_service.cpp` |

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
};

struct Connection {
    std::string connectionId;    // stable across reconnects
    uint32_t    token = 0;       // rotates on every session PUT
    std::string deviceId;
    std::string deviceName;
    HotSlot*    hot = nullptr;   // the receive thread's view (core/hot_table.h)
    int         slot = -1;       // index of `hot` in the service's hot table
    std::string clientIP;        // the address the PUT came from
    uint8_t     sessionKey[32];  // HKDF(pairingKey, salt, token); senders' copy
    uint8_t     sessionSalt[8];
    uint16_t    epoch = 0;       // bumps on every applied-topology change
    std::chrono::steady_clock::time_point graceUntil; // REST-open liveness grace
    std::array<Controller, 16> controllers;
    bool mouseControlGranted = false;
};
```

What the gamepad path needs per datagram is not in `Connection`: the
replay counter, the last-packet time and the client's address live in
the session's `HotSlot`, next to a published copy of the token, the key
and one word per controller (see "Thread Safety"). The dashboard shows
the slot's address once a datagram has taught it one, and the PUT's
address until then.

### State Ownership

All connection and controller state is owned by `SessionService`: the
cold state behind a mutex, with topology changes serialized by a
second one, and the per-session state the gamepad path reads in a fixed
table of slots that the cold path publishes to the receive thread (see
"Thread Safety"). There are no global connection maps or
serial arrays. The remaining globals are:

- `g_config` / `g_configMtx`: application configuration
- Atomic telemetry counters (`g_packetCount`, `g_submitOk`, etc.)
- `satellite::g_wire` (`app/wire_stats.h`): per-message wire counters, all
  incremented off the gamepad hot path (see "Hot-path discipline")
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
  ├─ n < 28 → drop (too small for header+inner+tag) [g_wire.rxRunt]
  │
  ├─ Extract token (bytes 0-3), counter (bytes 4-7)
  │  └─ svc.getDecryptInfo(token) → not found, or mid-change → drop [g_wire.rxUnknownToken]
  │  └─ counter <= lastCounter → drop (replay) [g_replayDrop]
  │
  ├─ Decrypt (ChaCha20-Poly1305)
  │  └─ Fail → drop [g_decryptFail]
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

Unknown-token drops are the one rejection that also reaches the log.
The maintenance thread feeds the counter and the last sender into
`core/unknown_token_log.h` once a second, which yields one line when a
stretch of such datagrams begins (with the sender's address) and one when
it has been quiet for the reaper's horizon (with how many were dropped).
A dish still sending to a session from before a Satellite restart, or to
one the reaper removed, therefore shows up in the log and not only on the
diagnostics page. A session whose own topology changes lands here too, for
a moment (see "A session during its own change"), so a stretch can open
and close around a PUT.

### Hot-path discipline

The gamepad packet path runs at controller-polling rate, so the receive
loop is kept allocation-free and takes no lock. If you touch
`receiver.cpp`, `core/hot_table.h` or the `SessionService` hot functions,
preserve:

- No heap on the hot path. `recvfrom` writes into a stack buffer, and
  decryption runs in place (`ciphertext == plaintext`, relying on
  libsodium's overlapping src/dst support), so the plaintext reuses the
  ciphertext bytes minus the 16-byte tag.
- No lock for gamepad packets, and no atomic read-modify-write inside
  `SessionService` (the receiver's own telemetry counters are the only
  ones on the path). `getDecryptInfo` and `handleGamepadDataAndUpdate`
  read the session's slot in the hot table, never `connections_`, so a
  gamepad frame does not touch `mtx_` whatever the cold path is doing.
  Per frame that is a
  scan of the packed tokens, two seqlock reads of a few words, two plain
  stores to the receive epoch, two to the slot (counter and time), and
  the submit. On ViGEm, HIDMaestro and uinput the submit takes no
  adapter-wide mutex either (see "Backend locking"); HIDMaestro's takes
  its slot's input flag, one uncontended test-and-set unless the idle
  re-publish is writing that slot. The receive side runs no fence on
  Windows or Linux: the cold side pays for the barrier instead (see
  "Grace"). Where no process barrier is injected (macOS, every test, the
  bench) the receive thread runs one full fence per frame.
- One receive thread. `getDecryptInfo`, `handleGamepadDataAndUpdate`,
  `handleGamepadData`, `updatePostDecryptV4` and `updatePostDecrypt` may
  be called by exactly one thread: the receive epoch and the slot fields
  that thread owns have a single writer. In production that thread is the
  receiver; a test plays it.
- A refused frame is dropped, never retried. A failed seqlock read means
  the session is changing under the frame; the call returns false and
  the receiver counts the drop like any other. Nothing on the frame path
  spins or waits.
- No per-packet string work. The sender's IPv4 address and port are
  packed into one 64-bit word and stored only when they change; the
  dashboard formats the string when it asks, once a second, under the
  cold lock.
- Lock-free loop telemetry. `g_maxLoopUs` uses a thread-local
  high-water-mark so the atomic CAS loop is skipped on the ~99% of
  packets below the running per-second peak. Because that mark never
  resets, a reader that zeroes `g_maxLoopUs` only sees it rise again on a
  new all-time record; `/api/debug` folds each windowed read into
  `g_wire.peakLoopUs` and reports that as the true peak.
- No new counters on the accepted-packet path. The per-message counters in
  `app/wire_stats.h` are incremented only in the receiver's rejection
  branches and in its cold (non-gamepad) dispatch branch, in
  `ClientAdapter::sendEncryptedPacket` (no outbound message is reachable
  from the gamepad path), and in `SessionService`'s audio paths, which run
  at 50 Hz per controller under a lock they already hold.

The conceptual pipeline above lists `getDecryptInfo` /
`updatePostDecrypt` / `handleGamepadData` as distinct steps; on the
gamepad hot path the last two are the single fused call, and every other
message records its datagram through `updatePostDecryptV4` before it is
dispatched. Those other messages (motion, touchpad, battery, mic audio,
the heartbeat) are handled under `mtx_` on the receive thread, which no
topology change holds across backend lifecycle work (see "Topology
changes").

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
   derives the session key via the injected HKDF, returns applied state.
   New pads are plugged first, the rotation and the new topology are
   published together, and retired pads are unplugged last, so the old
   token's pads keep working until the PUT publishes (see "Topology
   changes")
4. Serialize per-controller results + host-feature grants

The session upsert succeeds independently of ViGEm. Per-controller
failures (`backendUnavailable`, `noSlots`, …) ride in the response body;
partial success is normal, not an HTTP error. The one refusal is the
session cap: with every hot-table slot taken, a device without a session
gets 503 `{"error":"tooManySessions"}` (see "Session cap").

### DELETE /api/connections/:id

Client API: authed, scoped to the caller's own session, no close-notify.
Admin API kick: `svc.closeSessionById(..., CLOSE_REASON_KICKED, notify)`
sends the encrypted 0x000F before teardown.

### GET /api/connections (admin)

`svc.getConnectionsSnapshot()` returns a thread-safe copy of all
connections, controllers (with adapter-truth `pluggedIn`), and backend
status, so the caller needs no locking.

## Thread Safety

Two paths reach `SessionService`. The receive thread runs the frame path
described under "Hot-path discipline". Everything else is the cold path:
the HTTP threads (PUT, DELETE, snapshots), the maintenance thread
(rumble tick, reaper), backend callbacks, and the receive thread itself
for every non-gamepad message. The cold path's state lock is a
mutex (`mtx_`), which owns `connections_`, the serial pool and
every controller field; a second one (`topoMtx_`) serializes topology
changes from start to finish. The two paths share state only through the
hot table, and no cold operation holds `mtx_` while Plug and Play runs
(see "Topology changes").

### The hot table

`core/hot_table.h` holds `MAX_SESSIONS` (32) fixed slots and a packed
array of their tokens. A session takes a slot at its first PUT and keeps
it, across re-PUTs, until teardown. The receive thread finds a token by
scanning the packed array (128 bytes, two cache lines) and never touches
a `Connection`. Each `HotSlot` is two parts, each starting on a cache
line of its own, so the stores the receive thread makes every frame
never share a line with the published fields it reads.

Published: written only by the cold path, under `mtx_`, inside a seqlock
write; read by the receive thread.

| Field | Meaning |
| --- | --- |
| `gen` | Seqlock generation: even is stable, odd is being changed. |
| `token` | The session's current token; 0 in a free slot. |
| `keyEpoch` | Bumps on every key or token change (allocation, re-PUT), never on a topology-only change. |
| `key[4]` | The 32-byte session key, as four words. |
| `ctrl[16]` | One word per controller index: bit 31 is active, the low 8 bits the backend serial. |

Receive-owned: written by the receive thread, read by the cold path with
acquire loads.

| Field | Meaning |
| --- | --- |
| `replayKeyEpoch` | The `keyEpoch` the replay state belongs to. Equal to `keyEpoch` once the current key has carried a datagram; until then `getDecryptInfo` reports no counter seen. |
| `lastCounter` | The last accepted counter under that key. |
| `lastPacketTicks` | `steady_clock` time of the last authenticated datagram, for link state and the reaper. |
| `clientAddr` | The last datagram's IPv4 address (network order) shifted left 16 and or'ed with its port. 0 is unknown, and a send to it counts as unroutable. |
| `feedbackPending` | Set by the first datagram under a key; the maintenance tick takes it (see "First datagram"). |
| `txCounter` | The server-to-client nonce counter. Senders increment it under `mtx_`; a new key resets it, so the first send under it goes out as counter 1. |

### Seqlock

A cold write, with `mtx_` held:

```cpp
gen.fetch_add(1, std::memory_order_acq_rel);         // odd: readers refuse the slot
std::atomic_thread_fence(std::memory_order_release);
/* relaxed stores to the published fields */
gen.fetch_add(1, std::memory_order_release);          // even: the new state, whole
```

A read, on the receive thread:

```cpp
g1 = gen.load(std::memory_order_acquire);             // odd: refuse
/* relaxed loads of the fields this call needs */
std::atomic_thread_fence(std::memory_order_acquire);
g2 = gen.load(std::memory_order_relaxed);             // g1 != g2: refuse
/* and the loaded token must be the token looked up */
```

A refused read drops the frame. The reader never retries, so no writer
can starve it and it can never spin. The release fence after the opening
increment is what keeps a reader that loaded a new field from also
loading the old even generation; the increment alone does not order the
stores that follow it on every architecture.

### Grace

A frame can pass its seqlock read just before a cold operation opens a
write, then spend 15 µs in `submitReport` on a serial that operation is
about to unplug. The receive epoch (`RxEpoch`) closes that window. The
receive thread brackets every call that may submit or record a datagram:

```cpp
enter(): epoch = e + 1 (odd, release); compiler barrier; full fence only if no barrier was injected
exit():  compiler barrier; epoch = e + 2 (even, release)
```

and every cold write waits for it straight after making the generation
odd:

```cpp
waitQuiescent(): process barrier (or a full fence); e = epoch (acquire);
                 if e is odd, yield until epoch != e
```

It is Dekker's pattern. The cold side stores the odd generation and then
loads the epoch; the receive side stores the odd epoch and then loads
the generation. With a full barrier between each side's store and load,
at least one sees the other: either the cold side sees the frame in
progress and waits for it to leave, or the frame sees the odd generation
and refuses itself. Waiting for the epoch to change rather than to turn
even means a steady stream of frames cannot starve the wait. The enter
store is a release so that a cold thread that next sees a later frame's
entry still synchronizes with the earlier frame's exit.

The barrier is asymmetric because the two sides run at very different
rates: the receive thread enters thousands of times a second, the cold
side waits a few times a minute. `FlushProcessWriteBuffers` (Windows)
and `membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED)` (Linux, registered at
startup) interrupt every processor running a thread of the process and
drain its store buffer, which makes the receive thread's plain epoch
store as good as fenced at the one moment the cold side needs it, so
the receive side gets by with a compiler barrier. The platform mains pass
the function as the last `SessionService` constructor argument
(`ProcessBarrierFn`, from `platform/windows/process_barrier.h` and
`platform/linux/process_barrier.h`); core includes neither header. Where
none is passed (macOS, a kernel that refuses the membarrier
registration, every test, the bench) the receive thread pays one full
fence per frame instead.

### The rules

- Exactly one thread calls the hot functions (see "Hot-path discipline").
- Cold code never touches a published field outside a seqlock write, and
  every seqlock write starts with the grace wait.
- Cold code never writes a receive-owned field, with one exception and
  two hand-offs. The exception: a new key (allocation or re-PUT) re-seeds
  `clientAddr` to 0, `lastPacketTicks` to the PUT, `feedbackPending` and
  `txCounter`, inside the seqlock write, after the grace wait, where no
  frame can be writing them. That is what makes a send before the new
  key's first datagram unroutable, as the address the old token learned
  is no longer valid, and what keeps a fresh session from inheriting its
  slot's previous liveness. The hand-offs: senders increment `txCounter`
  under `mtx_`, and the maintenance tick takes `feedbackPending` with an
  exchange. (The test seam `backdateForTest` also writes
  `lastPacketTicks`, standing in for a receive thread that went quiet.)
- The receive thread writes a slot only after a successful read and
  inside its bracket, so a datagram that arrives mid-change is never
  recorded into the slot's next owner. `getDecryptInfo` writes nothing
  and needs no bracket.
- Every operation that changes a slot's published fields or a serial's
  owner keeps one order: plug what is new, publish in one seqlock write
  under `mtx_` (open the write, wait for grace, store the new published
  fields, close the write), then unplug what was retired (see "Topology
  changes"). No backend work happens inside a write. A PUT stores the
  session's packed token last, once the write has closed (a re-PUT
  replaces the old token there); a session being torn down has its
  packed token cleared first, before the write opens.

### Topology changes

Seven operations change topology: `upsertSession`, `applyController`,
`removeController`, `closeSessionById`, `closeSessionsForDevice`,
`closeAllSessions` and `reapTimedOut`. Each holds `topoMtx_` from start
to finish, so they run one at a time, and `topoMtx_` is the only lock any
of them holds across `ensureBusOpen`, `closeBus`, `pluginDevice`,
`pluginDevicePreferring` or `unplugDevice`. `mtx_` stays the state lock
and is held only for short sections between those calls: never across a
backend lifecycle call, a thread join, a sleep or a wait, with one
exception, the grace wait inside a publish, which lasts at most one
in-flight frame because a frame never blocks. The lock order is
`topoMtx_`, then `mtx_`; nothing acquires `topoMtx_` while holding
`mtx_`. Nothing else takes `topoMtx_` at all: not the receive thread, not
a backend callback, not a snapshot or a view, not the rumble or the
idle-input tick. The reaper takes it with `try_lock` and reaps nothing on
a tick when another topology change is running, so the maintenance thread
never waits behind a plug; the session goes a second later.

Each operation runs in the same phases, in read-copy-update order: build
the new state where no reader can see it, publish it in one step, wait
until no reader can still be using the old state, and only then reclaim
what it retired. A plug therefore never touches a serial a reader can
reach, and an unplug never touches one a reader might still be using.

1. Plan, under `mtx_`. Validate the request and its owner, dedup the
   descriptors, and decide for every controller index whether its pad is
   kept, updated in place, added, replugged or removed. Serials for adds
   and replugs are allocated here, a first PUT reserves its hot slot (the
   session cap and `tooManySessions` are decided before anything is
   plugged), and a PUT mints its token, salt and key.
2. Plug, under `topoMtx_` only. Open the bus if the plan plugs anything,
   then plug every new serial. No other thread can reach these serials
   yet: the cold state does not name them and the hot table does not
   publish them.
3. Commit and publish, in one hold of `mtx_`. Apply the outcomes to the
   cold state: a failed add releases its serial and reports
   `pluginFailed`, a failed replug releases the new serial, leaves the old
   pad untouched and reports `replugFailed`, the epoch bumps, a PUT sends
   the close-notify to the old token and re-keys the row, and every pad
   that leaves releases its held pointer buttons. Then one seqlock write
   publishes the slot: begin, grace wait, the receive-owned reset if the
   key rotated, the stores, end. From here no thread can reach a serial
   the change retired: the frame path because of the grace wait, the cold
   dispatch because it reads the cold state under `mtx_`.
4. Unplug, under `topoMtx_` only: the removed pads and the old serial of
   every replug.
5. Settle, under `mtx_`. Release each retired serial whose unplug was
   confirmed and quarantine the rest, decide whether the bus is now idle,
   and build the result from the applied state and epoch.
6. Close, under `topoMtx_` only: `closeBus` if the bus is idle, then,
   under `mtx_`, clear the quarantine and log.

Teardown (close, kick, unpair, reap, shutdown) has nothing to plug. Its
commit sends the close-notify while the key and the address still exist,
deactivates the pads, frees the slot through the seqlock (packed token
first) and erases the row; phases 4 to 6 follow. A change that touches
nothing a pad depends on, such as a PUT that only rotates the key, plugs
and unplugs nothing.

Two cases take a second round, both on a full bus. A replug with no free
serial cannot plug before it unplugs, so it retires its pad in phase 3
and plugs the new identity on the serial phase 5 freed: the unplug-first
fallback the converge always had. An add whose serial only a removal in
the same PUT can free waits for that removal the same way. The second
round plugs under `topoMtx_` and commits and publishes again under
`mtx_`. Between the rounds the pad is gone from the cold state and the
hot table alike, and the epoch has already moved, so a heartbeat in that
window reports the gap rather than a pad that is not there.

What a reader sees between the phases: until phase 3, the old state,
whole. The serials phase 1 allocated already count as in use
(`availableSlots`), and a first PUT's session does not exist yet: no
row, no packed token. During phase 2 every existing pad keeps working,
on the old token for a PUT. Phase 3 switches the cold state under `mtx_`
and the slot through the seqlock, so a frame of that session is refused
only between the write's begin and end. After phase 3 the retired
serials are unreachable but stay allocated until phase 5, so none is
reused while its unplug is still in flight. Between phases 5 and 6 an
idle bus is still open.

### A session during its own change

A session's existing pads keep receiving frames through its own plug: the
plug runs before the publish, while the slot still holds the old state.
`applyController` and `removeController` keep the token, so the session's
frames stop only inside the publish window, between the seqlock write's
begin and end, which the grace wait bounds by one in-flight frame. A PUT
rotates the token: frames on the old token are accepted until the PUT
publishes, after its plugs and before its unplugs, and refused from then
on, and frames on the new token are accepted once the dish has the
response and switches. The receiver counts the refusals as unknown-token
drops. Every other session's frames keep flowing throughout.

### First datagram

The first datagram under a new key is the first moment its client can be
reached. The receive thread cannot send from the frame path, so it sets
`feedbackPending` (after the address, so the flag never runs ahead of
it); the maintenance tick, every 100 ms and already under `mtx_`, takes
the flag and resends the pads' light bar, trigger effects and player
LEDs. The resend reaches the client up to 100 ms after its first datagram
rather than with it.

### Session cap

The table is fixed at `MAX_SESSIONS` (32) sessions. A PUT for a device
with no session, while every slot is taken, is refused before anything
is created: `upsertSession` returns `tooManySessions`, and
`PUT /api/connections` answers 503 with `{"error":"tooManySessions"}`. A
re-PUT from a device that has a session keeps its slot.

### The rest of the cold path

Driver lifecycle work never runs under `mtx_`, and no adapter holds a
lock of its own while it runs a callback, but a backend callback still
only tries for `mtx_`, so a notification worker never waits behind the
cold path and an unplug that joins the worker never waits behind `mtx_`
either. Rumble callbacks first publish the latest
motor state and duration into a fixed atomic slot per backend serial, then
try to acquire the service lock. The maintenance tick drains pending state
before refreshing held rumble, so a stop survives contention without
blocking a worker that teardown is joining. A slot whose serial no pad
owns yet keeps its state, so a notification that arrives between a plug
and its commit reaches the pad on the next tick. Serial retirement and
allocation clear the slot before it can serve another controller. Audio
remains a lossy stream and does not use this mailbox.

Mouse-button ownership is per backend serial. The host receives the union of
all controller-held buttons; removing one owner cannot release another owner's
button. Session teardown or replacement, controller removal or replacement,
and leaving mouse mode remove the corresponding contribution. A rejected
injection retains a pending reconciliation for the maintenance tick. On Windows,
only the prefix reported inserted by `SendInput` updates the adapter's button
cache, so a refused release can be retried after the foreground changes.

### Backend locking

The service gives every adapter behind the port four guarantees, and the
adapters rely on them instead of locking against one another's calls:

1. Lifecycle calls are serialized. `ensureBusOpen`, `closeBus`,
   `pluginDevice`, `pluginDevicePreferring` and `unplugDevice` run only
   inside a topology change, under `topoMtx_`, so at most one is in
   flight at a time across every adapter behind the mux. (An adapter's
   destructor calls its own `closeBus` once every thread has stopped.)
2. A serial's submits never overlap its own plug or unplug. No submit for
   a serial is in flight while `pluginDevice` or `unplugDevice` runs for
   it, and none starts after its `unplugDevice` begins until it is
   plugged again: the plug runs before the publish that makes the serial
   reachable, the unplug after the publish and grace wait that made it
   unreachable, and the cold dispatch finds serials through the cold
   state under `mtx_` (see "Topology changes").
3. `closeBus` runs only when no pad is active anywhere, so no submit is
   in flight during it. `ensureBusOpen` may run while other serials are
   live and submitting.
4. Submits have fixed threads. `submitReport`, `submitMotion`,
   `submitTouchpad`, `submitBattery` and `submitMicAudioPcm` come only
   from the receive thread, through the frame path or the cold dispatch.
   `submitRelativeMouse` is host-global and always called under `mtx_`:
   from the receive thread for a touchpad frame, from the maintenance
   tick retrying a refused injection, and from a topology change
   releasing the buttons a departing pad held. `refreshIdleInput` comes
   from the maintenance thread with no service lock held.
   `isDevicePlugged`, `motionBackendOk`, `backendIdForSerial`,
   `isBusOpen` and `supportsMotionForType` may come from any thread.

On top of those, the ViGEm, HIDMaestro and uinput adapters keep four
rules, and the macOS adapter keeps the first and the last:

1. No adapter mutex is held across a blocking call: a driver IOCTL that
   waits (plug, unplug, wait-device-ready, the version check), helper
   provisioning or deprovisioning, a driver-store query, uinput device
   creation or destruction, a thread join, a sleep.
2. A submit for a plugged serial takes no adapter-wide mutex. The
   per-slot fields a submit reads are either immutable while the slot is
   plugged (written before `plugged` is stored with release, changed only
   after `unplugDevice` begins) or owned by the receive thread, the one
   thread that submits. The bus handle is read through an atomic, and
   every field another thread reads (`plugged`, and what `motionBackendOk`
   reads: ViGEm's DS4 flag and EX latch, uinput's motion-node flag) is an
   atomic.
3. HIDMaestro's idle re-publish is the one other writer of a slot's input
   section. It and the submits exclude each other per slot with a
   `std::atomic_flag`: a submit takes it with `test_and_set(acquire)` in a
   spin that yields, since the only possible holder is the idle tick, for
   one pack and write; `refreshIdleInput` tries once and leaves a busy
   slot to the next tick. Nothing else takes the flag, and it is never
   held across a blocking call. The input section's seqlock has a single
   writer by design, and two writers would publish frames that mix two
   reports under an even sequence number.
4. The adapter-wide mutex still guards what it guarded before (the worker
   maps, the callbacks, lifecycle bookkeeping), in short sections only.

`ViGEmAdapter`. `busHandle_` is a `std::atomic<HANDLE>`: `ensureBusOpen`
opens and version-checks the bus without the lock and then stores the
handle, and `closeBus` keeps its order (stop the notification workers,
free the slot events, close the handle). `pluginDevice` reads the handle,
runs the plug and wait-device-ready IOCTLs, prepares the slot (its event,
the DS4 flag, the centred report) and runs the DS4 EX probe submit, all
without the lock, since nothing can reach the slot until `plugged` is
published; it then publishes `plugged` with release and starts the
notification worker under `busMtx_`. `unplugDevice` stops the worker,
whose join drops the lock, clears `plugged`, calls `unplugTarget` without
the lock and clears the slot. The submits and the DS4 report fold take no
lock. A slot's event is freed only in `closeBus` (guarantee 3), so no
in-flight IOCTL uses one after it is freed. `isDevicePlugged`,
`isBusOpen` and `motionBackendOk` read atomics.

`HidMaestroAdapter`. Every provisioner call (`installed`, `isReady`,
`ensureReady`, `provision`, `deprovision`, `shutdown`) runs without
`busMtx_`, and so do the composite plug hooks. The hooks still never
overlap, because plugs never do (guarantee 1), which is what keeps the
default-endpoint guard's window from opening between its check and its
snapshot. A plug adopts the provisioned handles and views, writes the
neutral first frame, publishes `plugged` and starts the slot's workers in
one short hold of `busMtx_`; a refused adoption deprovisions after the
lock is released. An unplug
clears `plugged`, stops the slot's workers (each join drops the lock) and
unmaps the slot under `busMtx_`, then deprovisions without it.
`refreshIdleInput` still holds `busMtx_` while it walks the slots, which
keeps it off a slot an unplug or `closeBus` is unmapping, since both
clear `plugged` under the same lock first. The input-section writers
(`submitReport`, `submitMotion`, `submitTouchpad`, `submitBattery`, all
through `packAndWrite`) take only the slot's flag; `submitMicAudioPcm`
has a single writer and takes neither. `busOpen_` is an atomic.

`GamepadAdapter` (uinput). The devices are a fixed array indexed by
serial, each with an atomic `plugged` and an atomic motion-node flag, in
place of a map that every call had to look up under `mtx_`. Device
creation and destruction and the force-feedback reader's join run
without `mtx_`, and so do the lazy creation and the destruction of the
host-global pointer node. The per-serial submits take no lock;
`submitRelativeMouse` holds `mtx_` only around its writes to the pointer
node, which `closeBus` may be destroying. A gamepad report, the Switch Pro
layout included, goes to the kernel as one `write()` of the whole event
batch, the same events in the same order with `SYN_REPORT` last, where
it used to cost one syscall per event. The reader thread owns its
device's effect table while it runs and takes `mtx_` only to copy the
rumble callback.

`MacHidGamepadAdapter`. Device creation and activation, and the
entitlement probe `ensureBusOpen` may run, happen without `mtx_`, and
cancellation already waited outside it. The submits still run under
`mtx_` (see "Locking" in the macOS backend section).

`GamepadMux` adds no lock: per-serial owners are atomics written at plug
and unplug time. On the ViGEm, HIDMaestro and uinput adapters a submit
therefore takes no mutex between the receive thread and the driver, so a
plug or unplug on one serial no longer delays another serial's submit,
whether it comes from the frame path or the cold dispatch, and no adapter
query made under `mtx_` waits behind a plug.

### Lock laws

Every lock the process shares is a `TrackedMutex` (`core/tracked_mutex.h`)
under a stable name: `service.state`, `service.topology`, `config`, `log`,
`vigem.bus`, `hidmaestro.bus`, `hidmaestro.helper`, `audio.guard`,
`tray.icon`, `update.state`, `pairing.requests`, `pairing.pin`,
`machine.id`, `uinput.devices` and `machid.devices`. A normal build compiles
it to a `std::mutex` and a name pointer, and the thread roles and blocking
markers below to nothing. A build with `SATELLITE_LOCK_STATS` (the lock
tests) records, per lock name and per thread role, acquisitions, contended
acquisitions, failed `try_lock`s, the wait from the `lock` call to the
acquisition and the hold from the acquisition to the `unlock`, with their
maxima and log2 histograms. It also records an edge from every lock a thread
holds to each lock it then calls `lock` on (a `try_lock` cannot wait, so it
records none), and at every `blockingRegion(what)` marker the locks the
thread holds there.

Each long-lived thread names its role at entry with `ScopedThreadRole`:
Receive, Maintenance, BackendWorker (ViGEm notification, HIDMaestro output
and uinput reader threads), AudioWorker (HIDMaestro speaker and haptic
drains), Logger, Update, Background (discovery, mDNS, the address watchers,
the tooltip ticker, the audio guard's poller) and Gui. The HTTP servers' pool
threads take HttpAdmin or HttpClient per request from each server's
pre-routing handler. Topology and ConfigSaver are reserved for threads that
do not exist yet. The markers are `config.save` (every `saveConfig`),
`log.write` (the file log's write and flush), `helper.pipe` (every HIDMaestro
helper pipe wait and launch), `tray.shell` (`Shell_NotifyIconW`),
`audio.com` (the audio guard's COM calls), `backend.lifecycle` (every
lifecycle call the service makes), `thread.join` (the joins in the receiver,
the update service and the adapters) and `vigem.pnp` (the ViGEm bus open,
plug and unplug waits).

`tests/test_lock_laws.cpp` runs the real service wired as in production (the
HKDF deriver, the Opus codec, `audioPolicyFromConfig`, `ClientAdapter`,
`LogAdapter`) with each thread in its production role. The receive thread
drives every message kind through the steps of `receiver.cpp` and
`inner_dispatch.cpp` while HttpClient, HttpAdmin, Maintenance and
BackendWorker threads run upserts that plug and unplug, single-controller
applies and removes, closes, unpairs, reaps, the dashboard JSON builders and
the pairing service's config writes. On Windows a second run puts
`ViGEmAdapter` over a fake driver and `HidMaestroAdapter` over the real
`HelperClient`, which talks to an in-process helper on a real pipe, behind
`GamepadMux`. The config lives in a temporary directory named by
`SATELLITE_CONFIG_DIR`. The test then checks four laws:

1. Receive: the locks a thread with the Receive role takes.
2. Lifecycle: every backend lifecycle call (the mock's, the fake ViGEm
   driver's, the HIDMaestro provisioner's) runs inside a topology change and
   holds nothing but `service.topology`; while the helper's pipe is in use,
   `hidmaestro.helper` as well.
3. Blocking: the locks held at each blocking marker.
4. Order: the nesting edges form an acyclic graph, and every edge is on the
   list of today's order.

Each list is exactly what today's code does. The test fails when a lock, a
pair or an edge appears that is not on its list, and also when one on the
list no longer appears, so a list can only shrink and the change that fixes
an entry deletes it. Every entry says why it is there. Debt is what later
changes remove: the receive thread's `service.state`, `config` and `log`,
the config lock held across its file write, the helper lock held across its
pipe waits, logging under `service.state` and `service.state` taken under
`config`. By design is what stays: anything held under `service.topology`,
which exists to serialize lifecycle work, and the documented order
`service.topology`, then `service.state`.
The test does not reach `tray.shell` or `audio.com`, whose calls act on the
desktop, so those two markers have no entries yet.

Reading the stats: a wait is time a thread spent inside `lock` before it
owned the lock, so a wait on a lock the Receive role takes is time gamepad
frames queue behind; a hold is how long the owner kept it. The histograms
are log2 buckets of nanoseconds, and a percentile is the upper edge of the
bucket that holds it: a p99 of 2,097.2 us means the 99th percentile fell
between 1,048.6 and 2,097.2 us.

### Follow-ups

Not done yet:

- Lock-free submits on macOS: `MacHidGamepadAdapter` still runs every
  submit under its mutex, so a submit there can wait behind another
  serial's submit or a plug's priming report (the backend is inert
  without the HID virtual-device entitlement).
- Coalescing of queued frames: after a stall the receive thread replays
  the backlog frame by frame instead of skipping to each pad's newest
  report.
- A sliding replay window: the replay check is a single high-water mark,
  so a datagram reordered on the network is dropped as a replay.
- Per-session sockets: every session shares one socket and one receive
  thread.

## Windows second backend: HIDMaestro (user-mode UMDF2, plus one kernel transport for audio)

HIDMaestro materializes the identities ViGEmBus cannot — virtual
DualSense (motion + touchpad + lightbar at the real DualSense report
offsets), Switch Pro (motion via the driver's Nintendo protocol
responder), plus DualShock 4 and Xbox 360 as fallbacks — while ViGEm
stays preferred for the types both can serve (kernel submit path,
lowest latency tier).

The input path is user-mode end to end. Controller audio is the
exception and is documented as such below: an audio-carrying persona is
served over HIDMaestro's bundled usbip-win2 kernel USB transport.

### Pure/IO split

- `platform/windows/hidmaestro_wire.{h,cpp}` — the shared-memory
  contract with the driver: input-section seqlock writer (with the GIP
  slice for Xbox profiles and the mandatory ExtendedReportSize clear)
  and the 64-slot output-ring reader. No `<windows.h>`; pinned by
  `tests/test_hidmaestro_wire.cpp` on every CI platform.
- `platform/windows/hidmaestro_report.h` — per-profile packers
  (xbox-360-wired HID+GIP, DS4 via the shared `core/ds4_report.h`
  codec with the Sony calibration-stub IMU rescale, DualSense, Switch
  Pro 0x30 body) and ring-packet decoding back to rumble/lightbar.
  Pinned by `tests/test_hidmaestro_report.cpp`.
- `platform/windows/hidmaestro_adapter.{h,cpp}` — the I/O shell:
  per-serial slots holding mapped views + duplicated handles, the
  zero-allocation submit path (pack → seqlock write → SetEvent), and
  one output-ring worker thread per plugged serial. Tested against a
  fake provisioner (`tests/test_hidmaestro_adapter.cpp`), the same
  fake-driver-link strategy as `test_vigem_adapter`.
- `platform/windows/hidmaestro_audio_wire.{h,cpp}` — the two
  controller-audio rings (see below). Same seqlock/doorbell shape as
  the driver's output ring, same no-`<windows.h>` rule, pinned by
  `tests/test_hidmaestro_audio_wire.cpp` on every CI platform.
- `platform/windows/audio_default_guard.{h,cpp}` — every rule the
  default-playback-device guard follows: the per-role restore decision,
  the parent-chain predicate that tells our endpoint from a real
  DualSense, and the vtable-validation verdict. No `<windows.h>`, pinned
  by `tests/test_audio_default_guard.cpp` on every CI platform.
- `platform/windows/audio_endpoint_com.{h,cpp}` — its I/O shell:
  MMDevice reads, the cfgmgr32 parent walk, the one undocumented
  `IPolicyConfig` write, and the worker that polls the guard after a
  composite plug. Decides nothing; every failure is soft, because a
  guard that cannot run must never fail a controller plug.

### Controller audio, and the one kernel driver

A real DualSense is a composite USB device: a HID gamepad plus a USB
Audio Class function carrying the speaker/headset output and the headset
microphone. HIDMaestro can materialize that composite too
(`dualsense-composite`, `dualshock-4-v2-composite`), which is what makes
the emulated pad present real Windows audio endpoints. `profileForIdentity`
picks the composite id only when the identity has one AND the
`controllerAudio` setting is on; `HidMaestroAdapter` reads that setting per
plug through a callback, so the dashboard toggle applies to the next pad
without a restart, and a refused composite falls back to the plain persona
rather than failing the plug.

The other three settings never touch the persona. A composite always
carries both endpoints: HIDMaestro ships no mic-only audio function, and
all three of its audio profiles (`dualsense-composite`,
`dualshock-4-v2-composite`, `dualsense-edge-composite`) declare both an
`audioStreamingOut` and an `audioStreamingIn` interface. So
`controllerAudioMic` and `controllerAudioSpeaker` gate the WIRE instead,
sampled per frame by `SessionService` through `setAudioPolicy`. That is
why they reach a stream already playing where the master switch cannot:
nothing is replugged, only routed. The sample is taken before `mtx_`,
because the policy itself takes the config lock and the two must never
nest; the callback slot itself is read unlocked, wired once at startup
like `keyDeriver`/`audioCodecs`, since a backend can fire its speaker
callback from inside a call that already holds `mtx_`. Absent keys read
as on, so a config written before the split keeps the behaviour its owner
chose.

`controllerAudioKeepDefaultDevice` answers a different problem. Windows
promotes a newly arrived endpoint to the default playback device: an
endpoint with no persisted `Level` value wins the "newest device" bucket,
and USB bus type plus Speakers form factor both rank top there.
Materializing the pad therefore hands it the whole desktop's audio, which
is what made controller audio look like it forwarded everything. With the
setting on Satellite puts the previous default back afterwards; it never
stops the endpoint existing, because the pad speaker is the point of the
feature.

**This is not user-mode.** A composite persona is served over
HIDMaestro's bundled WHLK-certified usbip-win2 kernel USB transport, which
self-installs on the first composite creation. The helper does that
install explicitly (`HMContext.InstallUsbipBackend`) so a blocked or
declined install is a clean, reportable failure instead of a surprise
inside device creation. Nothing else in Satellite's Windows path loads a
kernel driver, and `controllerAudio` off means it never happens.

PCM does not travel over the helper's JSON pipe (one 20 ms window is ~2 KB,
50 times a second, per direction). Instead a composite plug hands back more
shared sections plus their doorbells:

| Ring | Producer | Consumer | Carries |
|---|---|---|---|
| speaker | helper | satellite | the game's output, channels 1/2 of the pad's OUT stream |
| haptic | helper | satellite | channels 3/4 of that stream, the DualSense's two voice-coil actuators; absent on a persona whose endpoint has no such lanes (DualShock 4 v2) |
| mic | satellite | helper | the client's microphone, on its way to the pad's mic endpoint |

The contract each ring encodes: **the ring speaks the WIRE's channel
layout (stereo out, mono in) at the PERSONA's sample rate.** That split is
deliberate. Channel lane selection is trivially correct and belongs next to
the SDK, in the helper, which splits one OUT frame into the speaker pair and
the haptic pair under one lock so the two rings fill and flush in lockstep.
Rate conversion is not trivially correct,
so it lives in `core/audio/audio_resampler.h` on the satellite side, where
ctest covers it: the DualSense composite runs 48 kHz both ways and needs
none, but the DualShock 4 v2 composite runs 32 kHz out / 16 kHz in, exactly
like the hardware it impersonates. Handing 32 kHz samples to a 48 kHz
consumer would play the stream half again too fast; decimating 48 kHz to
16 kHz without a lowpass would fold everything above 8 kHz into the voice
band.

Both directions are Opus at 48 kHz, 20 ms, VBR, and both are configured in
`adapters/audio/opus_codec.cpp`; satellite itself only ever builds the mic
DECODER and the speaker ENCODER, since the other half of each pair lives
on the client. `OPUS_SET_INBAND_FEC(1)` and `OPUS_SET_PACKET_LOSS_PERC(10)`
are the load-bearing pair: together, not the application, they pick the
mode and force SILK in, so the speaker encodes as Hybrid (SILK under CELT)
instead of CELT alone and BOTH streams really do carry in-band FEC
(measured on libopus 1.6.1: 8.4 dB recovery through `decode_fec` against
-1.3 dB for blind PLC on the speaker stream).
Dropping either one to reach CELT would silently delete that FEC.

Silence is where the two directions diverge. DTX goes on the mic encoder
only: a live microphone never goes digitally silent, so a VAD is the only
thing that can collapse a quiet room (123 of 250 frames gated at -50 dBFS
after speech, 30.0 → 16.4 kbps). The speaker declines it, because that
gate cuts anything ~26-30 dB below the recent peak and would replace a
reverb tail or quiet ambience with comfort noise at -2.3 dB SNR. It uses
`isDigitalSilence` instead, an exact all-zero test that cannot touch
anything audible. Such a window is what Windows renders into the endpoint
whenever nothing is playing to it, and `sendSpeakerFrameLocked` neither
encodes nor sends it, saving ~28 kbps of Opus and ~52 kbps on the wire for
a stream carrying nothing. It deliberately does not advance `seq` either:
a suppressed window is not a hole, and asking the client to conceal one
would have Opus invent noise where the game wrote none.

The DualSense mute button rides `wButtons` bit 0x0800 into input byte 9 bit
0x04, and the game's mute-lamp writes come back out of the existing output
ring (DS5 report 0x02, valid_flag1 bit 0 gating byte 8) as `setMicLedCallback`.

### Elevation story

Satellite stays asInvoker. Device lifecycle needs an elevated token
(SwDevice creation, `Global\` section creation, driver deploy), so it
lives in the bundled .NET helper (`helper/hidmaestro` →
`satellite-hm-helper.exe`). The helper drives the HIDMaestro SDK and
duplicates the per-controller section + event handles into the
satellite process over a named pipe (`hidmaestro_helper_client.cpp`).
Two transports reach it, tried in order on the FIRST HIDMaestro plug
(never from a status probe):

1. The **SatelliteHmBroker** service (`satellite-hm-helper.exe
   service`): LocalSystem, demand-start, registered by setup with the
   hidmaestro component. It listens on `\\.\pipe\satellite-hm-broker`
   with a DACL that admits interactive logons only, and admits a
   connection only when the client PID is the installed `satellite.exe`
   beside it in an interactive session. Satellite starts it if stopped
   (setup grants SERVICE_START to interactive users, and the SCM's
   named-pipe trigger covers the same case), then accepts the pipe only
   if `GetNamedPipeServerProcessId` equals the service's PID from
   `QueryServiceStatusEx`, so a squatted pipe name is refused. One
   client session at a time (the SDK's orphan sweep is machine-global);
   a second is answered `busy`. The service exits after five idle
   minutes and the trigger brings it back. No UAC prompt.
2. A helper spawned `runas` with a private, per-session pipe name (one
   UAC prompt; the connecting client's PID is verified against the
   spawned process). This is the fallback when the service is absent or
   busy.

After that the per-frame hot path is native only. If the helper dies, submits keep working (the
mapped sections outlive it); the next unplug reports unconfirmed and
quarantines the serial, matching the ViGEm zombie-target contract. The
installer's optional-but-default "hidmaestro" component deploys the
UMDF2 driver at setup time (`satellite-hm-helper.exe install-driver`,
elevated there anyway; no reboot). It does NOT deploy the usbip
transport: that one waits for the first composite plug, so a host that
never enables controller audio never acquires it.

### UIAccess for pointer forwarding

The touchpad-as-mouse path injects with `SendInput`, which UIPI confines
to windows of equal or lower integrity. When a game exits and focus
lands on a more privileged window, Windows silently discards the motion
while the session and its counters stay healthy. The signed release
build turns on the CMake option `SATELLITE_UIACCESS` (default OFF),
which sets `uiAccess="true"` in the manifest generated from
`satellite.manifest.in`. Windows grants UIAccess only when the manifest
requests it, the exe is Authenticode-signed and it is installed under
Program Files. A standard user's process then runs at medium+0x10
integrity and an administrator's at high integrity, without admin rights
and without a prompt; neither reaches system-integrity UI. `level` stays
`asInvoker`, so the `HKCU\Run` autostart entry, the ports and the
HIDMaestro broker are unchanged, and unsigned, development and portable
builds keep `uiAccess="false"` and behave exactly as before. At high
integrity the tray window lets Explorer's `TaskbarCreated`, the tray
callback and the second-instance message through the UIPI message
filter. The process logs its UIAccess state at startup, and `/api/debug`
reports it as `uiAccess`.

Routing pointer frames through the LocalSystem broker was evaluated and
not pursued: a session-0 service cannot address the interactive desktop,
so it would have to spawn a helper into the user's session and feed it
every pointer frame over IPC, adding a hop to the hot path and creating
an input path that is not bound to a signed image in a protected
location, which is exactly what UIAccess binds it to.

## macOS backend: virtual DualShock 4 via IOHIDUserDevice

The macOS backend publishes each plugged slot as a kernel HID device
carrying a DualShock 4 v2 identity (VID 0x054C / PID 0x09CC). macOS's
native DS4 support and GameController.framework adopt it like real
hardware, so games get sticks/buttons/triggers plus rumble, lightbar,
touchpad, and motion with no per-game integration.

### Pure/IO split

- `src/core/ds4_report.h` — OS-free: the HID report
  descriptor bytes, input-report packing (reusing the core
  `ds4PackTouchFinger` 12-bit touch codec and the Windows XUSB→DS4
  button/stick mapping byte-for-byte), output-report parsing (rumble
  motors + lightbar RGB, gated by the report's valid flags), and the
  calibration / firmware / pairing feature blobs. The calibration blob
  uses the USB report-0x02 layout (gyro extremes interleaved per axis,
  not the Bluetooth grouped order) and is chosen so consumers' scaling
  math is the identity: wire `MotionReport` values pass through
  unscaled (the wire full-scale convention equals the DS4's raw sensor
  scale). Tested by
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

Unlike the ViGEm, HIDMaestro and uinput adapters, whose submits take no
adapter-wide lock (see "Backend locking"), this adapter still runs its
submits entirely under its mutex: a slot's `IOHIDUserDeviceRef` is only
ever used while the slot is still in the map, so teardown can never
release a ref under an in-flight submit. Lifecycle work follows the same
rule as the other adapters, though, and never holds the mutex across a
blocking call: a plug creates and activates the device without it and
takes it only to prime the pad with one neutral report and add the slot
to the map, and `ensureBusOpen` runs the entitlement probe, itself a
device create, before it takes the mutex. Teardown removes the slot from
the map under the lock, then cancels the device and waits for its cancel
handler outside it (the set-report block takes only the callback hub's
mutex, briefly, to snapshot callbacks); an unconfirmed cancel returns
false so `SessionService` quarantines the serial. Taking the submits off
the mutex is a follow-up (see "Follow-ups"). The full protocol is
documented in `mac_hid_gamepad_adapter.h`.

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
TrackedMutex g_logMtx("log");         // protects all of the above
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

