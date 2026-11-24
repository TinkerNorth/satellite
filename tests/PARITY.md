# Platform parity

What each platform's build of Satellite does, as the tree shows it today.
Satellite speaks one wire contract ([`docs/contract.md`](../docs/contract.md))
on every platform; what differs is what the host can do with what arrives.
A row changes in the same pull request as the code that changes it.

| Capability | Windows | Linux | macOS |
|---|---|---|---|
| Virtual gamepads | ViGEmBus (Xbox 360, DualShock 4) and HIDMaestro (UMDF2 personas, the DualSense, DualSense Edge and Switch Pro among them) | `/dev/uinput` | Virtual DualShock 4 through `IOHIDUserDevice`, only in a build carrying the `com.apple.developer.hid.virtual.device` entitlement. No such build has shipped: the released `satellite-macos-stub` falls back to an inert backend and answers every controller descriptor `backendUnavailable` |
| Controller audio (the pad's microphone and speaker) | HIDMaestro's composite DualSense and DualShock 4 v2 personas, over the usbip-win2 kernel transport, behind the `controllerAudio` setting | Not available: uinput has no audio endpoint (`src/platform/linux/main.cpp`) | Not available |
| Pointer injection past higher-integrity windows (UIAccess) | Only a signed build installed under Program Files (`docs/PACKAGING.md`, "Windows: code signing and UIAccess"); releases are unsigned today | Not applicable | Not applicable |
| Tray | Yes | libayatana-appindicator when present at build time; a headless loop otherwise | Yes |
| Crash reporting (sentry-native) | Crashpad, with `crashpad_wer.dll` for fast-fail crashes, in a release build with a DSN | inproc backend, in a release build with a DSN | Compiled in only where a sentry-native package is found at configure time; the release's macOS job installs none, so the stub ships without it |
| Keyboard injection | Not implemented (`keyboardControl.supported` is false) | Not implemented | Not implemented |

## Deferred

- An entitled, signed macOS build (Apple's virtual-HID entitlement), without
  which the macOS bundle stays a stub.
- The macOS stub links Homebrew's libsodium, OpenSSL and opus from
  `/opt/homebrew` instead of bundling them, so it starts only on a Mac with
  those formulas installed.

## Third-party code built with its own warning policy

No first-party file silences a warning. Two places keep a third party's code
under its own policy, because the code is not this repository's to change:

- `CMakeLists.txt` turns `CMAKE_COMPILE_WARNING_AS_ERROR` off while
  FetchContent builds sentry-native on Linux, and restores it for
  Satellite's targets. The SDK's warnings are upstream's; warnings-as-errors
  stays on for every file this repository owns.
- `vcpkg/ports/sentry-native/fix-usage-runtime.patch` adds
  `-Wno-ignored-attributes` for the SDK's own C++ sources. The patch is
  upstream vcpkg's, copied unchanged into the overlay so the port stays
  comparable with upstream's.
- `lib/` is included as `SYSTEM`, so the vendored headers' warnings are not
  reported against Satellite's translation units; cpp-httplib and
  nlohmann/json are byte-identical to their tags (`lib/VENDORED.md`).
- clang-tidy (`scripts/check-tidy.sh`) checks every translation unit under
  `src/`, `tests/` and `tools/` with the fleet's shared `.clang-tidy`, whose
  header filter covers this repository's headers only: the vendored headers
  in `lib/` and the HIDMaestro and ViGEm SDK headers are not reported, and
  first-party glue around them is held to every check.
