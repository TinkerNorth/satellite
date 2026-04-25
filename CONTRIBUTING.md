# Contributing to Satellite

Thanks for your interest in improving Satellite! This document captures the
conventions that aren't obvious from skimming the code.

## Getting set up

```bash
# 1) Install build deps for your platform (see README "Prerequisites")
# 2) Generate compile_commands.json + run the test suite
cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
# 3) Point git at the in-tree pre-commit hook
./scripts/setup-hooks.sh
```

The pre-commit hook runs `clang-format -i` (autofix, re-stages) on staged
C/C++/Objective-C++ files. It skips gracefully if the tool isn't installed —
CI re-runs `clang-format --dry-run --Werror` in strict mode on Linux,
macOS, and Windows runners, so anything that slips locally fails the PR.

## License headers

Every source file (`*.h`, `*.cpp`, `*.mm`) starts with:

```cpp
// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.
```

New files must include both lines. Don't introduce code under a different
license — the project is LGPL-3.0-or-later end-to-end (`LICENSE`,
`COPYING.GPL3`, source headers). Vendored third-party components under
`lib/` and `vigem/include/` keep their original (MIT-compatible) licenses
and are noted in the README.

## Style

- C++17, four-space indent, 100-column soft limit. The same `.clang-format`
  ships with `dish-android` (JNI) and `dish-linux` — run `clang-format -i`
  if you're unsure.
- Hexagonal architecture: `core/` is pure domain logic with **no platform
  deps**. Anything that touches sockets, files, the registry, the Cocoa
  runtime, ViGEm, etc. lives in `adapters/` or `platform/<os>/`.
  `SessionService` only depends on the abstract port interfaces in
  `core/ports.h`.
- New ports must be testable from `tests/test_session_service.cpp` with a
  mock implementation. Don't add framework dependencies to the test suite —
  the tiny in-tree assertion macros are enough for the current scope.

## Branching & PRs

- All changes land on `main` via pull request — no direct pushes.
- Use the PR template (`.github/pull_request_template.md`) to describe
  the change, the manual test matrix you ran (which platforms, with/without
  ViGEm), and call out anything that touches the wire protocol.
- Keep commits focused; squash noisy fixup commits before review.

## What CI runs

Three workflows run on every PR:

| Workflow | Runner | What it does |
|---|---|---|
| `linux-ci.yml` | ubuntu-24.04 | clang-format check, tray-enabled + headless builds, ctest, AppIndicator link verification |
| `macos-ci.yml` | macos-14 | clang-format check, build + ctest, .app layout verification, uploads `satellite-macos-stub.app` |
| `windows-ci.yml` | windows-latest | clang-format check, MinGW MSYS2 build, ctest, uploads `satellite.exe` |

All three workflows install clang-format **pinned to 22.1.4** so verdicts
match across runners. If any step fails, the PR is blocked.

## Touching the hot path

The receiver thread runs at `recvfrom()` rate and must never allocate or
take a long lock. From `net/receiver.cpp`:

- The hot path is three syscalls with zero allocations:
  `recvfrom()` → `memcpy()` → `DeviceIoControl()` (Windows) or
  `write(uinput_fd)` (Linux).
- Mutexes guard the connection table only at packet boundaries.
- No logging on the per-packet path.

## Touching the wire protocol

The server and all three Dish clients (Android, macOS, Linux) must
produce byte-identical traffic:

- AEAD: ChaCha20-Poly1305 IETF, 12-byte big-endian nonce derived from a
  monotonic counter.
- Packet layout: `token(4) | counter(4) | ciphertext+tag`, with the
  4-byte token as AAD.
- XUSB report: 12 bytes, little-endian.
- Ports: discovery UDP 9879, pairing TCP 9878, HTTP TCP 9877,
  streaming UDP 9876.

Any change here must be coordinated with `dish-android`, `dish-linux`,
and `dish-mac` in the same PR / release cycle.

## Platform notes

- **Windows** is the canonical target — virtual gamepads via ViGEmBus.
- **Linux** synthesizes virtual gamepads through `/dev/uinput`. Optional
  tray icon via libayatana-appindicator (CMake auto-detects; falls back
  to a headless `sigwait` loop).
- **macOS** is a **stub**: no signed DriverKit equivalent of ViGEmBus is
  available, so the build runs the protocol stack but rejects
  controller-add requests with `ACK_ERR_VIGEM_UNAVAIL`. The CI artifact
  is named `satellite-macos-stub.app` to make this explicit, and the
  binary logs a banner at startup.

## Reporting bugs

Use the issue templates under `.github/ISSUE_TEMPLATE/`. Include the
OS and version, the relevant log excerpt (Windows: `%APPDATA%\satellite\`;
Linux: `journalctl --user -e`), and which Dish client is connecting.
