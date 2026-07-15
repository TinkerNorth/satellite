// SPDX-License-Identifier: LGPL-3.0-or-later

// No user-facing copy here, only stable identifiers. The web UI keys its
// copy/remediation table off (id, errorCode); each platform owns its impl in
// src/platform/<os>/gamepad_backend.cpp.
#pragma once

// Stable identifiers exposed in the JSON API. Web UI matches on these.
inline const char* BACKEND_ID_VIGEM = "vigem";    // Windows / ViGEmBus
inline const char* BACKEND_ID_UINPUT = "uinput";  // Linux / /dev/uinput
inline const char* BACKEND_ID_MAC_HID = "machid"; // macOS / IOHIDUserDevice (entitled)
inline const char* BACKEND_ID_NONE = "none";      // unentitled macOS / unsupported

// Per-backend error codes. Null means the backend is available.
//
//   vigem:   "DRIVER_MISSING", "BUS_OPEN_FAILED"
//   uinput:  "MODULE_NOT_LOADED", "DEVICE_MISSING", "PERMISSION_DENIED"
//   machid:  no codes (an unentitled probe reports the `none` stub instead)
//   none:    no codes (the panel is hidden when supported == false)
//
// Adding a code is a server change only; the web UI falls back to a generic
// message for a code it doesn't recognize.

struct BackendStatus {
    const char* id = BACKEND_ID_NONE; // one of BACKEND_ID_*
    bool supported = false;           // false: web hides the backend panel
    bool available = false;           // can the backend accept controllers now?
    const char* errorCode = nullptr;  // null when available, else a per-backend tag
};

// Side-effect-free; safe to call from any thread and from per-request handlers.
BackendStatus probeBackend();

// Which TOUCHPAD_MODE_* values this server can honour. Clients query via GET
// /api/server/capabilities so their mode-picker disables modes the host can't
// deliver (macOS has no gamepad bus, so Pad and Mouse both no-op).
//   Pad:   needs a backend carrying a DS4 touchpad surface.
//   Mouse: needs host-global pointer injection.
//   Off:   always true; the receiver just drops the samples.
struct TouchpadCapabilities {
    bool padSupported = false;
    bool mouseSupported = false;
    bool offSupported = true;
};

// Inline + parameterised so tests can pin behaviour without the platform
// probeBackend(). Pad+mouse are tied to having any gamepad backend:
// ViGEm/uinput ship both; macOS ships neither.
inline TouchpadCapabilities deriveTouchpadCapabilities(const BackendStatus& s) {
    TouchpadCapabilities caps;
    caps.padSupported = s.supported;
    caps.mouseSupported = s.supported;
    caps.offSupported = true;
    return caps;
}

// A driver-installed-but-bus-down state still advertises support; the client
// UI shouldn't change just because the bus momentarily failed.
inline TouchpadCapabilities probeTouchpadCapabilities() {
    return deriveTouchpadCapabilities(probeBackend());
}
