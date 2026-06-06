// SPDX-License-Identifier: LGPL-3.0-or-later
// Stub: no virtual-gamepad bus driver ships for macOS, so the probe always
// reports unsupported. The web UI hides the backend panel on supported=false;
// SessionService surfaces ACK_ERR_BACKEND_UNAVAIL to add-controller requests.
#include "core/gamepad_backend.h"

BackendStatus probeBackend() {
    BackendStatus status;
    status.id = BACKEND_ID_NONE;
    status.supported = false;
    status.available = false;
    status.errorCode = nullptr;
    return status;
}
