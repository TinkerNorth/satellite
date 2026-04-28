// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * platform/windows/gamepad_backend.cpp — ViGEmBus probe.
 *
 * Reports backend status as one of:
 *   available         — bus can be opened (driver installed and responsive)
 *   DRIVER_MISSING    — could not enumerate the ViGEm device interface
 *   BUS_OPEN_FAILED   — interface present, but CreateFile/version check failed
 *                       (driver installed but in a bad state)
 */
#include "core/gamepad_backend.h"
#include "vigem.h"

static bool isViGEmDeviceInterfacePresent() {
    HDEVINFO devInfo = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_BUSENUM_VIGEM, nullptr, nullptr,
                                            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) return false;
    SP_DEVICE_INTERFACE_DATA did{};
    did.cbSize = sizeof(did);
    bool found = SetupDiEnumDeviceInterfaces(devInfo, nullptr, &GUID_DEVINTERFACE_BUSENUM_VIGEM, 0,
                                             &did) != 0;
    SetupDiDestroyDeviceInfoList(devInfo);
    return found;
}

BackendStatus probeBackend() {
    BackendStatus status;
    status.id = BACKEND_ID_VIGEM;
    status.supported = true;

    if (!isViGEmDeviceInterfacePresent()) {
        status.available = false;
        status.errorCode = "DRIVER_MISSING";
        return status;
    }

    HANDLE h = openVigemBus();
    if (h == INVALID_HANDLE_VALUE) {
        status.available = false;
        status.errorCode = "BUS_OPEN_FAILED";
        return status;
    }
    CloseHandle(h);

    status.available = true;
    status.errorCode = nullptr;
    return status;
}
