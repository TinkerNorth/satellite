// SPDX-License-Identifier: LGPL-3.0-or-later
#include "core/gamepad_backend.h"
#include "driver_pins.h"
#include "hidmaestro_helper_client.h"
#include "vigem.h"

#include <cfgmgr32.h>

static std::string vigemBusDriverFilePath() {
    wchar_t sysDir[MAX_PATH];
    const UINT n = GetSystemDirectoryW(sysDir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return "";
    std::wstring w(sysDir, n);
    w += L"\\drivers\\ViGEmBus.sys";
    const int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string out(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
}

static std::string installedVigemBusVersion() {
    const std::string path = vigemBusDriverFilePath();
    if (path.empty()) return "";
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeA(path.c_str(), &handle);
    if (size == 0) return "";
    std::vector<unsigned char> buf(size);
    if (!GetFileVersionInfoA(path.c_str(), 0, size, buf.data())) return "";
    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT ffiLen = 0;
    if (!VerQueryValueA(buf.data(), "\\", reinterpret_cast<LPVOID*>(&ffi), &ffiLen) ||
        ffi == nullptr || ffiLen < sizeof(VS_FIXEDFILEINFO)) {
        return "";
    }
    char v[48];
    std::snprintf(v, sizeof(v), "%u.%u.%u.%u", static_cast<unsigned>(HIWORD(ffi->dwFileVersionMS)),
                  static_cast<unsigned>(LOWORD(ffi->dwFileVersionMS)),
                  static_cast<unsigned>(HIWORD(ffi->dwFileVersionLS)),
                  static_cast<unsigned>(LOWORD(ffi->dwFileVersionLS)));
    return v;
}

static bool vigemBusRestartPending() {
    HDEVINFO devInfo = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_BUSENUM_VIGEM, nullptr, nullptr,
                                            DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) return false;
    bool pending = false;
    SP_DEVINFO_DATA dev{};
    dev.cbSize = sizeof(dev);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(devInfo, i, &dev); ++i) {
        ULONG status = 0, problem = 0;
        if (CM_Get_DevNode_Status(&status, &problem, dev.DevInst, 0) != CR_SUCCESS) continue;
        if ((status & DN_NEED_RESTART) != 0 || problem == CM_PROB_NEED_RESTART) {
            pending = true;
            break;
        }
    }
    SetupDiDestroyDeviceInfoList(devInfo);
    return pending;
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
                   vigem.errorCode ? std::string(vigem.errorCode) : std::string(),
                   installedVigemBusVersion(), SATELLITE_VIGEMBUS_BUNDLED_VERSION,
                   vigemBusRestartPending()});
    BackendStatus hm = probeHidMaestro();
    out.push_back({BACKEND_ID_HIDMAESTRO, hm.available,
                   hm.errorCode ? std::string(hm.errorCode) : std::string(),
                   satellite::hidmaestro::installedDriverVersion(),
                   SATELLITE_HIDMAESTRO_BUNDLED_DRIVER_VERSION, false});
    return out;
}
