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

## Building

```batch
build.bat
```

Or manually:

```bash
# Receiver (tray app + web UI)
g++ -O2 -Wall -std=c++17 -D_WIN32_WINNT=0x0A00 -static -DCPPHTTPLIB_NO_EXCEPTIONS \
    -Ivigem/include -Ilib -o controller-receiver.exe controller-receiver.cpp \
    -lsetupapi -lws2_32 -lshell32 -lole32 -mwindows

# Sender
g++ -O2 -Wall -std=c++17 -D_WIN32_WINNT=0x0A00 -static \
    -o controller-sender.exe controller-sender.cpp -lxinput1_4 -lws2_32
```

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
clang-format -i src/*.cpp src/*.h controller-sender.cpp controller-receiver.cpp
```

**Lint with clang-tidy:**

```powershell
clang-tidy src/*.cpp controller-sender.cpp controller-receiver.cpp -- -std=c++17 -Ivigem/include -Isrc -Ilib -D_WIN32_WINNT=0x0A00 -DCPPHTTPLIB_NO_EXCEPTIONS
```

**Run cppcheck:**

```powershell
cppcheck --enable=all --std=c++17 --suppress=missingIncludeSystem -Ivigem/include -Isrc -Ilib src/ controller-sender.cpp controller-receiver.cpp
```

### Editor Integration

- **VS Code:** Install the [clang-format](https://marketplace.visualstudio.com/items?itemName=xaver.clang-format) extension and set it as your default C++ formatter. Format-on-save will use the `.clang-format` config automatically.
- **clang-tidy** warnings appear inline if you use [clangd](https://marketplace.visualstudio.com/items?itemName=llvm-vs-code-extensions.vscode-clangd) as your language server.

## Project structure

```
├── controller-receiver.cpp     # Receiver: tray app + web UI + UDP → ViGEmBus
├── controller-sender.cpp       # Sender: XInput → UDP
├── lib/
│   └── httplib.h               # cpp-httplib (header-only HTTP server, vendored)
├── vigem/
│   └── include/ViGEm/
│       ├── Common.h            # XUSB_REPORT, button definitions
│       └── BusShared.h         # IOCTL codes, driver structures
├── .clang-format               # Code formatting rules (clang-format)
├── .clang-tidy                 # Linting & modernization checks (clang-tidy)
├── build.bat                   # Build script
├── LICENSE                     # MIT
└── README.md
```

## License

MIT — see [LICENSE](LICENSE).

- [cpp-httplib](https://github.com/yhirose/cpp-httplib) by Yuji Hirose (MIT License)
- ViGEm header definitions derived from [nefarius/ViGEmBus](https://github.com/nefarius/ViGEmBus) (MIT License)

