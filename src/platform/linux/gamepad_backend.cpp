// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * platform/linux/gamepad_backend.cpp — Linux uinput probe.
 *
 * Reports backend status as one of:
 *   available           — /dev/uinput exists and the current user can write to it
 *   PERMISSION_DENIED   — /dev/uinput exists but is not writable
 *   DEVICE_MISSING      — /dev/uinput is absent (uinput module likely not loaded)
 *
 * Note: we don't try to distinguish "module exists but not loaded" from
 * "module not built into the kernel" — both surface as DEVICE_MISSING. The
 * web UI's remediation copy covers `modprobe uinput` either way.
 */
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
