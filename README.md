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

- **System tray icon** — right-click for Start/Stop, Check for Updates, Open Web UI, Exit
- **Web-based configuration** — local dashboard at `http://localhost:9877`
- **Live status** — packet count, sender IP, listening state
- **Configurable UDP port** — change via the web UI
- **Start with Windows** — optional auto-start via registry
- **In-app OTA updates** — check / download / install / restart loop hits the
  GitHub Releases API, verifies SHA-256 against `SHA256SUMS`, then hands off
  to the platform-native installer (Inno Setup on Windows, `.app` bundle swap
  on macOS, AppImage in-place replace on Linux; `.deb` users see a copyable
  `apt upgrade` command instead). See [OTA Updates](#ota-updates) below.
- **Zero dependencies** — statically linked, single exe, no DLLs needed
- **Config persistence** — settings saved to `%APPDATA%\satellite\config.json`

## Prerequisites

### Receiver machine
- **Windows 10/11** with the **[ViGEmBus driver](https://github.com/nefarius/ViGEmBus/releases)** installed (the `SatelliteSetup.exe` installer bundles ViGEmBus 1.22.0 and installs it for you if it's missing — see [Installation](#installation)), **or**
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
- Detect the **ViGEmBus** driver and (by default) install the bundled
  v1.22.0 if it's missing or older. ViGEmBus 1.22.0 is the final upstream
  release — newer installations are left untouched. The Components page
  shows the detected status before you continue.

### ViGEmBus options for unattended installs

The installer accepts a `/VIGEM=` switch alongside Inno Setup's standard
`/SILENT` / `/VERYSILENT`:

| Switch | Behavior |
|---|---|
| *(none)* / `/VIGEM=auto` | **Default.** Install the bundled ViGEmBus only if missing or older than 1.22.0. |
| `/VIGEM=bundled` | Force-run the bundled installer regardless of what's already there. |
| `/VIGEM=skip` | Don't touch the driver. Use this on locked-down machines or when ViGEmBus is managed externally. |

A reboot is sometimes required on first ViGEmBus install (MSI exit code 3010).
The Satellite installer surfaces this as a normal "Restart now / later" prompt
on the final wizard page; until you reboot, virtual-gamepad output may not
work even though the driver is installed.

### Uninstalling

Use **Settings → Apps → Installed Apps → Satellite → Uninstall**, or run
the uninstaller from the Start Menu. The uninstaller will:

- Stop `satellite.exe` if it's running.
- Remove the four Windows Firewall rules (HTTP/UDP/Pairing/Discovery).
- Delete the program files, Start Menu and Desktop shortcuts, and the
  HKCU "Run at login" registry entry.
- **Ask** whether to also uninstall the ViGEmBus driver. The default is
  **No** — other apps (DS4Windows, BetterJoy, MoonDeck-Buddy, etc.)
  commonly share it, and removing it would break them silently.

User configuration (`%APPDATA%\satellite\config.json` and the pairing
keyfile) is **left in place** so a reinstall preserves your paired
clients. Delete that folder by hand if you want a fully clean wipe.

The uninstaller accepts a `/REMOVEVIGEM=` switch alongside Inno Setup's
standard `/SILENT`:

| Switch | Behavior |
|---|---|
| *(none)* / `/REMOVEVIGEM=auto` | **Default.** Prompt the user; on `/SILENT` uninstall, do not touch the driver. |
| `/REMOVEVIGEM=yes` | Always uninstall the ViGEmBus driver as part of removing Satellite. |
| `/REMOVEVIGEM=no` | Never uninstall the ViGEmBus driver. Suppresses the prompt. |

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

There are six install paths, in order of preference for desktop users:

1. **APT repository** (Debian / Ubuntu) — `apt install satellite`, with
   automatic future upgrades. **Recommended.**
2. **Standalone `.deb`** — one-shot install from the Releases page.
3. **DNF/YUM repository** (Fedora / RHEL / openSUSE) — `dnf install
   satellite`, with automatic future upgrades.
4. **Standalone `.rpm`** — one-shot install from the Releases page.
5. **Arch Linux AUR** — `yay -S satellite-bin`.
6. **Portable build** — works anywhere, you handle udev/group setup by
   hand (one-time).

#### Option A — APT repository (Debian / Ubuntu — recommended)

If you just want satellite the same way you'd want any other system
package — updated automatically by `apt upgrade` alongside your kernel
and browser — add our hosted APT repository:

```bash
curl -fsSL https://tinkernorth.github.io/satellite/gpg.key \
  | sudo gpg --dearmor -o /usr/share/keyrings/satellite-archive-keyring.gpg
echo "deb [signed-by=/usr/share/keyrings/satellite-archive-keyring.gpg] \
  https://tinkernorth.github.io/satellite/debian stable main" \
  | sudo tee /etc/apt/sources.list.d/satellite.list
sudo apt update && sudo apt install satellite
```

Setup details (signing key fingerprint, how publishing works) are at
[`packaging/repo/README.md`](packaging/repo/README.md).

#### Option B — Standalone `.deb` (Debian / Ubuntu)

Download `satellite_<version>_amd64.deb` from the
[Releases](https://github.com/TinkerNorth/satellite/releases) page and
install it directly:

```bash
sudo apt install ./satellite_*.deb
```

…or build it yourself with the same CPack flow CI uses:

```bash
sudo apt install build-essential cmake pkg-config dpkg-dev rpm \
                 libsodium-dev libcurl4-openssl-dev \
                 libayatana-appindicator3-dev libgtk-3-dev   # optional tray
./build-deb.sh
```

The postinstall script reloads udev, loads the `uinput` kernel module, adds
`$SUDO_USER` to the `input` group, and registers the autostart-friendly
`.desktop` file. Log out and back in once for the group change to take
effect, then launch **Satellite** from your application menu (or run
`satellite` from a terminal). Uninstall with `sudo apt remove satellite`.

The CPack DEB generator is wired into the top-level `CMakeLists.txt`; the
control files and post-{install,remove} scripts live in
[`packaging/debian/`](packaging/debian/).

#### Option C — DNF/YUM repository (Fedora / RHEL / Rocky / Alma / openSUSE)

```bash
sudo curl -fsSL -o /etc/yum.repos.d/satellite.repo \
  https://tinkernorth.github.io/satellite/rpm/satellite.repo
sudo dnf install satellite
```

The `.repo` file references the same signing key as the APT repo, so
`dnf upgrade` picks up new versions automatically.

#### Option D — Standalone `.rpm`

Download `satellite-<version>-1.x86_64.rpm` from the
[Releases](https://github.com/TinkerNorth/satellite/releases) page:

```bash
sudo dnf install ./satellite-*.x86_64.rpm
```

CPack RPM generator config lives next to the DEB config in
`CMakeLists.txt`; post-{install,uninstall} scriptlets are in
[`packaging/rpm/`](packaging/rpm/).

#### Option E — Arch Linux (AUR)

The [`satellite-bin`](https://aur.archlinux.org/packages/satellite-bin)
package wraps the official AppImage:

```bash
yay -S satellite-bin
# or
paru -S satellite-bin
```

PKGBUILD source: [`packaging/aur/`](packaging/aur/).

#### Option F — Portable build (any distro)

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

After `build-satellite.bat` produces `satellite.exe`, run:

```batch
build-installer.bat
```

This fetches the bundled ViGEmBus prerequisite (verifies SHA-256 against
the pin in `redist/SHA256SUMS`) and compiles the installer with
[Inno Setup](https://jrsoftware.org/isinfo.php), producing
`dist\SatelliteSetup.exe` — a single installer that packages the app, web
UI, ViGEmBus 1.22.0 prerequisite, and uninstaller. `redist/` is
`.gitignore`d; see [`redist/README.md`](redist/README.md) for the
vendoring policy and how to bump the pin.

`build-installer.bat` is just a thin wrapper around
`scripts/fetch-redist.ps1` + `iscc installer.iss` — invoke either piece
directly if you're scripting around it (the PowerShell script accepts
`-Force` to re-download even when the existing copy matches the pin).

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

## OTA Updates

Satellite ships with a built-in update checker that hits the GitHub Releases
API on a 24-hour cadence (configurable in **Settings → Updates**). When a
newer release is published, the dashboard shows a banner with **Download**,
**Remind Me Later**, and **Skip This Version** actions. The tray menu's
*Check for Updates…* item reflects the same state and toggles to *Install
Update vX.Y.Z* once the artifact has been fetched and verified.

The full state machine — `idle → checking → update-available → downloading
→ verifying → downloaded → installing` — is described in
[`src/core/update_service.h`](src/core/update_service.h). Platform IO
(HTTPS, SHA-256, install hand-off) lives behind `IUpdaterPort`:

| Platform | HTTPS    | Artifact                       | Install method |
|---|---|---|---|
| Windows | WinHTTP | `SatelliteSetup-vX.Y.Z.exe`         | Inno Setup `/VERYSILENT /OTA` then auto-relaunch |
| macOS   | NSURLSession | `satellite-macos-stub-vX.Y.Z.zip` | `ditto -xk` unpack + atomic bundle swap via helper, `open` relaunch |
| Linux (AppImage) | libcurl | `satellite-X.Y.Z-x86_64.AppImage` | `chmod +x` + atomic mv over `$APPIMAGE`, `setsid` relaunch |
| Linux (`.deb`) | libcurl | n/a — surfaces command | `sudo apt upgrade satellite` (auto-pulled from our APT repo if added) |
| Linux (`.rpm`) | libcurl | n/a — surfaces command | `sudo dnf upgrade --refresh satellite` (auto-pulled from our DNF repo) |
| Linux (AUR)    | libcurl | n/a — surfaces command | `yay -Syu satellite-bin` |
| Linux (portable) | libcurl | n/a — surfaces command | Manual: `chmod +x` and replace the binary |

Verification is SHA-256 against the `SHA256SUMS` asset published in every
release (signed by cosign at release time — verifying the cosign signature
in-app is tracked as a follow-up; transit is HTTPS-pinned to `github.com`).

### Settings (web UI: `/settings` → Updates section)

- **Channel** — *Stable* (only `vX.Y.Z` releases) or *Pre-release*
  (includes `vX.Y.Z-rc.1`, `-beta.2`, etc.).
- **Check automatically** — runs the 24-hour timer thread (skip if disabled).
- **Download in background** — auto-download an available update so it's
  ready to install on the next visit.
- **Install & restart automatically** — only effective with the above two
  enabled; turns the dashboard banner into a direct restart prompt.

### API

| Method | Path                       | Purpose                                        |
|---|---|---|
| GET    | `/api/version`             | `{version, platformId}`, unauthenticated       |
| GET    | `/api/updates/status`      | Full `UpdateStatusSnapshot` JSON               |
| POST   | `/api/updates/check`       | Trigger a check now                            |
| POST   | `/api/updates/download`    | Fetch the artifact                             |
| POST   | `/api/updates/install`     | Apply the downloaded artifact (will restart)   |
| POST   | `/api/updates/cancel`      | Cancel an in-flight download                   |
| POST   | `/api/updates/dismiss`     | "Remind me later" — hides the banner           |
| POST   | `/api/updates/skip`        | `{version}` — never notify about this version again |
| POST   | `/api/updates/preferences` | `{channel, autoCheck, autoDownload, autoInstall}` |

The existing `/api/events` SSE stream gained an `update` event channel that
pushes the same snapshot every tick, so the web UI stays current without
polling.

### Bumping the version

Edit two files in lockstep:

1. [`/VERSION`](VERSION) — consumed by CMake (`MACOSX_BUNDLE_*`, `CPACK_PACKAGE_VERSION`)
   and by `installer.iss` at preprocess time.
2. [`src/core/version.h`](src/core/version.h) — consumed by C++ and by
   `satellite.rc` (through `windres`'s preprocessor).

The `version-consistency` CI gate fails if the two diverge.

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

