# Satellite

Low-latency Xbox controller forwarding over the network. Captures physical controller input on one machine and injects it as a virtual Xbox 360 controller on another — similar to how [Moonlight](https://github.com/moonlight-stream/moonlight-qt) / [Sunshine](https://github.com/LizardByte/Sunshine) handle input, but as a standalone tool.

Runs as a **system tray application** with a built-in **web UI** for configuration — no console window required.

## How it works

```
┌──────────────┐        UDP (12 bytes)       ┌──────────────────┐
│    Sender     │ ─────────────────────────►  │     Receiver     │
│  (XInput)     │    XUSB_REPORT packet       │  (ViGEmBus)      │
│               │                             │                  │
│ Physical Xbox │ ◄─────────────────────────  │ Virtual Xbox 360 │
│  controller   │     MSG_RUMBLE (7 bytes)    │   controller     │
└──────────────┘                              └──────────────────┘
```

**Sender** polls a physical Xbox controller via XInput at ~250 Hz and streams 12-byte `XUSB_REPORT` packets over UDP.

**Receiver** runs as a system tray app. It listens for those packets and injects them into Windows as a virtual Xbox 360 controller through the ViGEmBus kernel driver — no DLLs required, communicates directly via `DeviceIoControl`.

The hot path is three syscalls with zero allocations: `recvfrom()` → `memcpy()` → `DeviceIoControl()`.

The **return path** carries rumble events the other direction: when a game on the receiver host calls `XInputSetState` (or the equivalent on Linux's evdev FF subsystem), the platform backend fires a notification, the receiver maps it to the originating dish session, and forwards a `MSG_RUMBLE` packet back over the encrypted UDP channel. See [Rumble (return path)](#rumble-return-path) below.

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
- **macOS** for development / web-UI testing only — controller injection is unavailable, so controller descriptors apply as `backendUnavailable`

### Client (sender) device
- **dish-android** — an Android phone running the Dish app (Bluetooth/USB controllers, touch overlay, motion). Other Dish clients implement the same contract ([`docs/contract.md`](docs/contract.md)).

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
- Remove the Windows Firewall rules (UDP input stream 9876, HTTPS client
  API 9443, discovery 9879 — the admin UI on 9877 is loopback-only and
  needs no rule).
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
but applies every controller descriptor as `backendUnavailable`. This is
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

Run `satellite.exe`. It starts minimized to the system tray and begins
listening immediately — there is no start/stop button.

- **Right-click** the tray icon → **Open Web UI** for the dashboard
- Or open `http://localhost:9877` in your browser
- Generate a PIN on the dashboard (or approve the client-shown PIN) to pair
  a Dish client; clients then connect on their own
- Enable **Start with Windows** to auto-launch on boot

Clients (dish-android first) discover the satellite via mDNS, pair over
HTTPS 9443, and stream input over UDP 9876 — the full client ↔ server
contract is [`docs/contract.md`](docs/contract.md).

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

The receiver runs four main threads:

| Thread       | Role                                                        |
|--------------|-------------------------------------------------------------|
| **Main**     | Win32 message loop, system tray icon                        |
| **Receiver** | UDP socket → ViGEmBus `DeviceIoControl` (the hot path); spawns the reaper |
| **Client API** | HTTPS 9443 ([cpp-httplib](https://github.com/yhirose/cpp-httplib)) — pairing, sessions, catalog |
| **Admin HTTP** | Loopback 9877 — web UI, admin API, SSE                    |

## Why UDP?

TCP's reliability guarantees cause **head-of-line blocking** — if one packet is lost, all subsequent packets are held until retransmission completes. For controller input, only the latest state matters. A lost packet is better than a delayed one. This is the same approach used by Moonlight, Parsec, and Steam Remote Play.

## Rumble (return path)

Games drive controller vibration by writing to the virtual gamepad device's
output channel. Satellite snapshots those writes from the platform backend
and forwards them to the dish that owns the session, which then actuates
the matching physical controller (or, on dish-android, the phone itself).

### How each backend listens

| Platform | Source | Mechanism |
|---|---|---|
| Windows | ViGEmBus | `IOCTL_XUSB_REQUEST_NOTIFICATION` (X360) / `IOCTL_DS4_REQUEST_NOTIFICATION` (DS4) — long-running async I/O. One worker thread per plugged virtual device blocks on the IOCTL until the driver completes it with the new motor/lightbar values. |
| Linux | uinput | `EV_FF` events on the device fd. Game uploads an `FF_RUMBLE` effect via `UI_FF_UPLOAD`, kernel hands us the descriptor, then sends an `EV_FF` event when the game presses play/stop. One reader thread per plugged device. |
| macOS | (none) | No virtual gamepad backend → no rumble events to forward. The macOS `IGamepadPort` accepts the callback registration so the SessionService composes uniformly, but it's never invoked. |

### Wire format

`MSG_RUMBLE = 0x0009`, satellite → dish, encrypted in the same ChaCha20-Poly1305 envelope as every other message:

```
inner header                           inner payload (7 bytes)
┌──────────┬──────────┐  ┌──────────┬─────────────┬─────────────┬──────────┐
│ msgType  │ payload  │  │ ctrlIdx  │  strongMag  │   weakMag   │  durMs   │
│  0x0009  │  length  │  │   u8     │   u16 BE    │   u16 BE    │  u16 BE  │
└──────────┴──────────┘  └──────────┴─────────────┴─────────────┴──────────┘
   2 bytes    2 bytes        1            2             2            2
```

| Field | Meaning |
|---|---|
| `ctrlIdx` | Controller index within the dish session that owns this rumble. |
| `strongMag` | Low-frequency / large-motor magnitude. 0..65535, XInput scale. |
| `weakMag` | High-frequency / small-motor magnitude. 0..65535. |
| `durMs` | Wire-side refresh deadline. Stamped by the satellite (default 500 ms); the dish clamps before driving its actuator. `0` is a stop sentinel. |

Rumble carries motor vibration only — the lightbar LED is a separate message
([Lightbar return path](#lightbar-return-path)).

The producer side lives in [`src/adapters/client_adapter.cpp`](src/adapters/client_adapter.cpp); the dish-side parsers (`SatelliteClient::parseRumbleMessage` in dish-linux/windows, `SatelliteClient.parseRumblePayload` in dish-mac, the JNI dispatch in `RumbleBridge.dispatchRumble` on dish-android) all consume this layout verbatim.

### Coalescing

Games can spam the same magnitudes 60+ Hz across many frames. The
`SessionService::handleRumbleFromBackend` path keeps the most recent
`(strong, weak)` per controller and suppresses re-emits when nothing changed.
Duration-only changes never defeat the suppression — the wire-side refresh
deadline is a knob the satellite owns, not a player-perceptible value.

### Per-dish actuator behaviour

| Dish | Actuator | Notes |
|---|---|---|
| dish-windows | `SDL_GameControllerRumble` | Magnitudes pass through verbatim (XInput scale matches). Vibration only — the lightbar LED is a decoupled path as of Task 1.4. |
| dish-linux | Same SDL2 entry point | Underlying call is evdev `EVIOCSFF`. Works for any pad SDL recognises. |
| dish-mac | `GCController.haptics` + `CHHapticEngine` per locator | Magnitudes mapped to `CHHapticIntensity` 0..1. Engines kept started; per-call `CHHapticPattern` for sustained drive. Falls back to no-op on legacy MFi pads with no haptics surface. Vibration only — lightbar is decoupled. |
| dish-android | `VibratorManager` (API 31+) / `Vibrator` (legacy) — phone body | All rumble routed to the device's own actuator(s). On dual-actuator phones the strong motor goes to vibrator id 0 and weak to id 1; on single-actuator phones the peak of the two is used. **No fallback to physical-controller actuators by design** — see `RumbleBridge.kt`. |

## Lightbar (return path)

Task 1.4. A game can drive a DualSense / DualShock 4's RGB lightbar
independently of vibration — plenty of games set a colour and never rumble.
Satellite forwards those colour changes on a dedicated stream, decoupled from
`MSG_RUMBLE`.

### Capability gate

A dish advertises `lightbar` in its controller descriptor's `caps` object
(the session PUT — see [`docs/contract.md`](docs/contract.md)) when the bound
physical controller has an addressable RGB LED (a DualSense / DS4 — not an
Xbox pad, and never on dish-android, which has no controller-LED API). The
satellite emits `MSG_LIGHTBAR` **only** to a controller that advertised the
capability; a dish that did not simply receives no lightbar traffic.

### Wire format

`MSG_LIGHTBAR = 0x000D`, satellite → dish, in the same ChaCha20-Poly1305
envelope as every other message:

```
inner header                           inner payload (4 bytes)
┌──────────┬──────────┐  ┌──────────┬──────────┬──────────┬──────────┐
│ msgType  │ payload  │  │ ctrlIdx  │    R     │    G     │    B     │
│  0x000D  │  length  │  │   u8     │   u8     │   u8     │   u8     │
└──────────┴──────────┘  └──────────┴──────────┴──────────┴──────────┘
   2 bytes    2 bytes        1           1          1          1
```

### How it is produced

On Windows the ViGEm DS4 driver delivers rumble and lightbar in the *same*
`IOCTL_DS4_REQUEST_NOTIFICATION` completion. `vigem_adapter`'s notification
worker fans that one event out to two sinks — the rumble callback (motors) and
the lightbar callback (colour). `SessionService::handleLightbarFromBackend`
resolves the backend serial → `(connection, ctrlIdx)`, coalesces identical
back-to-back colours, and — for a `CAP_LIGHTBAR` dish — calls
`IClientPort::sendLightbar`. The colour is cached on the `Controller`
regardless of the capability, so the web dashboard's per-controller lightbar
swatch stays live for every controller type.

The Linux uinput and (inert) macOS backends have no host-driven lightbar
channel, so they never emit `MSG_LIGHTBAR` — lightbar emission is a
Windows / ViGEm-DS4 feature today.

### Per-dish actuator behaviour

| Dish | Actuator | Notes |
|---|---|---|
| dish-windows / dish-linux | `SDL_GameControllerSetLED` | Marshalled onto the SDL thread; a no-op on a pad SDL reports no LED for. |
| dish-mac | `GCDeviceLight.color` (`GCColor`) | Applied on the main actor. |
| dish-android | — | No controller-LED API: an arriving `MSG_LIGHTBAR` is logged and dropped, and `CAP_LIGHTBAR` is never advertised. |

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

Unit tests use mock implementations of the port interfaces; no external test
framework is required. CMake registers every suite with CTest:

```powershell
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

### Suites

| Suite | Covers |
|---|---|
| `session_service` | Declarative session upsert/converge, transactional replug, serial round-robin + quarantine, epoch/bitmap, close-notify ordering, liveness grace + reap, host-feature grants, data streams |
| `receiver` | Inner-message length guards, wire decode, deleted-opcode drop |
| `test_catalog` | Catalog JSON, Accept-Language resolution, ETag, locale completeness gate |
| `test_json_mini` | Request-body JSON extraction helpers |
| `pairing` | Path-B pairing request registry lifecycle |
| `mdns_protocol` / `test_discovery` / `test_machine_id` | Discovery + identity |
| `test_codecs` / `test_ipv4_util` | Pure codecs (touchpad pack, IPv4 fast path) |
| `test_github_release` / `test_update_service` | OTA updater |
| `windows_platform` | Config persistence, autostart, hex/JSON helpers, HKDF + hmacProof + packet-AEAD vectors |
| `vigem_adapter` | Synchronous submit policy, DS4 EX fallback, observable unplug |
| `linux_platform` (Linux only) | Linux config/adapters |

## Project structure

```
├── src/
│   ├── core/                   # Pure domain logic (no platform deps)
│   │   ├── types.h             # Data structs, protocol constants, enums
│   │   ├── ports.h             # Outbound port interfaces (hexagonal)
│   │   ├── session_service.*   # Declarative session/controller converge
│   │   ├── catalog.*           # /api/catalog builder + locale resolution
│   │   ├── json_mini.h         # Dependency-free JSON extraction helpers
│   │   └── update_service.*    # OTA update state machine
│   ├── net/                    # Shared transport layer (all platforms)
│   │   ├── receiver.cpp        # UDP recv/decrypt hot loop
│   │   ├── inner_dispatch.cpp  # Inner-message parser + length guards
│   │   ├── webserver.cpp       # HTTPS client API + loopback admin UI + SSE
│   │   ├── session_crypto.*    # HKDF session keys, hmacProof, packet AEAD
│   │   ├── pairing*.{h,cpp}    # PIN pairing + Path-B request registry
│   │   └── discovery / mdns_*  # mDNS responder + legacy beacon
│   ├── adapters/               # Portable adapters
│   │   ├── client_adapter.*    # IClientPort — encrypted UDP downstream
│   │   └── log_adapter.*       # ILogPort — ring buffer logger
│   └── platform/
│       ├── windows/            # main, tray, config, ViGEm adapter (vigem*.{h,cpp})
│       ├── linux/              # main, tray, config, uinput adapter
│       └── macos/              # main, tray, config, inert gamepad stub
├── tests/                      # Self-contained suites (no framework), one per area
├── docs/
│   ├── contract.md             # THE client ↔ server protocol contract
│   └── architecture.md         # Server internals
├── lib/                        # cpp-httplib + libsodium (vendored)
├── vigem/include/ViGEm/        # ViGEm driver headers
├── web/                        # Admin web UI + lang catalogs + catalog images
├── .clang-format               # Code formatting rules
├── .clang-tidy                 # Linting checks
├── LICENSE                     # LGPL-3.0-or-later
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

