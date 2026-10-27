# dish latency benchmark

End-to-end input-latency measurement for the dish -> satellite path, decomposed
into every stage from a physical controller to the game on the PC. Built to answer
the question "how does this compare to Apollo/Artemis remote input + cakama3a/Polling
(<3 ms)?" with numbers instead of adjectives.

## The chain (controller button -> game sees it)

| # | Stage | What it is | Measured by | Clock |
|---|-------|-----------|-------------|-------|
| 0 | Controller + USB poll | controller scan/debounce + USB interrupt interval (a 1 kHz pad = 1 ms floor) | on-device URB inter-arrival (`getDeviceUrbCount`) | device |
| 1 | Android hot path | URB reap -> parse -> dedupe -> ChaCha20-Poly1305 encrypt -> `sendto` | on-device instrumentation (`hotpath_latency`) | device |
| 2 | Network (one way) | Wi-Fi/USB-tether transit dish -> satellite | ½ × heartbeat RTT (ping-send to ack-recv, one clock) | device |
| 3 | Satellite processing | `recvfrom` -> decrypt -> parse -> ViGEm submit returns | telemetry `lastLoopUs` (`GET /api/debug`) | PC |
| 4 | Driver -> game-visible | ViGEm submit -> state visible to XInput | `dish_bench` XInput reader (derived: e2e − stage 3) | PC |
| — | **Poll cadence** | update interval the PC actually sees — **the cakama3a/Polling metric** | `dish_bench` XInput inter-update interval | PC |

Stages 3, 4 and the poll cadence are all measured on **one PC on one clock**, so no
cross-device clock sync is needed. Stages 0-2 are measured on the phone. Add the two
halves for the glass-to-glass budget.

## Why this is honest

- **cakama3a/Polling measures cadence, not latency.** "<3 ms" with Apollo/Artemis is
  the interval between distinct updates the PC sees, i.e. effective poll rate. We
  reproduce *that exact metric* (stage "Poll cadence") and also break out true added
  latency, which cakama3a cannot see.
- **No fabricated end-to-end clock.** The protocol carries no cross-device timestamp,
  so we never pretend to sync two clocks. Each stage is timed on a single clock and
  the network leg uses a round trip (RTT/2) measured entirely on the device clock.
- **The crypto is the real thing.** `dish_bench` links satellite's own
  `session_crypto.cpp`; `dish_bench selftest` reproduces the pinned interop vector
  byte for byte, so the packets it sends are indistinguishable from a real dish.

---

## Part A — PC-side (satellite receive path). Runnable on the PC alone.

`dish_bench` impersonates a paired dish: opens a real protocol-1 session
(`PUT /api/connections` over HTTPS:9443), streams `MSG_GAMEPAD_DATA` to UDP:9876 with
a 16-bit sequence counter embedded in the right-stick X axis, and a co-located XInput
reader decodes that axis on every change. Same QPC clock on both ends -> the whole
receive chain (sendto -> recv -> decrypt -> ViGEm -> driver -> XInput-visible) is
timed directly. Running it on the satellite PC means the network leg is loopback (~0),
which isolates the software + driver cost.

### Build

```sh
bash tools/bench/build.sh        # ucrt64 g++; links session_crypto.cpp + libsodium
```

### Run

```sh
./dish_bench.exe selftest                       # validate crypto vs pinned vector
./dish_bench.exe run --rate 1000 --secs 10      # uses first paired device in config.json
./dish_bench.exe run --rate 2000 --secs 5 --csv out.csv
```

Options: `--host <ip>` (satellite, default 127.0.0.1), `--admin-host <ip>`,
`--device-id <s>`, `--key <hex64>` (else read from `%APPDATA%\satellite\config.json`),
`--rate <hz>`, `--secs <n>`, `--csv <path>`.

Requirements: satellite running (it auto-binds 9876/9443/9877), ViGEmBus installed,
and one paired device in `config.json`. The run does **not** rename the paired device
(it omits `deviceName`); it only updates `lastIP` to 127.0.0.1. Keep that device's Dish
disconnected while it runs: the bench's session replaces the device's live one (the
satellite sends it `SESSION_CLOSE` with reason `replaced`) and declares a single Xbox 360
pad.

### Measured results (this machine, loopback, satellite HIGH_PRIORITY)

ucrt64 build, ViGEmBus running, Xbox360 virtual pad on XInput slot 0:

| Send rate | e2e sendto→visible p50 / p99 | poll interval p50 (eff. rate) | stage3 recv→submit p50 | submitOk / decryptFail |
|-----------|------------------------------|-------------------------------|------------------------|------------------------|
| 1000 Hz   | 0.021 / 0.035 ms             | 1.000 ms (1000 Hz)            | 0.017 ms               | all / 0                |
| 2000 Hz   | 0.018 / 0.033 ms             | 0.499 ms (2002 Hz)            | 0.017 ms               | all / 0                |
| 4000 Hz   | 0.017 / 0.033 ms             | 0.250 ms (4005 Hz)            | 0.019 ms               | all / 0                |
| 8000 Hz   | 0.016 / 0.030 ms             | 0.125 ms (8019 Hz)            | 0.013 ms               | all / 0                |

**Reading:** the satellite receive path adds ~0.02 ms (median) and never throttles the
cadence — at 8 kHz the PC still sees ~8019 distinct updates/s. So the PC sees whatever
rate the source controller delivers, plus ~20 µs. The poll-interval floor is the
*source* rate, not satellite.

Caveats: this is loopback (network ≈ 0; the real LAN leg is stage 2, measured on the
device). `stage3` is sparse (telemetry `lastLoopUs` polled at 5 Hz) so it is indicative,
not a full distribution; `e2e` is the robust per-packet number. The XInput reader spins,
so stage-4 is best-case observation latency — a real game adds at most its own frame
interval, which is identical for any input method.

---

## Part B — device-side (stages 0-2). Run on the phone.

See `dish-android` `tools/bench/` and the `hotpath_latency` instrumentation. In short:

- **Stage 0 (poll interval):** already exposed — `PollRateSampler` / `getDeviceUrbCount`
  give the USB URB rate per controller. A 1 kHz pad reads ~1000 Hz.
- **Stage 1 (Android hot path):** the instrumentation captures `CLOCK_MONOTONIC` at URB
  reap and just before `sendto`, accumulating per-controller percentiles, surfaced via
  the JNI `InstrumentationNative.hotPathBenchJson(reset)` (armed by `setHotPathBench(true)`)
  and the debug readout.
- **Stage 2 (network one way):** the heartbeat path timestamps each ping send and its
  ack receipt on the device clock; one-way ≈ RTT/2.

### Results template (fill in from an on-device run)

```
stage 0  USB poll interval         : ____ ms   (= ____ Hz controller)
stage 1  Android hot path p50/p99  : ____ / ____ ms
stage 2  network one-way (RTT/2)   : ____ ms   (Wi-Fi)  /  ____ ms (USB tether)
```

---

## Putting it together (glass-to-glass)

```
total ≈ stage0(sampling) + stage1(android) + stage2(network) + stage3(satellite) + stage4(driver)
```

PC-measured stages 3+4 are ~0.02 ms combined. The dominant terms are the source poll
interval (stage 0, intrinsic to the controller — same for everyone) and the network
leg (stage 2). The relay's own software overhead is in the tens of microseconds.

---

## Lock path benchmark

`bench_frame_path` (Windows only; `tools/bench/frame_path_bench.cpp`) sends real encrypted datagrams through `SessionService` the way the receiver thread does and times each `MSG_GAMEPAD_DATA` frame from arrival to submit. One spin-paced producer per dish seals each datagram with the key derived from what `upsertSession` returned, stamps it with `steady_clock` and pushes it into a 1,600-entry ring (the 64 KiB socket buffer); a full ring counts a drop. One receiver thread, `TIME_CRITICAL` and pinned to CPU 2 by default (the installed Satellite pins its own to CPU 0), takes the oldest frame across the rings and runs the receiver's steps in order: `getDecryptInfo`, the replay check, in-place `decryptPacket`, `parseInnerHeader`, then `handleGamepadDataAndUpdate` for gamepad data or `updatePostDecryptV4` and `dispatchInnerMessage` for any other message. Latency, recorded for gamepad frames only, is the clock read after the last of those calls returns minus the stamp.

The default backend is a mock: `submitReport` and `submitMotion` spin `--submit-us` (the synchronous ViGEm call), `pluginDevice` sleeps `--plug-ms` (the Plug and Play wait, armed once the sessions are open) and `unplugDevice` sleeps 5 ms. `--backend vigem` swaps it for the real `ViGEmAdapter` behind `GamepadMux`, linked against a fake driver layer (`tools/bench/fake_vigem_driver.cpp`) whose plug, unplug and submit calls take those same times, so the adapter's own locking is part of what is timed. S1 is dish A alone at 1 kHz. S2 is dish A at `--rate-a` plus dish B at `--rate-b`; neither runs a cold thread. S3 adds an `sse` thread (1 Hz: `getConnectionsSnapshot`, `linkStateForDevice` per dish, `activeSessionCount`) and a `maintenance` thread (10 Hz: `refreshRumble`, `refreshIdleInput`, `reapTimedOut` on every 10th tick). S4 adds a PUT for dish B at 1.0 s with a second controller, which plugs a second pad; B's producer then switches to the new token and key, and any frames still queued under the old token count as `refused`. S5 is S4 plus the traffic that reaches the service through `dispatchInnerMessage`: dish A is declared as a PlayStation pad with `CAP_MOTION` and streams `MSG_MOTION` at `--motion-hz` (default 500) from its own producer and ring, and each dish sends a `MSG_HEARTBEAT_PING` every 2 s, A first at 0.3 s and B at 1.3 s. A dish's message types share one counter, so the receiver rescans until two scans agree on the oldest ring and never serves a motion frame after a later gamepad frame of the same dish, which the replay check would reject. Each scenario runs `--secs` seconds (default 3), `--reps` times (default 3) interleaved, after a discarded warm-up pass; the table shows the rep whose dish A p99.9 is the median, and it and the CSV carry a `backend` column (`mock` or `vigem`).

S6 to S8 add the cold paths that still take a lock the receive thread also takes. S6 is S4 with dish A declared as a PlayStation pad with `CAP_MIC`, streaming `MSG_MIC_AUDIO` at `--mic-hz` (default 50) from its own producer and ring, with real Opus packets encoded once at startup; after its upsert, dish B's PUT runs the production `refreshPairedDeviceIdentity`, which saves the config, as `PUT /api/connections` does before it replies. S7 is S5 plus four threads each logging 2,000 lines a second through the production `logMsg` and a reader calling the logs JSON builder (`src/net/admin_json.h`, the body of `/api/logs`) every 100 ms. S8 is S5 plus eight dashboard readers, staggered across the period, each calling the connections and devices JSON builders every 100 ms. Every scenario wires the service as production does: the HKDF deriver, the Opus codec, the production `audioPolicyFromConfig`, and a log port that writes into the production log ring (`--verbose` still prints each line to stderr). S1 to S5 send nothing that reaches the codec or the audio policy.

S6 saves the config for real, so the bench points `configPath()` somewhere harmless before it runs anything: unless `SATELLITE_CONFIG_DIR` is already set, it creates `%TEMP%\satellite-bench-<pid>`, sets the variable to it and removes the directory on exit, and the banner prints the path in use. `configPath()` honours `SATELLITE_CONFIG_DIR` on Windows, Linux and macOS: when it names a directory, the config file, the TLS certificate and the machine id go there instead of the per-user default.

Build it with `cmake --build <build dir> --target bench_frame_path`; the binary lands at the repo root, `--csv <path>` also writes the rows and `--json <path>` writes them, with the settings and the machine, as JSON. `bench_frame_path_lockstats` is the same bench compiled with `SATELLITE_LOCK_STATS` (see "Lock laws" in `docs/architecture.md`): after the table it prints, for the reported rep of every scenario, each lock and thread role that touched a lock, with its acquisitions, contended acquisitions, failed `try_lock`s, maximum wait, p99 wait from the log2 histogram and maximum hold, and its `--json` carries those rows too. The instrumentation costs a few clock reads per lock, which is why the latency figures come from `bench_frame_path` and only the lock figures from the stats build. `bench_frame_path --smoke` runs every scenario for 0.2 s on both backends with the cold-thread schedule compressed to fit, and ctest runs it as `frame_path_bench_smoke` and the stats build, with `--json`, as `frame_path_bench_lockstats_smoke`; the exit code reflects the harness's own accounting (and usage errors), never a latency, and `--help` lists the options. The process keeps the default priority class and timer resolution, which the application raises, so compare runs of the same binary on the same machine.

### Before: 2.1.7 lock path

`--submit-us 15`, `--plug-ms 200`, everything else at its default (3 s per run, 3 reps, dish A 8000 Hz, dish B 75 Hz, motion 500 Hz, receiver on CPU 2), one invocation per backend; AMD Ryzen 9 7900X 12-Core Processor, Release build. Latencies are in microseconds; `over 1 ms` counts frames slower than 1 ms; `longest gap` is the longest wait between two completed frames of that dish, in ms; `drops` counts ring overflows; `refused` counts frames whose token the service no longer knew.

| backend | scenario | dish | rate Hz | frames | p50 us | p99 us | p99.9 us | max us | over 1 ms | longest gap ms | drops | refused |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| mock | S1 | A | 1000 | 3000 | 15.4 | 16.0 | 18.3 | 52.1 | 0 | 1.037 | 0 | 0 |
| mock | S2 | A | 8000 | 24000 | 15.4 | 15.9 | 18.3 | 48.3 | 0 | 0.213 | 0 | 0 |
| mock | S2 | B | 75 | 225 | 15.5 | 31.0 | 36.0 | 36.0 | 0 | 13.349 | 0 | 0 |
| mock | S3 | A | 8000 | 24000 | 15.4 | 15.9 | 18.1 | 54.7 | 0 | 0.182 | 0 | 0 |
| mock | S3 | B | 75 | 225 | 15.5 | 30.9 | 32.8 | 32.8 | 0 | 13.351 | 0 | 0 |
| mock | S4 | A | 8000 | 23988 | 15.4 | 175711.2 | 199121.9 | 201608.5 | 1829 | 201.718 | 12 | 0 |
| mock | S4 | B | 75 | 210 | 15.5 | 1333.2 | 24691.9 | 24691.9 | 3 | 237.995 | 0 | 15 |
| mock | S5 | A | 8000 | 23984 | 15.4 | 176571.4 | 199621.3 | 202069.8 | 1864 | 202.179 | 16 | 0 |
| mock | S5 | B | 75 | 210 | 15.5 | 5075.0 | 27825.1 | 27825.1 | 3 | 241.128 | 0 | 15 |
| vigem | S1 | A | 1000 | 3000 | 15.4 | 16.1 | 17.9 | 19.2 | 0 | 1.006 | 0 | 0 |
| vigem | S2 | A | 8000 | 24000 | 15.4 | 16.0 | 17.7 | 81.2 | 0 | 0.191 | 0 | 0 |
| vigem | S2 | B | 75 | 225 | 15.5 | 31.1 | 31.1 | 31.1 | 0 | 13.349 | 0 | 0 |
| vigem | S3 | A | 8000 | 24000 | 15.4 | 16.0 | 17.8 | 74.1 | 0 | 0.185 | 0 | 0 |
| vigem | S3 | B | 75 | 225 | 15.5 | 30.8 | 30.8 | 30.8 | 0 | 13.349 | 0 | 0 |
| vigem | S4 | A | 8000 | 23987 | 15.4 | 175888.0 | 199185.9 | 201665.5 | 1830 | 201.775 | 13 | 0 |
| vigem | S4 | B | 75 | 210 | 15.5 | 337.6 | 23693.5 | 23693.5 | 2 | 236.997 | 0 | 15 |
| vigem | S5 | A | 8000 | 23987 | 15.5 | 176477.7 | 199229.9 | 201638.8 | 1842 | 201.748 | 13 | 0 |
| vigem | S5 | B | 75 | 210 | 15.5 | 2516.4 | 25648.1 | 25648.1 | 3 | 238.951 | 0 | 15 |

### After: this branch

The same settings on the same machine, one invocation per backend, this branch's Release build.

| backend | scenario | dish | rate Hz | frames | p50 us | p99 us | p99.9 us | max us | over 1 ms | longest gap ms | drops | refused |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| mock | S1 | A | 1000 | 3000 | 15.4 | 16.0 | 17.9 | 73.8 | 0 | 1.059 | 0 | 0 |
| mock | S2 | A | 8000 | 24000 | 15.4 | 15.8 | 17.7 | 86.8 | 0 | 0.196 | 0 | 0 |
| mock | S2 | B | 75 | 225 | 15.5 | 30.7 | 30.8 | 30.8 | 0 | 13.349 | 0 | 0 |
| mock | S3 | A | 8000 | 24000 | 15.4 | 15.8 | 17.7 | 86.4 | 0 | 0.196 | 0 | 0 |
| mock | S3 | B | 75 | 225 | 15.5 | 30.8 | 32.7 | 32.7 | 0 | 13.351 | 0 | 0 |
| mock | S4 | A | 8000 | 24000 | 15.4 | 15.9 | 17.5 | 65.7 | 0 | 0.197 | 0 | 0 |
| mock | S4 | B | 75 | 225 | 15.5 | 31.0 | 42.5 | 42.5 | 0 | 13.349 | 0 | 0 |
| mock | S5 | A | 8000 | 24000 | 15.4 | 15.9 | 30.5 | 85.1 | 0 | 0.199 | 0 | 0 |
| mock | S5 | B | 75 | 225 | 15.5 | 45.9 | 45.9 | 45.9 | 0 | 13.364 | 0 | 0 |
| vigem | S1 | A | 1000 | 3000 | 15.4 | 16.0 | 17.5 | 89.5 | 0 | 1.085 | 0 | 0 |
| vigem | S2 | A | 8000 | 24000 | 15.4 | 15.9 | 17.7 | 92.9 | 0 | 0.203 | 0 | 0 |
| vigem | S2 | B | 75 | 225 | 15.5 | 31.3 | 31.9 | 31.9 | 0 | 13.349 | 0 | 0 |
| vigem | S3 | A | 8000 | 24000 | 15.4 | 15.8 | 18.1 | 89.4 | 0 | 0.199 | 0 | 0 |
| vigem | S3 | B | 75 | 225 | 15.5 | 30.9 | 33.2 | 33.2 | 0 | 13.349 | 0 | 0 |
| vigem | S4 | A | 8000 | 24000 | 15.4 | 15.9 | 17.7 | 88.5 | 0 | 0.198 | 0 | 0 |
| vigem | S4 | B | 75 | 225 | 15.5 | 30.9 | 32.0 | 32.0 | 0 | 13.349 | 0 | 0 |
| vigem | S5 | A | 8000 | 24000 | 15.4 | 30.6 | 32.9 | 85.9 | 0 | 0.196 | 0 | 0 |
| vigem | S5 | B | 75 | 225 | 15.4 | 46.1 | 46.3 | 46.3 | 0 | 13.364 | 0 | 0 |

In 2.1.7 both backends stall dish A for the whole of dish B's 200 ms plug in S4 and S5: the service held its mutex across the plug, so the receive thread waited on it and 1,829 to 1,864 frames came in over 1 ms, with 12 to 16 ring drops and a gap of about 202 ms. On this branch before the adapters changed, the mock backend was already clean, while ViGEm mode still stalled the same way (201.5 and 202.0 ms, 1,815 and 1,835 frames over 1 ms), because `ViGEmAdapter` held `busMtx_` across the plug IOCTLs and every submit took it. Now no scenario on either backend has a dish A frame over 1 ms or a ring drop; its worst frame is under 0.1 ms and its longest gap is what it is in the scenarios without a plug (about 0.2 ms at 8 kHz, 1.1 ms at S1's 1 kHz), while the plug still takes its 200 ms. The medians are unchanged at 15.4 us, the 15 us submit spin plus the path. S5's p99.9 near 30 us is a gamepad frame waiting behind one of dish A's 500 Hz motion submits on the one receive thread, which about 0.75% of its frames do, so its p99 lands either side of that line from run to run. Dish B's 15 refused frames are gone too: they were its frames from the plug's 200 ms, queued behind the stalled receive thread until the PUT had rotated the token, and they are now served on the old token while the plug runs.

### Before: 2.1.8 cold paths

`--submit-us 15`, `--plug-ms 200`, everything else at its default (3 s per run, 3 reps, dish A 8000 Hz, dish B 75 Hz, motion 500 Hz, mic 50 Hz, receiver on CPU 2), one invocation per backend; AMD Ryzen 9 7900X 12-Core Processor, Release build. The locking is 2.1.8's: outside the stats build a named lock is a `std::mutex` and a name pointer, and the thread roles and blocking markers compile to nothing. The columns are those of the 2.1.7 table.

| backend | scenario | dish | rate Hz | frames | p50 us | p99 us | p99.9 us | max us | over 1 ms | longest gap ms | drops | refused |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| mock | S1 | A | 1000 | 3000 | 15.4 | 16.0 | 17.2 | 18.4 | 0 | 1.003 | 0 | 0 |
| mock | S2 | A | 8000 | 24000 | 15.4 | 15.8 | 17.8 | 89.6 | 0 | 0.199 | 0 | 0 |
| mock | S2 | B | 75 | 225 | 15.5 | 31.3 | 105.0 | 105.0 | 0 | 13.422 | 0 | 0 |
| mock | S3 | A | 8000 | 24000 | 15.4 | 15.8 | 17.4 | 76.7 | 0 | 0.193 | 0 | 0 |
| mock | S3 | B | 75 | 225 | 15.5 | 30.8 | 30.9 | 30.9 | 0 | 13.349 | 0 | 0 |
| mock | S4 | A | 8000 | 24000 | 15.4 | 15.8 | 17.3 | 73.7 | 0 | 0.192 | 0 | 0 |
| mock | S4 | B | 75 | 225 | 15.5 | 30.8 | 31.8 | 31.8 | 0 | 13.349 | 0 | 0 |
| mock | S5 | A | 8000 | 24000 | 15.4 | 30.5 | 45.7 | 92.1 | 0 | 0.228 | 0 | 0 |
| mock | S5 | B | 75 | 225 | 15.5 | 45.9 | 45.9 | 45.9 | 0 | 13.364 | 0 | 0 |
| mock | S6 | A | 8000 | 24000 | 15.4 | 15.8 | 47.8 | 92.9 | 0 | 0.198 | 0 | 0 |
| mock | S6 | B | 75 | 224 | 15.5 | 48.7 | 56.9 | 56.9 | 0 | 26.652 | 0 | 1 |
| mock | S7 | A | 8000 | 24000 | 15.4 | 30.3 | 30.6 | 94.9 | 0 | 0.241 | 0 | 0 |
| mock | S7 | B | 75 | 225 | 15.5 | 45.9 | 46.9 | 46.9 | 0 | 13.365 | 0 | 0 |
| mock | S8 | A | 8000 | 24000 | 15.4 | 30.5 | 32.7 | 76.4 | 0 | 0.185 | 0 | 0 |
| mock | S8 | B | 75 | 225 | 15.5 | 46.0 | 46.5 | 46.5 | 0 | 13.364 | 0 | 0 |
| vigem | S1 | A | 1000 | 3000 | 15.5 | 16.0 | 23.3 | 81.3 | 0 | 1.066 | 0 | 0 |
| vigem | S2 | A | 8000 | 24000 | 15.4 | 19.2 | 34.5 | 87.0 | 0 | 0.197 | 0 | 0 |
| vigem | S2 | B | 75 | 225 | 15.5 | 33.9 | 36.3 | 36.3 | 0 | 13.354 | 0 | 0 |
| vigem | S3 | A | 8000 | 24000 | 15.4 | 19.7 | 35.1 | 90.4 | 0 | 0.200 | 0 | 0 |
| vigem | S3 | B | 75 | 225 | 15.5 | 30.9 | 32.0 | 32.0 | 0 | 13.349 | 0 | 0 |
| vigem | S4 | A | 8000 | 24000 | 15.4 | 21.3 | 33.5 | 92.8 | 0 | 0.203 | 0 | 0 |
| vigem | S4 | B | 75 | 225 | 15.5 | 38.6 | 57.1 | 57.1 | 0 | 13.360 | 0 | 0 |
| vigem | S5 | A | 8000 | 24000 | 15.5 | 30.5 | 46.0 | 90.7 | 0 | 0.200 | 0 | 0 |
| vigem | S5 | B | 75 | 225 | 15.5 | 46.3 | 50.0 | 50.0 | 0 | 13.368 | 0 | 0 |
| vigem | S6 | A | 8000 | 24000 | 15.5 | 20.2 | 45.5 | 82.0 | 0 | 0.192 | 0 | 0 |
| vigem | S6 | B | 75 | 225 | 15.5 | 34.0 | 38.6 | 38.6 | 0 | 13.357 | 0 | 0 |
| vigem | S7 | A | 8000 | 24000 | 15.5 | 30.5 | 46.0 | 81.7 | 0 | 0.196 | 0 | 0 |
| vigem | S7 | B | 75 | 225 | 15.5 | 46.0 | 75.5 | 75.5 | 0 | 13.378 | 0 | 0 |
| vigem | S8 | A | 8000 | 24000 | 15.4 | 30.7 | 47.2 | 71.8 | 0 | 0.195 | 0 | 0 |
| vigem | S8 | B | 75 | 225 | 15.5 | 46.0 | 55.4 | 55.4 | 0 | 13.374 | 0 | 0 |

The locks touched in the reported rep of S6 to S8, from one `bench_frame_path_lockstats` invocation per backend with the same settings (so its reps are not the ones above), one row per lock and thread role. Waits and holds are in microseconds, and the p99 wait is the upper edge of its log2 bucket.

| backend | scenario | lock | role | acquisitions | contended | failed tries | max wait us | p99 wait us | max hold us |
| --- | --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| mock | S6 | config | Receive | 150 | 0 | 0 | 4.5 | 0.5 | 0.1 |
| mock | S6 | config | HttpClient | 1 | 0 | 0 | 0.0 | 0.0 | 1574.6 |
| mock | S6 | service.topology | Maintenance | 3 | 0 | 0 | 0.0 | 0.0 | 0.7 |
| mock | S6 | service.topology | HttpClient | 1 | 0 | 0 | 0.4 | 0.5 | 201533.1 |
| mock | S6 | service.state | Receive | 150 | 0 | 0 | 0.1 | 0.1 | 60.7 |
| mock | S6 | service.state | Maintenance | 63 | 0 | 0 | 0.3 | 0.5 | 0.6 |
| mock | S6 | service.state | HttpAdmin | 12 | 0 | 0 | 0.3 | 0.5 | 7.9 |
| mock | S6 | service.state | HttpClient | 4 | 0 | 0 | 0.1 | 0.1 | 8.1 |
| mock | S6 | log | Receive | 1 | 0 | 0 | 0.1 | 0.1 | 0.4 |
| mock | S6 | log | HttpClient | 2 | 0 | 0 | 0.0 | 0.0 | 1.1 |
| mock | S7 | service.topology | Maintenance | 3 | 0 | 0 | 0.0 | 0.0 | 0.7 |
| mock | S7 | service.topology | HttpClient | 1 | 0 | 0 | 0.2 | 0.3 | 202035.9 |
| mock | S7 | service.state | Receive | 1503 | 0 | 0 | 0.4 | 0.1 | 25.6 |
| mock | S7 | service.state | Maintenance | 63 | 0 | 0 | 0.5 | 0.5 | 1.3 |
| mock | S7 | service.state | HttpAdmin | 12 | 0 | 0 | 0.3 | 0.5 | 5.4 |
| mock | S7 | service.state | HttpClient | 4 | 0 | 0 | 0.2 | 0.3 | 5.2 |
| mock | S7 | log | HttpAdmin | 30 | 14 | 0 | 26.8 | 32.8 | 1160.6 |
| mock | S7 | log | HttpClient | 2 | 0 | 0 | 0.1 | 0.1 | 0.7 |
| mock | S7 | log | Background | 24000 | 5885 | 0 | 1204.0 | 32.8 | 6.3 |
| mock | S8 | config | HttpAdmin | 236 | 8 | 0 | 13.5 | 16.4 | 17.5 |
| mock | S8 | service.topology | Maintenance | 3 | 0 | 0 | 0.0 | 0.0 | 0.9 |
| mock | S8 | service.topology | HttpClient | 1 | 0 | 0 | 0.2 | 0.3 | 202059.7 |
| mock | S8 | service.state | Receive | 1503 | 2 | 0 | 11.6 | 0.3 | 28.9 |
| mock | S8 | service.state | Maintenance | 63 | 7 | 0 | 27.4 | 32.8 | 1.9 |
| mock | S8 | service.state | HttpAdmin | 720 | 27 | 0 | 25.3 | 16.4 | 9.3 |
| mock | S8 | service.state | HttpClient | 4 | 0 | 0 | 0.2 | 0.3 | 4.7 |
| mock | S8 | log | HttpClient | 2 | 0 | 0 | 0.1 | 0.1 | 0.8 |
| vigem | S6 | vigem.bus | HttpClient | 1 | 0 | 0 | 0.2 | 0.3 | 34.4 |
| vigem | S6 | config | Receive | 150 | 0 | 0 | 0.3 | 0.3 | 0.1 |
| vigem | S6 | config | HttpClient | 1 | 0 | 0 | 0.0 | 0.0 | 1649.2 |
| vigem | S6 | service.topology | Maintenance | 3 | 0 | 0 | 0.0 | 0.0 | 0.8 |
| vigem | S6 | service.topology | HttpClient | 1 | 0 | 0 | 0.2 | 0.3 | 201581.6 |
| vigem | S6 | service.state | Receive | 150 | 0 | 0 | 0.1 | 0.1 | 61.1 |
| vigem | S6 | service.state | Maintenance | 63 | 0 | 0 | 0.7 | 1.0 | 1.3 |
| vigem | S6 | service.state | HttpAdmin | 12 | 0 | 0 | 0.7 | 1.0 | 12.9 |
| vigem | S6 | service.state | HttpClient | 4 | 0 | 0 | 0.1 | 0.1 | 5.6 |
| vigem | S6 | log | Receive | 1 | 0 | 0 | 0.0 | 0.0 | 0.6 |
| vigem | S6 | log | HttpClient | 2 | 0 | 0 | 0.1 | 0.1 | 0.9 |
| vigem | S7 | vigem.bus | HttpClient | 1 | 0 | 0 | 0.1 | 0.1 | 32.0 |
| vigem | S7 | service.topology | Maintenance | 3 | 0 | 0 | 0.0 | 0.0 | 0.7 |
| vigem | S7 | service.topology | HttpClient | 1 | 0 | 0 | 0.2 | 0.3 | 201602.3 |
| vigem | S7 | service.state | Receive | 1503 | 0 | 0 | 0.2 | 0.1 | 17.0 |
| vigem | S7 | service.state | Maintenance | 63 | 0 | 0 | 0.6 | 1.0 | 1.6 |
| vigem | S7 | service.state | HttpAdmin | 12 | 0 | 0 | 0.4 | 0.5 | 10.0 |
| vigem | S7 | service.state | HttpClient | 4 | 0 | 0 | 0.1 | 0.1 | 9.1 |
| vigem | S7 | log | HttpAdmin | 30 | 11 | 0 | 3.6 | 4.1 | 1029.2 |
| vigem | S7 | log | HttpClient | 2 | 1 | 0 | 0.4 | 0.5 | 0.4 |
| vigem | S7 | log | Background | 24000 | 5904 | 0 | 1053.5 | 32.8 | 5.6 |
| vigem | S8 | vigem.bus | HttpClient | 1 | 0 | 0 | 0.1 | 0.1 | 31.3 |
| vigem | S8 | config | HttpAdmin | 236 | 11 | 0 | 14.0 | 16.4 | 29.6 |
| vigem | S8 | service.topology | Maintenance | 3 | 0 | 0 | 0.0 | 0.0 | 0.7 |
| vigem | S8 | service.topology | HttpClient | 1 | 0 | 0 | 0.2 | 0.3 | 201572.1 |
| vigem | S8 | service.state | Receive | 1503 | 0 | 0 | 0.2 | 0.1 | 17.2 |
| vigem | S8 | service.state | Maintenance | 63 | 11 | 0 | 13.8 | 16.4 | 1.2 |
| vigem | S8 | service.state | HttpAdmin | 720 | 29 | 0 | 24.3 | 16.4 | 12.1 |
| vigem | S8 | service.state | HttpClient | 4 | 1 | 0 | 19.2 | 32.8 | 9.9 |
| vigem | S8 | log | HttpClient | 2 | 0 | 0 | 0.1 | 0.1 | 0.8 |

No scenario on either backend has a dish A frame over 1 ms or a ring drop, and dish A's worst frame stays under 0.1 ms. The gamepad path takes none of these locks; its frames wait only while the one receive thread handles, or waits on, another message, and the Receive rows show what that thread takes: `config` and then `service.state` for every `MSG_MIC_AUDIO` (the mic handler reads the audio policy from the config before it takes the service lock, and decodes the Opus packet under it), `service.state` for every `MSG_MOTION` and heartbeat, and `log` for any line it logs. S6's p99.9 near 46 us is a gamepad frame queued behind a mic frame's decode. S7 and S8 put S5's traffic through the receive thread, and their dish A rows stay inside S5's spread: the p99.9 lands near 31 or near 46 us from run to run, one or two 15 us submits queued ahead of the frame.

In S6, dish B's PUT holds `config` for about 1.6 ms while `refreshPairedDeviceIdentity` writes the file (1,574.6 us mock, 1,649.2 us vigem), yet the receive thread's longest wait on `config` is 4.5 us, because the defaults keep the two apart: the 50 Hz mic frames sit on a 20 ms grid from the start of the run, and the PUT due at 1.0 s, with its 200 ms plug, starts the save about 1.6 ms after a mic frame and ends it long before the next. With `--mic-hz 1000` a mic frame lands inside the save every time; the handler reads the config before its 75 packets/s limit, so even the frames it then drops take the lock. There the receive thread waited up to 893.8 us (mock) and 1,293.2 us (vigem) on `config`, the gamepad frames queued behind it waited as long, and dish A's worst frame in each of the six reps was between 0.5 and 1.7 ms; the reported vigem rep has 3 frames over 1 ms and a 1.395 ms gap. Dish B's refused frame in the mock row is one it sent on the old token during the save: the PUT replies only after the save, as `PUT /api/connections` does, so B switched keys about 1.6 ms later than in S4, and the lost frame doubled its longest gap.

In S7 the four loggers contend for `log` with each other and with the logs reader, which holds it for up to 1.2 ms while it builds the JSON (1,160.6 us mock, 1,029.2 us vigem), and a logging thread waited up to 1,204.0 us. The receive thread logged nothing in those reps, so dish A never met the lock; a line logged on the receive path would wait behind the same build, and the mic handler logs its once-per-session lines while it holds `service.state`. In S8 the HttpAdmin rows are the eight dashboard readers and the sse thread, which take `service.state` 720 times and `config` 236 times per rep and hold either for at most 29.6 us; the receive thread met them twice in the mock rep, waiting up to 11.6 us on `service.state`, and not at all in the vigem rep. The PUT holds `service.topology` across the whole 200 ms plug in every scenario, which is by design: the receive path never takes it.
