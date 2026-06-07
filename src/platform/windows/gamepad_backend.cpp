// SPDX-License-Identifier: LGPL-3.0-or-later
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
        status.errorCode = "DRIVER_MISSING"; // interface not enumerable
        return status;
    }

    HANDLE h = openVigemBus();
    if (h == INVALID_HANDLE_VALUE) {
        status.available = false;
        status.errorCode = "BUS_OPEN_FAILED"; // installed but in a bad state
        return status;
    }
    CloseHandle(h);

    status.available = true;
    status.errorCode = nullptr;
    return status;
}
