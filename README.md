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
- **Windows 10/11** with the **[ViGEmBus driver](https://github.com/nefarius/ViGEmBus/releases)** installed, **or**
- **Linux** with the in-tree `uinput` kernel module and write access to `/dev/uinput` (see [Building → Linux](#linux) for the udev/group setup), **or**
- **macOS** for development / web-UI testing only — controller injection is unavailable, so the receiver returns `ACK_ERR_VIGEM_UNAVAIL` for `add controller` requests

### Sender machine
- **Windows 10/11** with an Xbox controller connected (XInput is the only sender backend at present)

### Build toolchain
- **Windows:** **[MinGW-w64](https://winlibs.com/)** (g++) — or any C++17 compiler targeting Windows. **[Inno Setup 6](https://jrsoftware.org/isinfo.php)** is only needed to build the installer.
- **Linux / macOS:** `cmake`, `pkg-config`, a C++17 compiler, and the libsodium development headers. See the per-platform sections in [Building](#building) for distro-specific package names.

## Installation

Download `SatelliteSetup.exe` from the [Releases](https://github.com/TinkerNorth/satellite/releases) page and run it. The installer will:

- Install Satellite to `Program Files\Satellite`
- Create a Start Menu shortcut
- Optionally create a Desktop shortcut
- Optionally set Satellite to start with Windows
- Register in **Settings → Apps → Installed Apps** with a proper uninstaller

To uninstall, use **Settings → Apps → Installed Apps → Satellite → Uninstall**, or run the uninstaller from the Start Menu.

## Building

### Windows

```batch
build-satellite.bat
```

### macOS

Virtual-gamepad injection is not available on macOS — there is no signed
DriverKit equivalent of ViGEmBus — so the macOS build produces a server that
still pairs, authenticates, serves the web UI, and reports status to clients,
but refuses controller-add requests with `ACK_ERR_VIGEM_UNAVAIL`. This is
useful for development, protocol testing, and running the web dashboard from
a Mac; it is **not** a drop-in receiver for game input.

Prerequisites:

- Xcode Command Line Tools (`xcode-select --install`)
- Homebrew packages: `brew install cmake pkg-config libsodium`

Build:

```bash
./build-satellite.sh
```

The script runs `cmake -S . -B build` and `cmake --build build`, producing a
`satellite.app` bundle at the repo root. Config lives at
`~/Library/Application Support/satellite/config.json`; the DPAPI-equivalent
keyfile lives at `~/.config/satellite/keyfile` (mode `0600`). "Run at login"
is implemented via a `LaunchAgents` plist.

### Linux

Linux is a fully-featured receiver: virtual gamepads are synthesized via
`/dev/uinput` (the in-tree `uinput` kernel module — no out-of-tree driver
needed). Pairing, encryption, the web UI, and config persistence all match
the Windows behavior.

Prerequisites:

- `cmake`, `g++` (or `clang++`), `pkg-config`
- libsodium development headers
    - Debian/Ubuntu: `sudo apt install libsodium-dev`
    - Fedora/RHEL:   `sudo dnf install libsodium-devel`
    - Arch:          `sudo pacman -S libsodium`
- *(Optional, for the system tray icon)* libayatana-appindicator + GTK3 dev
  headers. Without these, the binary builds **headless** and just runs a
  `sigwait` loop — the web UI at `http://localhost:9877` is still the primary
  control surface either way.
    - Debian/Ubuntu: `sudo apt install libayatana-appindicator3-dev libgtk-3-dev`
    - Fedora/RHEL:   `sudo dnf install libayatana-appindicator-gtk3-devel gtk3-devel`
    - Arch:          `sudo pacman -S libayatana-appindicator gtk3`

> **Vanilla GNOME users:** GNOME has no built-in tray. Install the
> [AppIndicator and KStatusNotifierItem Support](https://extensions.gnome.org/extension/615/appindicator-support/)
> extension to see the icon. KDE, XFCE, Cinnamon, MATE, Budgie, and Pantheon
> work out of the box.

There are two install paths: a **`.deb` package** (recommended on
Debian/Ubuntu — handles the udev rule, `uinput` module, and group setup
automatically) and a **portable build** (works anywhere, but you do the
permission setup once by hand).

#### Option A — `.deb` package (Debian / Ubuntu)

Build the package:

```bash
sudo apt install build-essential cmake pkg-config dpkg-dev \
                 libsodium-dev \
                 libayatana-appindicator3-dev libgtk-3-dev   # optional tray
./build-deb.sh
```

Output: `dist/satellite_<version>_<arch>.deb`. Install it:

```bash
sudo apt install ./dist/satellite_*.deb
```

The postinstall script reloads udev, loads the `uinput` kernel module, adds
`$SUDO_USER` to the `input` group, and registers the autostart-friendly
`.desktop` file. Log out and back in once for the group change to take
effect, then launch **Satellite** from your application menu (or run
`satellite` from a terminal). Uninstall with `sudo apt remove satellite`.

The CPack DEB generator is wired into the top-level `CMakeLists.txt`; the
control files and post-{install,remove} scripts live in
[`packaging/debian/`](packaging/debian/).

#### Option B — Portable build (any distro)

Build:

```bash
./build-satellite.sh
```

The same script handles macOS and Linux (it dispatches on `uname -s`). On
Linux it produces a `satellite` binary at the repo root.

Grant `/dev/uinput` access (one-time setup — the `.deb` postinstall does
this for you):

```bash
sudo modprobe uinput                                              # load the kernel module
echo 'KERNEL=="uinput", GROUP="input", MODE="0660"' \
    | sudo tee /etc/udev/rules.d/99-uinput.rules                  # persistent udev rule
sudo udevadm control --reload-rules && sudo udevadm trigger
sudo usermod -aG input "$USER"                                    # then log out / back in
```

Run:

```bash
./satellite
```

#### After install (either path)

Open `http://localhost:9877` in your browser to pair a sender and start the
receiver. Config lives at `$XDG_CONFIG_HOME/satellite/config.json` (falls
back to `~/.config/satellite/config.json`); the keyfile is named `keyfile`
in the same directory (mode `0600`). "Start with login" is wired up via the
XDG autostart spec — a `.desktop` file under `$XDG_CONFIG_HOME/autostart/`
toggled from the web UI.

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
clang-format -i src/core/*.cpp src/core/*.h src/net/*.cpp src/net/*.h src/adapters/*.cpp src/adapters/*.h src/platform/windows/*.cpp src/platform/windows/*.h
```

**Lint with clang-tidy:**

```powershell
clang-tidy src/core/*.cpp src/net/*.cpp src/adapters/*.cpp src/platform/windows/*.cpp -- -std=c++17 -Ivigem/include -Isrc -Ilib -Ilib/libsodium/libsodium-win64/include -D_WIN32_WINNT=0x0A00 -DCPPHTTPLIB_NO_EXCEPTIONS
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
│   │   ├── vigem_adapter.*     # IGamepadPort — ViGEm bus driver
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

## Security

PR-time security gates ([`.github/workflows/security.yml`](.github/workflows/security.yml)
and [`.github/workflows/codeql.yml`](.github/workflows/codeql.yml)) run on
every PR: action-pin lint, OSV-Scanner against vendored components,
gitleaks, GitHub `dependency-review-action`, allowlist-expiry check, and
CodeQL `cpp` analysis. See [`CONTRIBUTING.md`](CONTRIBUTING.md#security)
for the local-equivalent commands and the per-release verification recipe
(`cosign verify-blob` + `slsa-verifier verify-artifact`). Vulnerability
disclosure: [`SECURITY.md`](SECURITY.md).

> **Note on branch protection.** GitHub's branch-protection and repository-
> ruleset features are not available for private repositories on the free
> org plan this repo lives under, so direct pushes to `main` are not
> blocked at the platform level. Treat the PR-based flow as a convention
> and rely on the CI workflows (`linux-ci.yml`, `macos-ci.yml`,
> `windows-ci.yml`, `security.yml`, `codeql.yml`) as the quality gate.

## License

Distributed under the terms of the **GNU Lesser General Public License v3.0
or later**. See [`LICENSE`](LICENSE) (LGPL) and [`COPYING.GPL3`](COPYING.GPL3)
(the GPL v3 the LGPL incorporates by reference).

### Vendored third-party components

The following components retain their original (MIT-compatible) licenses
and are redistributed unchanged under their own terms:

- [cpp-httplib](https://github.com/yhirose/cpp-httplib) by Yuji Hirose — MIT
- ViGEm header definitions derived from [nefarius/ViGEmBus](https://github.com/nefarius/ViGEmBus) — MIT

