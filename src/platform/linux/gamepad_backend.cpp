// SPDX-License-Identifier: LGPL-3.0-or-later
// "module not loaded" and "not built into the kernel" both surface as
// DEVICE_MISSING; the web UI's `modprobe uinput` copy covers both.
#include "core/gamepad_backend.h"

#include <sys/stat.h>
#include <unistd.h>

BackendStatus probeBackend() {
    BackendStatus status;
    status.id = BACKEND_ID_UINPUT;
    status.supported = true;

    struct stat st;
    if (::stat("/dev/uinput", &st) != 0) {
        status.available = false;
        status.errorCode = "DEVICE_MISSING";
        return status;
    }

    if (::access("/dev/uinput", W_OK) != 0) {
        status.available = false;
        status.errorCode = "PERMISSION_DENIED";
        return status;
    }

    status.available = true;
    status.errorCode = nullptr;
    return status;
}
