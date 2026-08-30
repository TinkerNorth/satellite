// SPDX-License-Identifier: LGPL-3.0-or-later
#include "core/gamepad_backend.h"
#include "hidmaestro_helper_client.h"
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

static BackendStatus probeViGEm() {
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
        status.errorCode = "BUS_OPEN_FAILED"; // installed but in a bad state
        return status;
    }
    CloseHandle(h);

    status.available = true;
    status.errorCode = nullptr;
    return status;
}

// Passive footprint checks only — the elevated helper (and its UAC prompt)
// must never launch from a status probe.
static BackendStatus probeHidMaestro() {
    BackendStatus status;
    status.id = BACKEND_ID_HIDMAESTRO;
    status.supported = true;

    if (!satellite::hidmaestro::helperBinaryPresent()) {
        status.available = false;
        status.errorCode = "HELPER_MISSING";
        return status;
    }
    if (!satellite::hidmaestro::driverInstalled()) {
        status.available = false;
        status.errorCode = "DRIVER_MISSING";
        return status;
    }

    status.available = true;
    status.errorCode = nullptr;
    return status;
}

// The singular legacy status: the preferred backend that is live right now,
// else the most-preferred backend's error so the panel shows the remediation
// for the backend the user is most likely to want fixed.
BackendStatus probeBackend() {
    BackendStatus vigem = probeViGEm();
    if (vigem.available) return vigem;
    BackendStatus hm = probeHidMaestro();
    if (hm.available) return hm;
    return vigem;
}

std::vector<satellite::BackendRuntimeStatus> enumerateBackends() {
    std::vector<satellite::BackendRuntimeStatus> out;
    BackendStatus vigem = probeViGEm();
    out.push_back({BACKEND_ID_VIGEM, vigem.available,
                   vigem.errorCode ? std::string(vigem.errorCode) : std::string()});
    BackendStatus hm = probeHidMaestro();
    out.push_back({BACKEND_ID_HIDMAESTRO, hm.available,
                   hm.errorCode ? std::string(hm.errorCode) : std::string()});
    return out;
}
