# Satellite

Low-latency Xbox controller forwarding over the network. Captures physical controller input on one machine and injects it as a virtual Xbox 360 controller on another — similar to how [Moonlight](https://github.com/moonlight-stream/moonlight-qt) / [Sunshine](https://github.com/LizardByte/Sunshine) handle input, but as a standalone tool.

Runs as a **system tray application** with a built-in **web UI** for configuration — no console window required.

## How it works

```
┌──────────────┐        UDP (12 bytes)       ┌──────────────────┐
│    Sender     │ ─────────────────────────►  │     Receiver     │
│  (XInput)     │    XUSB_REPORT packet       │  (ViGEmBus)      │
│               │                             │                  │
│ Physical Xbox │                             │ Virtual Xbox 360 │
│  controller   │                             │   controller     │
└──────────────┘                              └──────────────────┘
```

**Sender** polls a physical Xbox controller via XInput at ~250 Hz and streams 12-byte `XUSB_REPORT` packets over UDP.

**Receiver** runs as a system tray app. It listens for those packets and injects them into Windows as a virtual Xbox 360 controller through the ViGEmBus kernel driver — no DLLs required, communicates directly via `DeviceIoControl`.

The hot path is three syscalls with zero allocations: `recvfrom()` → `memcpy()` → `DeviceIoControl()`.

## Features

- **System tray icon** — right-click for Start/Stop, Open Web UI, Exit
- **Web-based configuration** — local dashboard at `http://localhost:9877`
- **Live status** — packet count, sender IP, listening state
- **Configurable UDP port** — change via the web UI
- **Start with Windows** — optional auto-start via registry
- **Zero dependencies** — statically linked, single exe, no DLLs needed
- **Config persistence** — settings saved to `%APPDATA%\satellite\config.json`

## Prerequisites

### Receiver machine
- **Windows 10/11**
- **[ViGEmBus driver](https://github.com/nefarius/ViGEmBus/releases)** — install the latest release

### Sender machine
- **Windows 10/11** with an Xbox controller connected

### Build toolchain (either machine)
- **[MinGW-w64](https://winlibs.com/)** (g++) — or any C++17 compiler targeting Windows
- **[Inno Setup 6](https://jrsoftware.org/isinfo.php)** — only needed to build the installer

## Installation

Download `SatelliteSetup.exe` from the [Releases](https://github.com/TinkerNorth/satellite/releases) page and run it. The installer will:

- Install Satellite to `Program Files\Satellite`
- Create a Start Menu shortcut
- Optionally create a Desktop shortcut
- Optionally set Satellite to start with Windows
- Register in **Settings → Apps → Installed Apps** with a proper uninstaller

To uninstall, use **Settings → Apps → Installed Apps → Satellite → Uninstall**, or run the uninstaller from the Start Menu.

## Building

```batch
build-satellite.bat
```

### Building the installer

After building `satellite.exe`, compile the installer with [Inno Setup](https://jrsoftware.org/isinfo.php):

```powershell
iscc installer.iss
```

This produces `dist\SatelliteSetup.exe` — a single installer that packages the app, web UI, and uninstaller.

## Usage

### Receiver (tray app)

Run `controller-receiver.exe`. It starts minimized to the system tray.

- **Right-click** the tray icon → **Open Web UI** to configure
- Or open `http://localhost:9877` in your browser
- Click **Start** in the web UI to begin listening for controller input
- Enable **Start with Windows** to auto-launch on boot

### Sender (console app)

```
controller-sender.exe <receiver-ip> [port] [poll-rate-hz] [controller-index]
controller-sender.exe 192.168.1.50 9876 250 0
```

| Argument           | Default     | Description                          |
|--------------------|-------------|--------------------------------------|
| `receiver-ip`      | `127.0.0.1` | IP address of the receiver machine   |
| `port`             | `9876`      | UDP port                             |
| `poll-rate-hz`     | `250`       | How often to poll the controller (Hz)|
| `controller-index` | `0`         | XInput user index (0-3)              |

## Architecture

The receiver runs three threads:

| Thread       | Role                                                        |
|--------------|-------------------------------------------------------------|
| **Main**     | Win32 message loop, system tray icon                        |
| **Receiver** | UDP socket → ViGEmBus `DeviceIoControl` (the hot path)      |
| **HTTP**     | Embedded web server ([cpp-httplib](https://github.com/yhirose/cpp-httplib)) serving the config UI |

## Why UDP?

TCP's reliability guarantees cause **head-of-line blocking** — if one packet is lost, all subsequent packets are held until retransmission completes. For controller input, only the latest state matters. A lost packet is better than a delayed one. This is the same approach used by Moonlight, Parsec, and Steam Remote Play.

## Code Quality

The project uses industry-standard C++ tooling for formatting, linting, and static analysis.

### Tools

| Tool | Purpose | Config file | Equivalent in JS |
|------|---------|-------------|------------------|
| **clang-format** | Auto-formatting | `.clang-format` | Prettier |
| **clang-tidy** | Linting & modernization | `.clang-tidy` | ESLint |
| **cppcheck** | Deep static analysis (leaks, UB, bounds) | — | — |

### Installing

All three tools are available via [winget](https://learn.microsoft.com/en-us/windows/package-manager/winget/):

```powershell
winget install LLVM.ClangFormat      # clang-format
winget install LLVM.LLVM             # clang-tidy (included in full LLVM)
winget install Cppcheck.Cppcheck     # cppcheck
```

> **Note:** `clang-format` can also be installed standalone via `LLVM.ClangFormat` if you don't need the full LLVM toolchain. `clang-tidy` requires the full `LLVM.LLVM` package.

After installing, restart your terminal so the tools are on your `PATH`.

### Usage

**Format all source files:**

```powershell
clang-format -i src/*.cpp src/*.h src/adapters/*.cpp src/adapters/*.h src/core/*.cpp src/core/*.h
```

**Lint with clang-tidy:**

```powershell
clang-tidy src/*.cpp src/adapters/*.cpp src/core/*.cpp -- -std=c++17 -Ivigem/include -Isrc -Ilib -Ilib/libsodium/libsodium-win64/include -D_WIN32_WINNT=0x0A00 -DCPPHTTPLIB_NO_EXCEPTIONS
```

**Run cppcheck:**

```powershell
cppcheck --enable=all --std=c++17 --suppress=missingIncludeSystem -Ivigem/include -Isrc -Ilib -Ilib/libsodium/libsodium-win64/include src/
```

### Editor Integration

- **VS Code:** Install the [clang-format](https://marketplace.visualstudio.com/items?itemName=xaver.clang-format) extension and set it as your default C++ formatter. Format-on-save will use the `.clang-format` config automatically.
- **clang-tidy** warnings appear inline if you use [clangd](https://marketplace.visualstudio.com/items?itemName=llvm-vs-code-extensions.vscode-clangd) as your language server.

## Testing

Unit tests cover the core domain service (`SessionService`) using mock implementations of all port interfaces. No external test framework is required.

### Running tests

```powershell
build-tests.bat
```

This compiles `tests/test_session_service.cpp` alongside `src/core/session_service.cpp`, then runs the test binary. Output shows pass/fail counts and individual failure details.

### What's tested

- **Connection lifecycle** — open, close, close-all, stale replacement, multiple devices
- **Controller lifecycle** — add, remove, serial allocation/recycling, bus open/close
- **Error handling** — invalid tokens, out-of-bounds indices, ViGEm unavailable, no slots, plugin failures
- **Gamepad data** — report submission, inactive controller rejection
- **Heartbeat** — ACK + server status responses
- **Decrypt helpers** — key retrieval, counter updates
- **Query/stats** — snapshot, device connected check, slot counts
- **Broadcast** — status broadcasts on controller changes

## Project structure

```
├── src/
│   ├── core/                   # Pure domain logic (no platform deps)
│   │   ├── types.h             # Data structs, constants, enums
│   │   ├── ports.h             # Outbound port interfaces
│   │   ├── session_service.h   # SessionService declaration
│   │   └── session_service.cpp # SessionService implementation
│   ├── adapters/               # Infrastructure adapters
│   │   ├── vigem_adapter.*     # IViGemPort — ViGEm bus driver
│   │   ├── client_adapter.*    # IClientPort — encrypted UDP
│   │   ├── log_adapter.*       # ILogPort — ring buffer logger
│   │   └── config_adapter.*    # IConfigPort — JSON config
│   ├── main.cpp                # Composition root (wires adapters)
│   ├── receiver.cpp            # UDP recv/decrypt loop
│   ├── webserver.cpp           # HTTP API + SSE + web UI
│   └── ...                     # config, crypto, pairing, discovery, tray
├── tests/
│   └── test_session_service.cpp # Unit tests (self-contained, no framework)
├── lib/
│   └── httplib.h               # cpp-httplib (vendored)
├── vigem/include/ViGEm/        # ViGEm driver headers
├── web/                        # Web UI static files
├── build-satellite.bat         # Build script
├── build-tests.bat             # Test build & run script
├── .clang-format               # Code formatting rules
├── .clang-tidy                 # Linting checks
├── LICENSE                     # MIT
└── README.md
```

## License

MIT — see [LICENSE](LICENSE).

- [cpp-httplib](https://github.com/yhirose/cpp-httplib) by Yuji Hirose (MIT License)
- ViGEm header definitions derived from [nefarius/ViGEmBus](https://github.com/nefarius/ViGEmBus) (MIT License)

