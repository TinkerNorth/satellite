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

`bench_frame_path` (Windows only; `tools/bench/frame_path_bench.cpp`) sends real encrypted `MSG_GAMEPAD_DATA` datagrams through `SessionService` the way the receiver thread does and times each frame from arrival to submit. One spin-paced producer per dish seals each datagram with the key derived from what `upsertSession` returned, stamps it with `steady_clock` and pushes it into a 1,600-entry ring (the 64 KiB socket buffer); a full ring counts a drop. One receiver thread, `TIME_CRITICAL` and pinned to CPU 2 by default (the installed Satellite pins its own to CPU 0), takes the oldest frame across the rings and runs the receiver's steps in order: `getDecryptInfo`, the replay check, in-place `decryptPacket`, `parseInnerHeader`, `handleGamepadDataAndUpdate`. Latency is the clock read after that last call returns minus the stamp.

The ports are mocks: `submitReport` spins `--submit-us` (the synchronous ViGEm call), `pluginDevice` sleeps `--plug-ms` (the Plug and Play wait, armed once the sessions are open) and `unplugDevice` sleeps 5 ms. S1 is dish A alone at 1 kHz. S2 is dish A at `--rate-a` plus dish B at `--rate-b`; neither runs a cold thread. S3 adds an `sse` thread (1 Hz: `getConnectionsSnapshot`, `linkStateForDevice` per dish, `activeSessionCount`) and a `maintenance` thread (10 Hz: `refreshRumble`, `refreshIdleInput`, `reapTimedOut` on every 10th tick). S4 adds a PUT for dish B at 1.0 s with a second controller, which plugs a new pad while the service holds its lock; B's producer then switches to the new token and key, and the frames still queued under the old token count as `refused`. Each scenario runs `--secs` seconds (default 3), `--reps` times (default 3) interleaved, after a discarded warm-up pass; the table shows the rep whose dish A p99.9 is the median.

Build it with `cmake --build <build dir> --target bench_frame_path`; the binary lands at the repo root, and `--csv <path>` also writes the rows. `bench_frame_path --smoke` runs every scenario for 0.2 s with the cold-thread schedule compressed to fit, and ctest runs it as `frame_path_bench_smoke`; the exit code reflects the harness's own accounting (and usage errors), never a latency, and `--help` lists the options. The process keeps the default priority class and timer resolution, which the application raises, so compare runs of the same binary on the same machine.

### Before: 2.1.7 lock path

`--submit-us 15`, `--plug-ms 200`, everything else at its default (3 s per run, 3 reps, dish A 8000 Hz, dish B 75 Hz, receiver on CPU 2); AMD Ryzen 9 7900X 12-Core Processor, Release build. Latencies are in microseconds; `over 1 ms` counts frames slower than 1 ms; `longest gap` is the longest wait between two completed frames of that dish, in ms; `drops` counts ring overflows; `refused` counts frames whose token the service no longer knew.

| scenario | dish | rate Hz | frames | p50 us | p99 us | p99.9 us | max us | over 1 ms | longest gap ms | drops | refused |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| S1 | A | 1000 | 3000 | 15.4 | 16.0 | 17.4 | 46.0 | 0 | 1.030 | 0 | 0 |
| S2 | A | 8000 | 24000 | 15.4 | 15.9 | 17.8 | 63.7 | 0 | 0.183 | 0 | 0 |
| S2 | B | 75 | 225 | 15.4 | 31.0 | 32.6 | 32.6 | 0 | 13.353 | 0 | 0 |
| S3 | A | 8000 | 24000 | 15.4 | 15.8 | 17.6 | 70.2 | 0 | 0.232 | 0 | 0 |
| S3 | B | 75 | 225 | 15.5 | 30.9 | 31.2 | 31.2 | 0 | 13.349 | 0 | 0 |
| S4 | A | 8000 | 23988 | 15.4 | 175346.5 | 199020.8 | 201541.4 | 1817 | 201.651 | 12 | 0 |
| S4 | B | 75 | 210 | 15.4 | 30.9 | 22857.7 | 22857.7 | 2 | 236.161 | 0 | 15 |
