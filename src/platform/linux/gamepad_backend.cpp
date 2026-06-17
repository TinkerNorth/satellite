// SPDX-License-Identifier: LGPL-3.0-or-later
// Linux uinput probe. "module not loaded" vs "not built into the kernel" both
// surface as DEVICE_MISSING — the web UI's `modprobe uinput` copy covers both.
#include "core/gamepad_backend.h"
#include "core/backend_registry.h"

#include <sys/stat.h>
#include <unistd.h>

BackendStatus probeBackend() {
    BackendStatus status;
    status.id = BACKEND_ID_UINPUT;
    const satellite::BackendDescriptor* d = satellite::backendDescriptorById(BACKEND_ID_UINPUT);
    status.vendor = d ? d->vendor : "";
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

std::vector<satellite::BackendRuntimeStatus> enumerateBackends() {
    BackendStatus s = probeBackend();
    return {
        {BACKEND_ID_UINPUT, s.available, s.errorCode ? std::string(s.errorCode) : std::string()}};
}
