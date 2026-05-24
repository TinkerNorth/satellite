// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * core/gamepad_backend.h — Cross-platform virtual-gamepad backend probe.
 *
 * The single C++ surface the rest of the project (SessionService, webserver,
 * web UI) uses to ask "what backend is this OS using, and can it accept
 * controllers right now?"
 *
 * No user-facing copy lives here — only stable identifiers. The web UI keys
 * its own copy/remediation table off (id, errorCode); each platform owns
 * its impl in src/platform/<os>/gamepad_backend.cpp.
 */
#pragma once

// Stable identifiers exposed in the JSON API. Web UI matches on these.
inline const char* BACKEND_ID_VIGEM = "vigem";   // Windows / ViGEmBus
inline const char* BACKEND_ID_UINPUT = "uinput"; // Linux / /dev/uinput
inline const char* BACKEND_ID_NONE = "none";     // macOS / unsupported

// Per-backend error codes. Null means the backend is available.
//
//   vigem:   "DRIVER_MISSING", "BUS_OPEN_FAILED"
//   uinput:  "MODULE_NOT_LOADED", "DEVICE_MISSING", "PERMISSION_DENIED"
//   none:    no codes (the panel is hidden when supported == false)
//
// Adding a new code is a server change only — the web UI falls back to a
// generic message when it sees a code it doesn't recognize.

struct BackendStatus {
    const char* id = BACKEND_ID_NONE; // one of BACKEND_ID_*
    bool supported = false;           // false → web hides the backend panel
    bool available = false;           // can the backend accept controllers right now?
    const char* errorCode = nullptr;  // null when available, else a per-backend tag
};

// Side-effect-free probe. Safe to call from any thread; cheap enough to call
// from per-request handlers.
BackendStatus probeBackend();

// ── Touchpad-mode capabilities (server-side advertisement) ──────────────────
// Which TOUCHPAD_MODE_* values this server can actually honour. Clients query
// this via GET /api/server/capabilities at session open so their mode-picker UI
// disables modes the host can't deliver (e.g. macOS has no virtual gamepad bus
// → Pad and Mouse both no-op there). `off` is always supported (no work).
//
// Pad   — depends on a backend that can carry a DS4 touchpad surface
//         (ViGEm DS4 on Windows, multitouch uinput on Linux).
// Mouse — depends on a host-global pointer-injection API
//         (SendInput on Windows, uinput EV_REL on Linux).
// Off   — always true; the receiver just drops the samples.
struct TouchpadCapabilities {
    bool padSupported = false;
    bool mouseSupported = false;
    bool offSupported = true;
};

// Pure derivation from a BackendStatus — kept inline + parameterised so
// tests can pin behaviour without linking platform-specific probeBackend().
// Hard-ties pad+mouse to whether the host has any virtual-gamepad backend
// at all: ViGEm (Windows) / uinput (Linux) ship both; macOS ships neither.
inline TouchpadCapabilities deriveTouchpadCapabilities(const BackendStatus& s) {
    TouchpadCapabilities caps;
    caps.padSupported = s.supported;
    caps.mouseSupported = s.supported;
    caps.offSupported = true;
    return caps;
}

// Side-effect-free probe. Derived from the backend status so a Mac
// (BACKEND_ID_NONE) reports only `off`, while a Windows/Linux host with the
// backend healthy reports all three. A driver-installed-but-bus-down state
// still advertises support — the client UI shouldn't change just because
// the bus has momentarily failed.
inline TouchpadCapabilities probeTouchpadCapabilities() {
    return deriveTouchpadCapabilities(probeBackend());
}
