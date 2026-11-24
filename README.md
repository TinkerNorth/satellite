# Satellite

Satellite runs on your gaming PC and lets the Dish apps on your phone, tablet or other computer act as wireless controllers for it.

Dish is made of two parts. The Dish apps run on the device that holds your controller, or that is the controller: Dish for Android, Dish for iOS, Dish for macOS, Dish for Windows and Dish for Linux. Satellite is the small server on the PC you play on, and it makes each Dish controller look to your games like a controller plugged into that PC. You need one of each. Everything is free and open source, the website is [dish.tinkernorth.com](https://dish.tinkernorth.com), and I maintain all of it on my own.

## Download

Satellite 2.4.0 is the current release. Every link below always points at the newest one.

Windows 10 or 11 (x64): download **[SatelliteSetup.exe](https://github.com/TinkerNorth/satellite/releases/latest/download/SatelliteSetup.exe)** and run it. It installs Satellite and the drivers it needs, and from then on Satellite can update itself.

Debian and Ubuntu: add the APT repository once, and `apt upgrade` keeps Satellite current with the rest of your system.

```bash
curl -fsSL https://tinkernorth.github.io/satellite/gpg.key \
  | sudo gpg --dearmor -o /usr/share/keyrings/satellite-archive-keyring.gpg
echo "deb [signed-by=/usr/share/keyrings/satellite-archive-keyring.gpg] \
  https://tinkernorth.github.io/satellite/debian stable main" \
  | sudo tee /etc/apt/sources.list.d/satellite.list
sudo apt update && sudo apt install satellite
```

Fedora and the RHEL family: add the DNF repository once, and `dnf upgrade` keeps it current.

```bash
sudo curl -fsSL -o /etc/yum.repos.d/satellite.repo \
  https://tinkernorth.github.io/satellite/rpm/satellite.repo
sudo dnf install satellite
```

If you tried the repository with 2.3.0, dnf refused it with "The package is not signed": that package was unsigned, and from 2.4.0 the repository signs it.

If you'd rather not add a repository, [satellite-amd64.deb](https://github.com/TinkerNorth/satellite/releases/latest/download/satellite-amd64.deb) and [satellite-x86_64.rpm](https://github.com/TinkerNorth/satellite/releases/latest/download/satellite-x86_64.rpm) install the same package once (`sudo apt install ./satellite-amd64.deb` or `sudo dnf install ./satellite-x86_64.rpm`), and you install later versions by hand. With any of the packages, log out and back in once after the first install so your user can create virtual controllers.

Any other x86_64 Linux: [satellite-x86_64.AppImage](https://github.com/TinkerNorth/satellite/releases/latest/download/satellite-x86_64.AppImage) runs on glibc 2.35 or newer and can update itself. An AppImage can't install the udev rule that lets you create virtual controllers, so you grant access to `/dev/uinput` once yourself; the dashboard shows the exact commands when it's missing.

Arch Linux: add Satellite's signed pacman repository once, and `pacman -Syu` keeps it current.

```bash
curl -fsSL https://tinkernorth.github.io/satellite/gpg.key | sudo pacman-key --add -
sudo pacman-key --lsign-key 4371E67B00CB54E69ED34D70428695F4ED61D3A8
printf '\n[satellite]\nSigLevel = Required DatabaseOptional\nServer = https://tinkernorth.github.io/satellite/arch/$arch\n' \
  | sudo tee -a /etc/pacman.conf
sudo pacman -Syu satellite-bin
```

If you installed `satellite-bin` 2.3.0, it carried an AppImage that could not start; `sudo pacman -Syu` replaces it with 2.4.0, which does.

All files and older versions are on the [releases page](https://github.com/TinkerNorth/satellite/releases), and the repositories have their own page at [tinkernorth.github.io/satellite](https://tinkernorth.github.io/satellite/).

### Satellite and the Dish apps

| App | Where to get it |
|---|---|
| Satellite (Windows, Linux) | [GitHub releases](https://github.com/TinkerNorth/satellite/releases/latest) |
| Dish for Android | [Google Play](https://play.google.com/store/apps/details?id=com.tinkernorth.dish), or the APK from [GitHub releases](https://github.com/TinkerNorth/dish-android/releases/latest) |
| Dish for iOS | Not yet released |
| Dish for macOS | [GitHub releases](https://github.com/TinkerNorth/dish-mac/releases/latest) |
| Dish for Windows | [GitHub releases](https://github.com/TinkerNorth/dish-windows/releases/latest) |
| Dish for Linux | [GitHub releases](https://github.com/TinkerNorth/dish-linux/releases/latest) |

## Getting started

1. Install Satellite on the PC you play on and start it. It lives in the system tray: right-click the icon and pick Open Web UI to open the dashboard. On that PC the dashboard is also at http://localhost:9877.
2. Install a Dish app on the device with your controller. It has to be on the same local network as the PC.
3. Pair them. The dashboard shows a 4-digit PIN under Device Pairing, with a new one every five minutes. In Dish, find the PC (Dish for Android has a Scan button), pick it and type the PIN. If Dish shows you a PIN instead, the request appears under Pairing Requests on the dashboard; accept it only if the two PINs match. Satellite remembers the device, so you only pair once.
4. Play. The controller reaches your games as an Xbox 360 Controller, DualShock 4, DualSense, DualSense Edge or Switch Pro Controller, whichever you choose in Dish (in Dish for Android it's the Emulate as setting). The dashboard lists every connected controller.

## What it does

Satellite creates a virtual controller on the PC for each controller a Dish app sends, up to 16 at once, and passes back what the game sends to it. Input travels over UDP on your own network, encrypted with a key the two devices agree on when they pair.

All five controller types work on Windows and on Linux. What else reaches the game depends on the type you pick and on the PC:

| | Windows | Linux |
|---|---|---|
| Rumble | Yes | Yes |
| Motion (gyro and accelerometer) | DualShock 4, DualSense, DualSense Edge, Switch Pro | The same |
| Touchpad | DualShock 4, DualSense, DualSense Edge | The same |
| Light bar colour | DualShock 4, DualSense, DualSense Edge | No |
| Adaptive triggers | DualSense, DualSense Edge | No |
| Controller microphone and speaker | DualShock 4, DualSense, DualSense Edge | No |
| DualSense HD haptics | DualSense, DualSense Edge | No |
| Back paddles | DualSense Edge | DualSense Edge |
| Switch Capture button | Switch Pro | Switch Pro |

The back buttons of an Xbox Elite pad, a DualSense Edge or a Steam Controller travel from Dish to the PC, and a virtual DualSense Edge is the type that has back buttons to put them on. No DualSense Edge has met Satellite on Linux yet, so I haven't been able to confirm the paddles there.

Games that vibrate a DualSense through Sony's own library send that vibration as audio. Satellite forwards it as the waveform to a Dish app that can play it on a real DualSense, and turns it into ordinary rumble for every other controller. The pad's microphone and speaker show up in Windows as the controller's own sound devices, so voice chat works through the controller on the Dish side.

Each controller card on the dashboard has a Test button. It sends that controller a short rumble, a white light bar, the first player LED and a light trigger resistance, each only where the controller has it, then puts back whatever the game had set after two seconds. The card also shows the last thing the game sent to that controller and when, so between the two you can tell whether a missing rumble starts in the game or further along.

Satellite doesn't stream video. If you stream your games with Moonlight from Sunshine or Apollo, Satellite runs beside it and carries the controller. Dish for Android can also be a controller for a Sunshine, Apollo or Wolf host directly, without Satellite.

## Requirements

Windows: Windows 10 or 11, 64-bit. Installing needs administrator rights once; Satellite itself runs as your normal user. The installer puts Satellite in Program Files, two drivers that create the virtual controllers (ViGEmBus, a kernel driver, for the Xbox 360 and DualShock 4, and HIDMaestro, a user-mode driver that adds the DualSense, DualSense Edge and Switch Pro), a small service that lets Satellite create HIDMaestro controllers without asking for administrator rights each time, and firewall rules for private and domain networks. The first time an emulated controller with a microphone and speaker is created, HIDMaestro also installs its signed kernel USB transport; turning off Settings, Controller audio prevents that. The uninstaller asks separately whether to remove each driver, because other programs often share them.

Because the firewall rules leave public networks out, Dish can't find the PC while Windows treats your home network as public.

Linux: an x86_64 machine with the kernel's `uinput` module, which every mainline kernel has. The .deb is built on Debian 13, the .rpm on Fedora. The tray icon uses AppIndicator, so stock GNOME shows no icon without its AppIndicator extension; the dashboard at http://localhost:9877 works either way.

Network: the PC and the Dish device on the same local network. Satellite listens on UDP 9876 for input, TCP 9443 for pairing, UDP 5353 for discovery (mDNS) and UDP 9879 for an older discovery beacon. The dashboard on port 9877 answers only on the PC itself.

macOS: not supported. Creating a virtual controller on a Mac needs an entitlement from Apple that no Satellite build has. The release page carries a macOS build, but it is a stub: it runs the dashboard, creates no controllers, and needs Homebrew's libsodium, OpenSSL and opus installed to start.

## Trust and privacy

Every release has a `SHA256SUMS` file signed with cosign by the release workflow. To check a download on Linux, put `SHA256SUMS` beside it and run:

```bash
sha256sum --ignore-missing -c SHA256SUMS
```

On Windows, `Get-FileHash .\SatelliteSetup.exe` in PowerShell gives the hash to compare with the line in `SHA256SUMS`. [SECURITY.md](SECURITY.md) has the signature and build provenance checks, and how to report a vulnerability.

The Windows installer is not code-signed yet. I don't have a certificate, so 2.4.0 ships unsigned (the versioned copy on the release page is named `SatelliteSetup-2.4.0-unsigned.exe`), and SmartScreen will say the publisher is unknown. The checksum and its signature are how you can tell it came from this repository's release.

Satellite works on your own network and has no account, no analytics and no ads. Three things can leave your PC, and each has a switch in Settings: a daily check for a new version against GitHub, a check for a newer controller database at dish.tinkernorth.com, and, if Satellite crashes, one crash report to Sentry. Crash reports are on by default, and the Windows installer asks about them and the update check. Controller input and audio never leave your network. The diagnostics file on the Debug page is something you save and send yourself; Satellite never sends it. [PRIVACY.md](PRIVACY.md) says exactly what is stored and sent.

Satellite is licensed under the GNU Lesser General Public License v3.0 or later ([LICENSE](LICENSE), with [COPYING.GPL3](COPYING.GPL3)). The third-party components and their licences are in [THIRD_PARTY.md](THIRD_PARTY.md).

## For developers

Satellite is C++20 with CMake. The presets in `CMakePresets.json` (`release`, `debug`, `package`, and `windows-mingw`, `windows-mingw-debug` and `windows-msvc` on Windows) carry the same flags CI uses, warnings as errors included, and the scripts drive them:

```bash
scripts/install-deps.sh          # once: the toolchain CI installs
scripts/build.sh release test    # build, then run the tests
scripts/ci-local.sh              # what the main CI jobs run, in their order
```

On Windows the same three are `scripts\install-deps.ps1`, `scripts\build.ps1 release test` and `scripts\ci-local.ps1`, and `scripts\build-installer.ps1` builds the installer. `scripts/ci-local.sh --with-packages` adds the .deb and .rpm builds in containers.

Where things are written down:

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): how the server is built, the hexagonal core, the Windows drivers and the elevation story.
- [docs/contract.md](docs/contract.md): the protocol between Dish and Satellite, the single source of truth for every client.
- [docs/threading.md](docs/threading.md): the threads, the locks and the rules for changing them.
- [docs/PACKAGING.md](docs/PACKAGING.md): every Linux channel, why Flathub and the Snap Store are out, and Windows signing.
- [CHANGELOG.md](CHANGELOG.md): what changed in each release.

[CONTRIBUTING.md](CONTRIBUTING.md) covers where code goes, the code style, the tests and how a release is cut. Please read it before opening a pull request.

## Getting help

If something doesn't work, open an issue at [github.com/TinkerNorth/satellite/issues](https://github.com/TinkerNorth/satellite/issues/new/choose). The most useful thing you can attach is the diagnostics file: on the dashboard's Debug page, use Report a problem, say what happened, and press Save the file and write the email. The file leaves out pairing keys, PINs, addresses and device names; read it before you attach it, it's plain JSON. If you'd rather write to me than open an issue, the Email support@tinkernorth.com link beside that button starts a mail with the details filled in; attach the file to it yourself.

Please also say which Satellite version you run and how you installed it, your operating system, which Dish app and version you use, and which controller. If the problem is in a Dish app rather than on the PC, its own repository is the better place. For a security problem, don't open a public issue; [SECURITY.md](SECURITY.md) says how to report it privately.
