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

The default backend is a mock: `submitReport` and `submitMotion` spin `--submit-us` (the synchronous ViGEm call), `pluginDevice` sleeps `--plug-ms` (the Plug and Play wait, armed once the sessions are open) and `unplugDevice` sleeps 5 ms. `--backend vigem` swaps it for the real `ViGEmAdapter` behind `GamepadMux`, linked against a fake driver layer (`tools/bench/fake_vigem_driver.cpp`) whose plug, unplug and submit calls take those same times, so the adapter's own `busMtx_`, held across the plug and taken by `submitReport`, is part of what is timed. S1 is dish A alone at 1 kHz. S2 is dish A at `--rate-a` plus dish B at `--rate-b`; neither runs a cold thread. S3 adds an `sse` thread (1 Hz: `getConnectionsSnapshot`, `linkStateForDevice` per dish, `activeSessionCount`) and a `maintenance` thread (10 Hz: `refreshRumble`, `refreshIdleInput`, `reapTimedOut` on every 10th tick). S4 adds a PUT for dish B at 1.0 s with a second controller, which plugs a new pad while the service holds its lock; B's producer then switches to the new token and key, and the frames still queued under the old token count as `refused`. S5 is S4 plus the traffic that reaches the service through `dispatchInnerMessage`: dish A is declared as a PlayStation pad with `CAP_MOTION` and streams `MSG_MOTION` at `--motion-hz` (default 500) from its own producer and ring, and each dish sends a `MSG_HEARTBEAT_PING` every 2 s, A first at 0.3 s and B at 1.3 s. A dish's message types share one counter, so the receiver rescans until two scans agree on the oldest ring and never serves a motion frame after a later gamepad frame of the same dish, which the replay check would reject. Each scenario runs `--secs` seconds (default 3), `--reps` times (default 3) interleaved, after a discarded warm-up pass; the table shows the rep whose dish A p99.9 is the median, and it and the CSV carry a `backend` column (`mock` or `vigem`).

Build it with `cmake --build <build dir> --target bench_frame_path`; the binary lands at the repo root, and `--csv <path>` also writes the rows. `bench_frame_path --smoke` runs every scenario for 0.2 s on both backends with the cold-thread schedule compressed to fit, and ctest runs it as `frame_path_bench_smoke`; the exit code reflects the harness's own accounting (and usage errors), never a latency, and `--help` lists the options. The process keeps the default priority class and timer resolution, which the application raises, so compare runs of the same binary on the same machine.

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
