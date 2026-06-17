// SPDX-License-Identifier: LGPL-3.0-or-later
#include "core/gamepad_backend.h"
#include "core/backend_registry.h"
#include "vigem.h"

static const char* vigemVendor() {
    const satellite::BackendDescriptor* d = satellite::backendDescriptorById(BACKEND_ID_VIGEM);
    return d ? d->vendor : "";
}

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
    status.vendor = vigemVendor();
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

std::vector<satellite::BackendRuntimeStatus> enumerateBackends() {
    std::vector<satellite::BackendRuntimeStatus> out;

    BackendStatus vigem = probeBackend();
    out.push_back({BACKEND_ID_VIGEM, vigem.available,
                   vigem.errorCode ? std::string(vigem.errorCode) : std::string()});

    // HIDMaestro is a registered option; live provisioning lands with the
    // provisioner seam, so until then it probes as not-installed on this host.
    out.push_back({BACKEND_ID_HIDMAESTRO, false, "NOT_INSTALLED"});
    return out;
}
