# Threading and locking

This is for anyone who changes code that runs on more than one thread. It has three parts: [how the design got here](#how-the-design-got-here), release by release; [the rules today](#the-rules-today), which are who owns which state, every lock, the order locks nest in, and the lock laws that hold them; and [how we measure and enforce it](#how-we-measure-and-enforce-it).

The mechanisms are described once, in [`architecture.md`](architecture.md), and each rule below links to the section that details it. The numbers come from [`tools/bench/README.md`](../tools/bench/README.md), which also says how each was measured, including its section [After: this branch](../tools/bench/README.md#after-this-branch).

## How the design got here

Each release took a wait off the path of a datagram, and the next one found where the input path still waited. The steps are in the order they happened.

### 2.1.7: a plug held every dish's input

Plugging a virtual pad waits for Windows Plug and Play, or for the HIDMaestro helper to provision the pad. In 2.1.7 the locks that the input path takes were held across that wait:

- the service mutex, which every gamepad frame took, was held across the plug and, for HIDMaestro, across helper provisioning;
- the same mutex was taken for every heartbeat, motion, touchpad, battery and mic datagram too;
- each backend's bus mutex was held across the plug IOCTLs, and every submit took it.

A dish connecting, adding a pad or going away therefore froze every other dish's controllers for the length of the plug, and they caught up with a burst of stale input afterwards. In the lock path benchmark dish A streams at 8,000 Hz while dish B's PUT plugs a second pad, which takes 200 ms in the bench ([Before: 2.1.7 lock path](../tools/bench/README.md#before-217-lock-path)). In S4 and S5, on both backends, dish A's worst frame took about 202 ms, 1,829 to 1,864 of its frames came in over 1 ms, 12 to 16 were dropped from the ring, and its longest gap was 201.7 to 202.2 ms. The median stayed at 15.4 or 15.5 µs in every row: the cost was waiting behind a lock, not work. The rows are in the table under "This branch".

### 2.1.8: a gamepad frame takes no lock

2.1.8 made the gamepad frame path lock-free in three layers, and the last was needed too: with the service changed and the adapters not, ViGEm mode still stalled 201.5 and 202.0 ms with 1,815 and 1,835 frames over 1 ms, because `ViGEmAdapter` held its bus mutex across the plug IOCTLs and every submit took it.

1. **The frame path.** `core/hot_table.h` holds 32 fixed slots, one per session. Each slot has two halves: a published half, written only by the cold path under `service.state` inside a seqlock write and read by the receive thread; and a receive-owned half, written only by the receive thread (replay counter, last packet time, client address). The receive thread finds a token in a packed array and never touches a `Connection`. A frame can pass its seqlock read just before a cold operation opens a write, then spend its submit on a serial that operation is about to unplug, so the receive thread brackets every call that may submit with an epoch, and every cold write waits for it straight after making the generation odd. The barrier between the two sides is asymmetric: the receive thread pays a compiler barrier, and the cold side pays `FlushProcessWriteBuffers` on Windows or an expedited `membarrier` on Linux. Where no barrier is injected (macOS, the tests, the bench) the receive thread pays a full fence per frame instead. See [The hot table](architecture.md#the-hot-table), [Seqlock](architecture.md#seqlock) and [Grace](architecture.md#grace).
2. **The topology mutex and read-copy-update order.** `service.topology` serializes the seven operations that change topology, from start to finish, and is the only lock held across a backend lifecycle call. `service.state` is held only for short sections between those calls. Each operation builds the new state where no reader can see it, publishes it in one step, waits until no reader can still use the old state, and only then reclaims what it retired:

   | phase | holds | what happens |
   | --- | --- | --- |
   | 1 plan | `service.state` | validate, decide each pad's fate, allocate serials, reserve the hot slot, mint the token and key |
   | 2 plug | `service.topology` | open the bus and plug the new serials; nothing can reach them yet |
   | 3 publish | `service.state` | apply the outcomes to the cold state, then one seqlock write: begin, grace wait, stores, end |
   | 4 unplug | `service.topology` | unplug the retired serials; nothing can reach them since phase 3 |
   | 5 settle | `service.state` | release or quarantine the retired serials, decide whether the bus is idle, build the result |
   | 6 close | `service.topology` | `closeBus` if the bus is idle |

   See [Topology changes](architecture.md#topology-changes).
3. **The backend locking contract.** The service gives every adapter four guarantees: lifecycle calls are serialized, a serial's submits never overlap its own plug or unplug, `closeBus` runs only when no pad is active, and submits have fixed threads. The ViGEm, HIDMaestro and uinput adapters keep four rules in return: no adapter mutex is held across a blocking call, a submit for a plugged serial takes no adapter-wide mutex, HIDMaestro's idle re-publish excludes the submits with a per-slot flag, and the adapter-wide mutex covers only short sections. See [Backend locking](architecture.md#backend-locking).

With 2.1.8's locking no dish A frame is over 1 ms and no ring drops one ([Before: 2.1.8 cold paths](../tools/bench/README.md#before-218-cold-paths); the rows are in the table under "This branch").

On 2026-10-27 the change was also checked on real hardware, with two dishes connected. The MacBook's pad was replugged as a PlayStation pad on ViGEm, then as a DualSense and a Switch Pro on HIDMaestro (about 1.2 s through the helper), then as an Xbox pad again. The Pixel's pad, polled through XInput, showed no pause beyond its own 40 to 120 ms Wi-Fi gaps, and Satellite's worst receive loop was 258 µs. [`xinput_monitor`](../tools/bench/README.md#xinput_monitor) is the tool for repeating that check.

What 2.1.8 left was every other message on the receive thread, and the cold paths that those messages shared locks with. The README measured them: the receive thread took `config` and then `service.state` for every mic datagram (the handler read the audio policy from the config before the service lock and decoded the Opus packet under it), `service.state` for every motion sample and heartbeat, and `log` for any line it logged. With the mic at 1,000 packets a second during dish B's PUT, which held `config` for about 1.6 ms while it wrote the config file, the receive thread waited up to 893.8 µs (mock) and 1,293.2 µs (ViGEm) on `config`, and dish A's worst frame was between 0.5 and 1.7 ms. In S7 the loggers contended for `log` with each other and with a logs reader that held it up to 1.2 ms while it built the JSON, and a logging thread waited up to 1,204.0 µs.

### 2.1.9

2.1.9 is an installer licensing fix and changed no threaded code.

### This branch

This branch removes the remaining waits on the receive thread and the places where a lock was held across something slow. It adds the lock laws that hold the result in place and the release measurements that record it.

- **Messages on the receive thread.** Before, mic audio, motion samples and heartbeats took `service.state`, and mic audio also took `config`. Now the receive thread handles every message with no lock, and each handler reads what the commit published through the hot table inside its grace bracket. The heartbeat ack goes out through the session's send gate, and is left in `txDeferredAck` for the gate's holder when another sender has the gate. Mouse-mode touchpad frames go through a single-producer ring to the **pointer thread**, which owns all pointer state and is the only caller of `submitRelativeMouse`. Mic packets go through another ring to the **mic worker**, which owns the reorder windows and the Opus decoders and brackets its submits in the same grace. The backend's **audio workers** encode and send speaker and haptic audio from lane state they own, and take `service.state` only to mix a haptic window into rumble. See [Threads and what they own](architecture.md#threads-and-what-they-own), [Grace](architecture.md#grace) and [Sending](architecture.md#sending).
- **Configuration.** Before, one `config` mutex guarded the whole config and was held across the file write. Now `ConfigStore` publishes immutable snapshots: a reader copies a pointer under `config.snapshot`, and a writer copies, mutates and publishes under `config.write` without I/O. A mutation that changes nothing publishes and saves nothing. The **saver thread** gives a burst of updates 200 ms to settle, saves outside every lock, and retries a failed save with a backoff from 250 ms to 30 s. `ConfigEffects` applies the two settings with side effects (autostart and crash reporting) one at a time from the latest snapshot, under `config.effects`. The audio policy and the UDP port are atomics. See [Configuration](architecture.md#configuration).
- **Logging.** Before, every line was copied into a 500-line ring under a `log` mutex, which the receive thread took on its rare log paths, the service took while it held `service.state` or `service.topology`, and the logs reader held while it built the JSON; a file logger drained the ring once a second, and lines past 500 between drains were lost. Now `logMsg` builds a fixed 512-byte record and pushes it into a bounded multi-producer queue with one compare-and-swap. One **writer thread** (the Logger role) formats and writes the files and feeds a ring of the last 500 records behind a per-slot seqlock, which the logs JSON reads without a lock. Linux and macOS write log files too. On Windows the crash handlers write straight to the open file with no queue and no lock. See [Logging Infrastructure](architecture.md#logging-infrastructure).
- **The HIDMaestro helper.** Before, the helper client held its lock across pipe waits of up to 120 s, made its first connection inside the first plug, and read each reply one byte at a time. Now each request has its own bound and one deadline for its write and its whole reply: 10 s for hello, unplug and shutdown and for the broker connection, 120 s for a plug and for a launched helper to connect, and 5 s for the helper to exit after its shutdown request. A request that misses its deadline fails and drops the channel. A Background thread connects to the broker service at startup, and again after the dashboard installs the driver, without the helper lock, and the first plug adopts that connection. Replies are read in chunks. The helper lock stays a transaction lock, by design. See [Backend locking](architecture.md#backend-locking), `HelperClient`.
- **The audio default-device guard.** Before, the guard held its lock across COM and audio-service calls, on the plug thread for the snapshot and on its worker for the restore polls. Now every COM call runs on the guard's **worker thread**. The plug thread posts a snapshot request and waits at most 2 s, holding none of the guard's locks, and `audio.guard` guards only the enabled flag, the counters and the note. See [Controller audio, and the one kernel driver](architecture.md#controller-audio-and-the-one-kernel-driver).
- **The tray (Windows).** Before, the tray icon's mutex was held across `Shell_NotifyIconW`, and a ticker thread, the update status callback and the toast path all reached the shell. Now the **Gui thread** owns the icon and makes every shell call, balloons included. A timer on the tray window retitles the tooltip every two seconds, and other threads post `WM_REFRESH` or `WM_BALLOON` to the window. The lock and the ticker thread are gone. See [Lock laws](architecture.md#lock-laws).
- **The dashboard.** Before, each open event stream built its own snapshot every second on its pool thread, taking `service.state` and a config snapshot each time. Now `EventBroadcaster` builds the tick once a second on one **Background thread** that holds no lock, and publishes it as an immutable frame under a generation number. A stream waits for a newer generation and writes the shared frame, so a slow client skips frames and never delays the broadcaster. At most 8 streams run at once; a ninth gets 503. See [GET /api/events (admin)](architecture.md#get-apievents-admin).
- **HTTP idle wakeups (Windows).** Before, httplib's accept loop waited with a 1 ms timeout, so each idle server woke about a thousand times a second. Now `CPPHTTPLIB_IDLE_INTERVAL_USECOND` is 100000 for every Windows target and an idle server wakes about ten times a second; a connection still wakes the accept at once. See [Idle wakeups](architecture.md#idle-wakeups).
- **Rumble.** Before, a rumble callback for a serial that was plugged but not yet committed found no pad and was dropped, so a game that started a motor the moment its pad appeared could leave the client without it. Now the callback stores the game's latest HID motor level for the serial in an atomic first, the commit seeds a newly bound pad from that word, and the level goes out once the publish has closed. See [The rest of the cold path](architecture.md#the-rest-of-the-cold-path).
- **The Linux pointer node.** Before, `submitRelativeMouse` took the adapter mutex around every write, because `closeBus` could be destroying the pointer node beside it. Now the pointer thread creates the node at its first sample and owns it, `closeBus` leaves it alone, and the destructor destroys it once that thread has stopped, so `submitRelativeMouse` takes no lock. See [Backend locking](architecture.md#backend-locking), uinput.

The result, from the same machine and settings as the 2.1.7 and 2.1.8 tables ([After: this branch](../tools/bench/README.md#after-this-branch)). Dish A in S4 and S5, with the 200 ms plug:

| release | backend | scenario | p99.9 µs | max µs | frames over 1 ms | longest gap ms | ring drops |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: |
| 2.1.7 | mock | S4 | 199,121.9 | 201,608.5 | 1,829 | 201.718 | 12 |
| 2.1.7 | mock | S5 | 199,621.3 | 202,069.8 | 1,864 | 202.179 | 16 |
| 2.1.7 | vigem | S4 | 199,185.9 | 201,665.5 | 1,830 | 201.775 | 13 |
| 2.1.7 | vigem | S5 | 199,229.9 | 201,638.8 | 1,842 | 201.748 | 13 |
| 2.1.8 | mock | S4 | 17.3 | 73.7 | 0 | 0.192 | 0 |
| 2.1.8 | mock | S5 | 45.7 | 92.1 | 0 | 0.228 | 0 |
| 2.1.8 | vigem | S4 | 33.5 | 92.8 | 0 | 0.203 | 0 |
| 2.1.8 | vigem | S5 | 46.0 | 90.7 | 0 | 0.200 | 0 |
| this branch | mock | S4 | 18.1 | 31.0 | 0 | 0.141 | 0 |
| this branch | mock | S5 | 28.4 | 32.3 | 0 | 0.144 | 0 |
| this branch | vigem | S4 | 17.9 | 253.6 | 0 | 0.363 | 0 |
| this branch | vigem | S5 | 45.9 | 98.3 | 0 | 0.208 | 0 |

A maximum is one frame of 24,000, so it moves with the machine: ViGEm S4's 253.6 µs has a p99.9 of 17.9 µs beside it. What the branch changes is S6 to S8, where the receive thread no longer shares a lock with anything:

| S6 with `--mic-hz 1000` | 2.1.8 | this branch |
| --- | --- | --- |
| Receive thread's longest wait on a config lock | 893.8 µs (mock), 1,293.2 µs (ViGEm) | none: the receive thread takes no lock |
| Dish A's worst frame per rep | 0.5 to 1.7 ms (six reps) | 32.7 to 168.7 µs (twelve reps) |
| The PUT's `refreshPairedDeviceIdentity` | held `config` for 1,574.6 µs (mock) and 1,649.2 µs (ViGEm) across the file write | returns in 6 to 13 µs and holds `config.write` for 7.0 to 8.0 µs; the saver thread writes the file outside every lock |

In every scenario, on both backends, the Receive role has no row in the lock table. S7 has no `log` rows, because there is no `log` lock any more: the loggers, the logs reader and the writer take no lock at all.

## The rules today

### Threads and what they own

Outside what the cold path changes under `service.state` (its own state and what it publishes) and the senders' gate, every piece of mutable state has exactly one thread that writes it. A thread names its role with `ScopedThreadRole` at entry, and the lock statistics are kept per role. See [Threads and what they own](architecture.md#threads-and-what-they-own) and [Lock laws](architecture.md#lock-laws).

| role | threads | owns (written by it alone) | takes |
| --- | --- | --- | --- |
| Receive | `receiverThread` in `net/receiver.cpp` (a test plays it) | The receive-owned half of each hot-table slot (replay counter, last packet time, client address, first-datagram flag); per serial the `RxPad` (stream caches the dashboard shows, mic rate window); per slot the `RxSession` (mic drop-log flags); the producer end of the pointer and mic rings. It sends the heartbeat ack. | No tracked lock while it handles datagrams. It reads the audio policy and the UDP port as atomics. |
| Pointer | `core/pointer_worker.cpp` | All pointer state: per serial the held buttons, the last touchpad sample and the sub-pixel remainder; per session the refusal run and its log lines; the retry a refused injection leaves. On Linux, the pointer node and its button state. It is the only caller of `submitRelativeMouse`. | No lock |
| Mic | `core/mic_worker.cpp` | Per serial the reorder window and the Opus decoder; per session the missing-endpoint log line. It is the only caller of `submitMicAudioPcm`. | No lock |
| AudioWorker | HIDMaestro's speaker and haptic drains, one per pad and lane | Per serial and lane the outbound lane: encoder, partial window, sequence number. | `service.state`, only to mix a haptic window into rumble; `hidmaestro.bus`, to read its slot and copy callbacks |
| BackendWorker | ViGEm notification threads, HIDMaestro output threads, uinput reader threads | Their device's loop; the uinput reader owns its device's effect table. They deliver rumble, light bar, trigger effects, player LEDs and the mic lamp. | `service.state` with an ordinary lock; their adapter's bus lock, to copy callbacks |
| Maintenance | The tick `receiverThread` starts (`maintenanceLoop`): every 100 ms the rumble refresh and the idle-input re-publish, every second the reaper and the unknown-token log feed | The unknown-token log's state | `service.state`; `service.topology` by `try_lock` only; `hidmaestro.bus` for the idle-input re-publish, and the adapter locks through the reaper's unplugs |
| HttpAdmin | The loopback admin server's pool threads (port 9877), named per request | Nothing outside the cold state | `service.state`, `service.topology` (kick and unpair), `config.snapshot`, `config.write`, `config.effects`, `events.broadcast` (the event streams), `update.state`, `pairing.requests`, `pairing.pin`, and the adapter locks through plug and unplug |
| HttpClient | The client API server's pool threads (HTTPS, port 9443), named per request | Nothing outside the cold state | `service.topology` (PUT, DELETE, controller apply and remove), `service.state`, `config.snapshot`, `config.write`, `hidmaestro.helper`, `pairing.requests`, `pairing.pin`, and the adapter locks through plug and unplug |
| Logger | The log writer thread | The consumer end of the log queue, the recent ring and the log file | No lock |
| ConfigSaver | The config store's saver thread | The persisted generation | `config.write` |
| Update | The update worker and the update timer | Checks, downloads and installs; the updater's state is under `update.state` | `update.state`, `config.snapshot`, `config.write` |
| Background | Discovery beacon, mDNS responder, address-change watchers, the dashboard broadcaster, the audio guard's worker, the helper client's broker-connect thread | Each its own socket or state. The broadcaster owns the build of the tick. The guard's worker owns the default-endpoint snapshot and the cache of which endpoints are ours, and makes every COM call. The connect thread owns its channel until it publishes it through an atomic. | `events.broadcast`; for the dashboard tick `service.state`, `config.snapshot`, `update.state`, `pairing.pin` and `pairing.requests`; `audio.guard` (the guard's worker); `machine.id` (discovery and mDNS) |
| Gui | The main thread: startup, the platform's message or tray loop, shutdown. On Windows it owns the tray icon and makes every `Shell_NotifyIconW` call. | The tray icon's state (Windows) | `config.snapshot`, `pairing.requests`, `update.state`; `service.topology` and `service.state` at shutdown |
| Topology | Reserved; no thread has it | | |
| Other | A thread that names no role: the test harness's main thread, the bench's setup | | |

Law 1 covers the datagram steps. The receive thread's setup and teardown are outside it: `receiverThread` arms `ReceiveWake` (an [untracked mutex](#untracked-mutexes)) around the loop, and when the loop ends at shutdown `stopListening` calls `closeAllSessions`, which is a topology change that takes `service.topology` and `service.state`, and joins the maintenance thread.

The receive thread reaches the pointer thread and the mic worker through single-producer rings (`core/spsc_ring.h`); a full ring drops the frame, as the network would. A topology change and the maintenance tick reach the pointer thread through a bounded multi-producer queue (`core/mpsc_queue.h`) whose posters yield until there is room, so a release or a retry is never lost. A worker sleeps on a counting semaphore that a producer releases only after it sees the worker asleep (`core/worker_wake.h`), so a busy stream costs its producer one load per frame and no system call.

### Locks

Every lock the process shares is a `TrackedMutex` (`core/tracked_mutex.h`) under a stable name. A normal build compiles it to a `std::mutex` and a name pointer; a build with `SATELLITE_LOCK_STATS` records acquisitions, contention, waits, holds, nesting edges and the locks held at each blocking marker, per lock name and thread role. "Held across" names the longest thing the lock is allowed to be held over.

| lock | declared in | protects | held across | taken by |
| --- | --- | --- | --- | --- |
| `service.state` | `SessionService::mtx_` | `connections_`, the serial pool (in use, quarantine, scan cursor) and every controller field: the cold state | Short sections between backend calls. Never a lifecycle call, a join, a sleep or a wait, except the grace wait inside a publish, which lasts at most one in-flight frame, mic submit or audio send. | Maintenance, HttpAdmin, HttpClient, BackendWorker, AudioWorker, Background (the dashboard tick), and the shutdown path. Not Pointer or Mic, and not Receive while it handles datagrams. |
| `service.topology` | `SessionService::topoMtx_` | Nothing by itself: it serializes the seven topology operations (`upsertSession`, `applyController`, `removeController`, `closeSessionById`, `closeSessionsForDevice`, `closeAllSessions`, `reapTimedOut`) from start to finish | Everything slow, by design: `ensureBusOpen`, `closeBus`, `pluginDevice`, `unplugDevice`, Plug and Play waits, helper requests, thread joins | HttpClient, HttpAdmin, Maintenance (`try_lock` only), and the shutdown path. Not a backend callback, a snapshot or a view, or the rumble and idle-input ticks, and not Receive while it handles datagrams. |
| `config.snapshot` | `ConfigStore::snapshotMtx_` | The `shared_ptr<const Config>` | One pointer copy or swap | Any thread that reads the config: HttpAdmin, HttpClient, Background, Gui, Update, and the plug path under `service.topology` on Windows. Not Receive. |
| `config.write` | `ConfigStore::writeMtx_` | Writers' copy-mutate-publish, the generation counters, the flush waiters and the saver's state | A writer's copy, its `mutate` callback (no I/O) and the publish. Never the save: the saver drops it around `saveConfig`. | HttpAdmin (settings, pairing), HttpClient (pairing, identity refresh), Update, ConfigSaver, Gui (startup, flush and stop) |
| `config.effects` | `ConfigEffects::mtx_` | The applied-effects record: which autostart and crash-reporting values were last applied | The autostart registry write and the crash SDK's arm or disarm (markers `autostart.write` and `crash.toggle`), by design, because both can wait on the OS | HttpAdmin (the settings route). Never taken while `service.state` or `service.topology` is held. |
| `events.broadcast` | `EventBroadcaster::Shared::mtx` | The latest frame, its generation, the subscriber count and the close flag | Field updates and condition waits, which release it. The builder runs outside it. | Background (the broadcaster thread), HttpAdmin (the event streams). Nests with nothing. |
| `vigem.bus` | `ViGEmAdapter::busMtx_` | The notification-worker map, the callbacks, lifecycle bookkeeping | Short sections: publishing `plugged` and starting a worker. Not the plug IOCTLs; a worker join drops it. | Whoever holds `service.topology` while plugging or unplugging, and BackendWorker (the ViGEm notification threads), to copy callbacks |
| `hidmaestro.bus` | `HidMaestroAdapter::busMtx_` | The worker maps, the callbacks, slot bookkeeping | Short sections; `refreshIdleInput` holds it while it walks the slots. Provisioner calls and worker joins run without it. | Whoever holds `service.topology` while plugging or unplugging, Maintenance (idle input), BackendWorker and AudioWorker (to copy callbacks) |
| `hidmaestro.helper` | `HelperClient::mtx_` | The helper channel and its request and reply | One request and its reply, pipe waits included: 10 s for hello, unplug and shutdown, 120 s for a plug and for a launched helper's connect. By design: a transaction lock. | Only inside `service.topology`, or by the shutdown path. The broker-connect thread does not take it. |
| `audio.guard` | The audio default guard (`audio_endpoint_com.h`) | Its enabled flag, its counters and its note | Reads and writes of those. Never a COM call. | The guard's worker (Background); `report()` and `active()` may be called from any thread |
| `update.state` | `UpdateService::mtx_` | The updater's state machine: state, info, progress, pending jobs | Short sections; the status callback fires with it released | Update (worker and timer), HttpAdmin (status, check and install requests), Background (the dashboard tick), Gui |
| `pairing.requests` | `net/pairing.cpp` | The pending pair requests | Short sections; the new-request listener runs outside it | HttpClient (submit, poll), HttpAdmin (list, accept, deny), Background (the dashboard tick), Gui (announce) |
| `pairing.pin` | `net/pin_rotation.cpp` | The current and previous PIN, the rotation time, the failure count | PIN compare and rotation | HttpClient (verify), HttpAdmin (the PIN status route), Background (the dashboard tick) |
| `machine.id` | `net/machine_id.cpp` | The cached machine id | The file read or write of the first call, once per process | Background (the discovery beacon and the mDNS responder) |
| `uinput.devices` | Linux `GamepadAdapter::mtx_` | The worker maps, the callbacks, lifecycle bookkeeping | Short sections; device creation and destruction and the reader's join run outside it | Whoever holds `service.topology` while plugging, and the reader threads, to copy the rumble callback |
| `machid.devices` | macOS `MacHidGamepadAdapter::mtx_` | The device map | The submits, in full: the one adapter whose submit still takes its lock (see [Follow-ups](architecture.md#follow-ups)) | Whoever holds `service.topology`, and the receive thread's submits |

The lock-law test runs the Windows adapters (`vigem.bus`, `hidmaestro.bus`, `hidmaestro.helper`) and the mock backend, not `audio.guard`, `update.state`, `uinput.devices` or `machid.devices`; the rows above for those come from reading the code.

Logging takes no lock, and the tray's lock is gone with `tray.icon`.

#### Untracked mutexes

A few small mutexes are plain `std::mutex` and are not in the statistics. Each guards a handshake or a small queue and is held for a moment, and the receive thread takes none per frame.

| mutex | guards | taken by |
| --- | --- | --- |
| `ReceiveWake::mtx_` (`net/receive_wake.cpp`) | The socket the shutdown path pokes to wake the receive loop | The receive thread, to arm and disarm around the loop; the thread that stops the app, to wake it |
| `UpdateService::timerMtx_` | The update timer's interruptible sleep | The update timer and `stop()` |
| `GuardWorker::mtx_` (`audio_guard_worker.h`) | The snapshot request and reply between the plug thread and the guard's worker | The plug thread, which waits at most 2 s on its condition, and the worker |
| `g_incomingMtx`, `g_pairQueueMtx` (`windows/tray.cpp`), `UpdateToastGate::mtx` (`windows/main.cpp`) | Small queues and a once-per-version flag that the Gui thread drains | The Gui thread and the thread that raises the event |
| The callback hub's mutex (`macos/mac_hid_gamepad_adapter.cpp`) | The set-report callbacks the cancel handler snapshots | The macOS adapter |

### Lock order

In the lock-law runs only three locks are ever held while another is taken: `service.topology`, `config.write` and `config.effects`. Every other lock is a leaf.

| outer | inner | where | why |
| --- | --- | --- | --- |
| `service.topology` | `service.state` | all platforms | A topology change takes the state lock for its short sections between lifecycle calls. Nothing acquires `service.topology` while holding `service.state`. |
| `service.topology` | `config.snapshot` | Windows | The HIDMaestro adapter reads the controller-audio setting from the config while it plugs. |
| `service.topology` | `vigem.bus` | Windows | The ViGEm adapter's plug and unplug bookkeeping. |
| `service.topology` | `hidmaestro.bus` | Windows | The HIDMaestro adapter's plug and unplug bookkeeping. |
| `service.topology` | `hidmaestro.helper` | Windows | A helper request. The lock is only ever taken inside a topology change, apart from the shutdown path. |
| `config.write` | `config.snapshot` | all platforms | A writer publishes its copy. |
| `config.effects` | `config.snapshot` | all platforms | The applier reads the latest snapshot inside its lock. |

`config.effects` is never taken while `service.state` or `service.topology` is held. The order law checks all of this: the edges must form a graph with no cycle, and every edge must be on this list. See [Topology changes](architecture.md#topology-changes) and [Lock laws](architecture.md#lock-laws).

### The lock laws and the ratchet

`tests/test_lock_laws.cpp` runs the real service wired as in production, with every thread in its production role, and holds the lock statistics to nine laws:

| # | law | what the test requires | detail |
| --- | --- | --- | --- |
| 1 | Receive | The Receive role acquires no tracked lock (`RECEIVE_MAY_ACQUIRE` is empty) | [Hot-path discipline](architecture.md#hot-path-discipline), [Threads and what they own](architecture.md#threads-and-what-they-own) |
| 2 | Lifecycle | Every backend lifecycle call runs inside a topology change and holds nothing but `service.topology`, plus `hidmaestro.helper` while the helper's pipe is in use | [Topology changes](architecture.md#topology-changes), [Backend locking](architecture.md#backend-locking) |
| 3 | Blocking | The locks held at each blocking marker are on the list | [Lock laws](architecture.md#lock-laws) |
| 4 | Order | The nesting edges form an acyclic graph and every edge is on the list; `hidmaestro.helper` is only taken inside `service.topology`; `config.effects` is taken, and never while `service.state` or `service.topology` is held | [Topology changes](architecture.md#topology-changes), [Lock laws](architecture.md#lock-laws) |
| 5 | Dashboard | The broadcaster built its ticks on a Background thread, which took `service.state` there, and the streams took `events.broadcast` on HttpAdmin threads | [GET /api/events (admin)](architecture.md#get-apievents-admin) |
| 6 | Logger | The writer takes no lock and reaches `log.write`, where it holds nothing | [Logging Infrastructure](architecture.md#logging-infrastructure) |
| 7 | Pointer | The pointer thread takes no lock, injected the mouse, and no other thread did | [Threads and what they own](architecture.md#threads-and-what-they-own), [Backend locking](architecture.md#backend-locking) |
| 8 | Mic | The mic worker takes no lock, submitted the decoded audio, and no other thread did | [Threads and what they own](architecture.md#threads-and-what-they-own), [Grace](architecture.md#grace) |
| 9 | Tray (Windows) | The Gui role made every `Shell_NotifyIconW` call, and no other thread did | [Lock laws](architecture.md#lock-laws) |

Each list is exactly what today's code does. The test fails when a lock, a pair or an edge appears that is not on its list, and it also fails when one on the list no longer appears. So a list can only shrink, and the change that fixes an entry deletes it: this is the ratchet. Every entry carries an `Allowance`. `Debt` marks what a later change will remove, and `ByDesign` marks what stays. No `Debt` entry remains. The entries today, all `ByDesign`:

| list | entries |
| --- | --- |
| Receive may acquire | none |
| Lifecycle may hold | `service.topology` |
| Held while the helper pipe is in use (Windows) | `hidmaestro.helper`, `service.topology` |
| Blocking: marker under lock | `autostart.write` and `crash.toggle` under `config.effects`; `backend.lifecycle` under `service.topology`; on Windows `helper.pipe` under `hidmaestro.helper` and under `service.topology`, `thread.join` under `service.topology`, `vigem.pnp` under `service.topology` |
| Order: may nest | The table in [Lock order](#lock-order) |

The markers with no entry are `config.save`, `log.write` and `tray.shell`, which the test reaches and finds nothing held across, and `audio.com`, which it does not reach because its calls act on the desktop.

To read the statistics a run prints, see "Reading the stats" under [Lock laws](architecture.md#lock-laws).

### When you change threaded code

- **Name the owner.** Every piece of mutable state outside the cold state has one writer. Add the state to the table above. If the receive thread would need something it does not own, hand the work to the thread that owns it through a ring or a queue, and wake that thread with `core/worker_wake.h`.
- **Keep locks off the receive thread.** A handler on that thread reads what the commit published through the hot table's seqlock inside its grace bracket. A refused read drops the frame, and nothing on the frame path retries, spins or waits ([Hot-path discipline](architecture.md#hot-path-discipline)). Do not add a heap allocation, a string build or a new counter that is incremented on the accepted-packet path.
- **Publish through the seqlock.** Cold code writes a published field only inside a seqlock write that starts with the grace wait, and writes a receive-owned field or a worker's state only in the cases the rules name ([The rules](architecture.md#the-rules)). Keep per-pad state outside the cold state tagged with the pad's plug generation, so the owner starts over when it reads a new one.
- **Plug and unplug under `service.topology`, never under `service.state`.** Keep the phase order: plan, plug, publish, unplug, settle, close.
- **Block in no adapter lock.** No adapter mutex is held across a blocking call, and a submit for a plugged serial takes no adapter-wide mutex ([Backend locking](architecture.md#backend-locking)).
- **Bound every wait that sits under a lock.** The helper requests show the pattern: a bound per request, one deadline for the write and the whole reply.
- **A new lock is a `TrackedMutex` with a name** of the form `area.thing`, in a thread that names its role with `ScopedThreadRole`, with every blocking call marked `blockingRegion("what")`. Add its entries to the lists in `tests/test_lock_laws.cpp` with an `Allowance`, and a row to the lock table above. The test tells you what is missing.
- **Run the laws and the bench** (the commands are below) and compare with [Release measurements](../tools/bench/README.md#release-measurements): the plug stall gate fails on a stall.

## How we measure and enforce it

`bench_frame_path` sends real encrypted datagrams through `SessionService` the way the receiver thread does, from one spin-paced producer per dish into a 1,600-entry ring (the 64 KiB socket buffer), and times each gamepad frame from arrival to submit. Its scenarios build on one another as the table says:

| scenario | what runs |
| --- | --- |
| S1 | Dish A alone at 1,000 Hz; no cold threads |
| S2 | Dish A at `--rate-a` (8,000 Hz) and dish B at `--rate-b` (75 Hz); no cold threads |
| S3 | S2 plus the sse thread (1 Hz) and the maintenance thread (10 Hz, reaping on every 10th tick) |
| S4 | S3 plus a PUT for dish B at 1.0 s that plugs a second pad: the 200 ms plug |
| S5 | S4 plus dish A as a PlayStation pad streaming `MSG_MOTION` at `--motion-hz` (500 Hz), and a heartbeat from each dish every 2 s |
| S6 | S4 plus dish A as a PlayStation pad streaming `MSG_MIC_AUDIO` at `--mic-hz` (50 Hz), and dish B's PUT running `refreshPairedDeviceIdentity` with a new name, which saves the config |
| S7 | S5 plus four threads logging 2,000 lines a second each and a reader building the logs JSON every 100 ms |
| S8 | S5 plus eight dashboard readers, each building the connections and devices JSON every 100 ms |

| tool | what it tells you | command |
| --- | --- | --- |
| Lock-law test, `tests/test_lock_laws.cpp` | Structure, never time: which roles take which locks, what is held across blocking calls, how locks nest, and that the workers did their jobs. It fails on a lock, pair or edge that is not on a list, and on one that no longer appears. | `cmake --build <build dir> --target test_lock_laws`, then `ctest --test-dir <build dir> -R '^lock_laws$' --output-on-failure`. The measurements workflow runs `ctest --preset windows-msvc -R '^lock_laws$'`, and every CI lane runs it in its full ctest pass. The binary lands at the repo root: run `./test_lock_laws` there to print the statistics table, the nesting edges, the blocking regions and the regions reached. |
| `bench_frame_path` | Time on the frame path, in loopback, with a simulated submit and a simulated plug, on a mock backend and on the real `ViGEmAdapter` over a fake driver. Windows only. Compare runs of the same binary on the same machine: the process keeps the default priority class and timer resolution, which the application raises. | `cmake --build <build dir> --target bench_frame_path`, then `./bench_frame_path.exe --backend mock --json out.json` and `./bench_frame_path.exe --backend vigem --json out.json`. `--mic-hz 1000` adds the 1 kHz mic case, and `--help` lists the rest. |
| `bench_frame_path_lockstats` | The same bench built with `SATELLITE_LOCK_STATS`: for the reported rep of every scenario, each lock and role with acquisitions, contended acquisitions, failed `try_lock`s, maximum wait, p99 wait and maximum hold. The instrumentation costs a few clock reads per lock, so latency comes from `bench_frame_path` and lock figures from this one. | `cmake --build <build dir> --target bench_frame_path_lockstats`, then `./bench_frame_path_lockstats.exe --backend vigem --json out.json` |
| Smoke runs of both benches | The harness's own accounting, never a latency: every scenario for 0.2 s on both backends. | `ctest --test-dir <build dir> -R frame_path_bench --output-on-failure`, or `./bench_frame_path.exe --smoke` |
| `tools/bench/dish_bench` | The real chain end to end: it impersonates a paired dish against a running Satellite (a protocol-1 session over HTTPS 9443, `MSG_GAMEPAD_DATA` to UDP 9876) and reads the result back through XInput on one clock. It needs Satellite running, ViGEmBus installed and one paired device, and its session replaces that device's live one. | `bash tools/bench/build.sh`, then `./dish_bench.exe selftest` and `./dish_bench.exe run --rate 1000 --secs 10` |
| `tools/bench/xinput_monitor` | What a game would see on a real pad: every slot connect and disconnect, and every pause of at least `--gap-ms` between two changes of a slot's packet number. Keep a stick moving, because an idle pad is a pause too. The bookkeeping is in `tools/bench/pause_detector.h`, which `tests/test_pause_detector.cpp` runs against synthetic timelines. | `cmake --build <build dir> --target xinput_monitor`, then `./xinput_monitor.exe --secs 60 --gap-ms 40` |
| `scripts/smoke-windows-app.ps1` | That the built app starts, owns UDP 9876 and TCP 9877, counts three unauthenticated datagrams, stays up past the updater's settle time, closes on `WM_CLOSE` with exit code 0, and logs no `ERROR` line. Exit codes: 0 passed, 1 failed, 2 refused because a Satellite is already running. `windows-msvc-ci.yml` runs it after the build. | `pwsh scripts/smoke-windows-app.ps1 -Exe satellite.exe -StaySecs 45` |
| Release measurements | A record per release, a gate on plug stalls and a trend against the previous release. See below. | `gh workflow run measurements.yml --ref <branch> -f version=<label>`, or by hand as below |

### The per-release measurements

`.github/workflows/measurements.yml` turns the lock path benchmark into a record per release. It runs on pull requests that touch `src/**`, `tools/bench/**`, `tests/test_lock_laws.cpp` or `CMakeLists.txt`, by hand (`workflow_dispatch`), and from `release.yml` as the `measurements` job between `draft` and `publish`. One `windows-latest` job builds `bench_frame_path`, `bench_frame_path_lockstats` and `test_lock_laws` (MSVC, Release), runs the lock laws, and runs each bench target at its defaults with `--backend mock` and with `--backend vigem`, and once more per backend with `--mic-hz 1000`, of which only S6 is kept. `scripts/measurements-report.ps1` merges the eight JSON files and renders the report.

- **Plug stall gate (fails the job).** In S4 to S8, dish A's longest gap in the reported (median) rep must stay under 50 ms, in every run: both bench targets, both backends, and S6 at `--mic-hz 1000`. A real stall lasts the whole 200 ms plug and a runner hiccup does not. A missing run, or a missing dish A row, fails too, because it cannot be judged. `publish` needs `measurements`, so a release that fails the gate stays a draft, and "Re-run failed jobs" repeats only the measurement.
- **Trend (warnings only).** The report downloads `measurements-*.json` from the latest published release and warns, in the markdown and as `::warning::` annotations, when a p99.9 or a max is more than twice the previous value and more than 100 µs higher. Shared runners are noisy, so this never fails a run.
- **Where the files land.** `measurements-<version>.md` and `measurements-<version>.json`. The markdown is the job summary, both files are the `measurements` workflow artifact (kept 90 days), and a release run attaches them to the draft release with `gh release upload --clobber`. The next release's run reads the JSON back from there.

To produce the same files by hand, run each bench target once per backend, and once more per backend with `--mic-hz 1000`, each with `--json` into one directory (`raw`), then:

```powershell
gh release download --pattern 'measurements-*.json' --dir previous
pwsh scripts/measurements-report.ps1 -InputDir raw -OutputDir out -Version dev -PreviousDir previous
```

The report exits 1 when the gate fails. `-PreviousDir` is optional, `-AllowPartial` accepts a directory that lacks some of the eight runs, and `-GapLimitMs`, `-TrendRatio` and `-TrendMinDeltaUs` move the thresholds. `pwsh tests/test_measurements_report.ps1` tests the script, and also runs the built benches with `--smoke` to check the JSON they really write.
