// SPDX-License-Identifier: LGPL-3.0-or-later
// Entitled (production) builds publish virtual DualShock 4 pads through
// IOHIDUserDevice and report the `machid` backend. Unentitled processes (dev
// machines, CI runners) and SDKs without the header fall back to exactly the
// historical stub values: id `none`, unsupported, unavailable — the web UI
// hides the backend panel and controller descriptors apply as
// backendUnavailable, byte-identical to the pre-backend macOS build.
#include "core/gamepad_backend.h"

#include "mac_hid_gamepad_adapter.h"

BackendStatus probeBackend() {
    return macHidBackendStatus(MacHidGamepadAdapter::runtimeAvailable());
}
