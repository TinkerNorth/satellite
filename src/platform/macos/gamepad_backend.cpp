// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * platform/macos/gamepad_backend.cpp — macOS stub.
 *
 * macOS has no signed virtual-gamepad bus driver shipped with this project,
 * so the probe always reports unsupported. The web UI sees supported=false
 * and hides the backend status panel entirely; the SessionService surfaces
 * ACK_ERR_BACKEND_UNAVAIL to clients attempting to add controllers.
 */
#include "core/gamepad_backend.h"

BackendStatus probeBackend() {
    BackendStatus status;
    status.id = BACKEND_ID_NONE;
    status.supported = false;
    status.available = false;
    status.errorCode = nullptr;
    return status;
}
