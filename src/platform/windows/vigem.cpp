// SPDX-License-Identifier: LGPL-3.0-or-later
#include "vigem.h"

#include "vigem_submit_policy.h"

#include <cstring>
#include <type_traits>

HANDLE openVigemBus() {
    SP_DEVICE_INTERFACE_DATA did{};
    did.cbSize = sizeof(did);
    DWORD idx = 0, reqSize = 0;
    HDEVINFO devInfo = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_BUSENUM_VIGEM, nullptr, nullptr,
                                            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

    while (SetupDiEnumDeviceInterfaces(devInfo, nullptr, &GUID_DEVINTERFACE_BUSENUM_VIGEM, idx++,
                                       &did)) {
        SetupDiGetDeviceInterfaceDetailW(devInfo, &did, nullptr, 0, &reqSize, nullptr);
        auto* detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)malloc(reqSize);
        if (!detail) continue;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (SetupDiGetDeviceInterfaceDetailW(devInfo, &did, detail, reqSize, nullptr, nullptr)) {
            HANDLE h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
            free(detail);
            if (h != INVALID_HANDLE_VALUE) {
                OVERLAPPED ov{};
                ov.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                VIGEM_CHECK_VERSION ver;
                VIGEM_CHECK_VERSION_INIT(&ver, VIGEM_COMMON_VERSION);
                DWORD xfr = 0;
                DeviceIoControl(h, IOCTL_VIGEM_CHECK_VERSION, &ver, ver.Size, nullptr, 0, &xfr,
                                &ov);
                if (GetOverlappedResult(h, &ov, &xfr, TRUE)) {
                    CloseHandle(ov.hEvent);
                    SetupDiDestroyDeviceInfoList(devInfo);
                    return h;
                }
                CloseHandle(ov.hEvent);
                CloseHandle(h);
            }
        } else {
            free(detail);
        }
    }
    SetupDiDestroyDeviceInfoList(devInfo);
    return INVALID_HANDLE_VALUE;
}

bool pluginTarget(HANDLE bus, ULONG serial) {
    OVERLAPPED ov{};
    ov.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    DWORD xfr = 0;
    VIGEM_PLUGIN_TARGET plug;
    VIGEM_PLUGIN_TARGET_INIT(&plug, serial, Xbox360Wired);
    plug.VendorId = 0x045E;
    plug.ProductId = 0x028E;
    DeviceIoControl(bus, IOCTL_VIGEM_PLUGIN_TARGET, &plug, plug.Size, nullptr, 0, &xfr, &ov);
    bool ok = GetOverlappedResult(bus, &ov, &xfr, TRUE) != 0;
    if (ok) {
        VIGEM_WAIT_DEVICE_READY wr;
        VIGEM_WAIT_DEVICE_READY_INIT(&wr, serial);
        OVERLAPPED ov2{};
        ov2.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        DeviceIoControl(bus, IOCTL_VIGEM_WAIT_DEVICE_READY, &wr, wr.Size, nullptr, 0, &xfr, &ov2);
        GetOverlappedResult(bus, &ov2, &xfr, TRUE);
        CloseHandle(ov2.hEvent);
    }
    CloseHandle(ov.hEvent);
    return ok;
}

bool pluginTargetDS4(HANDLE bus, ULONG serial) {
    OVERLAPPED ov{};
    ov.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    DWORD xfr = 0;
    VIGEM_PLUGIN_TARGET plug;
    VIGEM_PLUGIN_TARGET_INIT(&plug, serial, DualShock4Wired);
    plug.VendorId = 0x054C;
    plug.ProductId = 0x05C4;
    DeviceIoControl(bus, IOCTL_VIGEM_PLUGIN_TARGET, &plug, plug.Size, nullptr, 0, &xfr, &ov);
    bool ok = GetOverlappedResult(bus, &ov, &xfr, TRUE) != 0;
    if (ok) {
        VIGEM_WAIT_DEVICE_READY wr;
        VIGEM_WAIT_DEVICE_READY_INIT(&wr, serial);
        OVERLAPPED ov2{};
        ov2.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        DeviceIoControl(bus, IOCTL_VIGEM_WAIT_DEVICE_READY, &wr, wr.Size, nullptr, 0, &xfr, &ov2);
        GetOverlappedResult(bus, &ov2, &xfr, TRUE);
        CloseHandle(ov2.hEvent);
    }
    CloseHandle(ov.hEvent);
    return ok;
}

bool submitReport(HANDLE bus, ULONG serial, const XUSB_REPORT& rpt) {
    OVERLAPPED ov{};
    ov.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    DWORD xfr = 0;
    XUSB_SUBMIT_REPORT sr;
    XUSB_SUBMIT_REPORT_INIT(&sr, serial);
    sr.Report = rpt;
    DeviceIoControl(bus, IOCTL_XUSB_SUBMIT_REPORT, &sr, sr.Size, nullptr, 0, &xfr, &ov);
    bool ok = GetOverlappedResult(bus, &ov, &xfr, TRUE) != 0;
    CloseHandle(ov.hEvent);
    return ok;
}

// Per-slot synchronous submit helpers. Caller-owned submit struct + per-slot
// persistent event keep the hot path to one memcpy and no CreateEvent/Close
// pair. Sync (GetOverlappedResult bWait=TRUE) is mandatory: fire-and-forget here
// made the dish report "no input reaching the game" with no driver-side error
// (the kernel seems not to reliably signal the user event on sync-success
// completion for ViGEmBus IOCTLs).
static bool issueOverlappedSync(HANDLE bus, DWORD ioctl, void* inBuf, DWORD inSize, HANDLE event) {
    OVERLAPPED ov{};
    ov.hEvent = event;
    DWORD xfr = 0;
    DeviceIoControl(bus, ioctl, inBuf, inSize, nullptr, 0, &xfr, &ov);
    return GetOverlappedResult(bus, &ov, &xfr, TRUE) != 0;
}

bool submitXusbSync(HANDLE bus, ULONG serial, XUSB_SUBMIT_REPORT& xsr, HANDLE event,
                    const void* reportBytes) {
    XUSB_SUBMIT_REPORT_INIT(&xsr, serial);
    static_assert(sizeof(XUSB_REPORT) == 12, "XUSB_REPORT size assumption");
    std::memcpy(&xsr.Report, reportBytes, sizeof(XUSB_REPORT));
    return issueOverlappedSync(bus, IOCTL_XUSB_SUBMIT_REPORT, &xsr, xsr.Size, event);
}

bool submitDs4Sync(HANDLE bus, ULONG serial, DS4_SUBMIT_REPORT& sr, HANDLE event,
                   const DS4_REPORT& rpt) {
    DS4_SUBMIT_REPORT_INIT(&sr, serial);
    sr.Report = rpt;
    return issueOverlappedSync(bus, IOCTL_DS4_SUBMIT_REPORT, &sr, sr.Size, event);
}

bool submitDs4ExSync(HANDLE bus, ULONG serial, DS4_SUBMIT_REPORT_EX& sr, HANDLE event,
                     const DS4_REPORT_EX& rpt) {
    DS4_SUBMIT_REPORT_EX_INIT(&sr, serial);
    sr.Report = rpt;
    // Upstream ViGEmBus has no separate "_EX" submit IOCTL: vigem_target_ds4_update_ex
    // submits the extended report through the SAME IOCTL_DS4_SUBMIT_REPORT and the
    // driver dispatches basic vs extended by the buffer size (sr.Size, = 71 packed).
    OVERLAPPED ov{};
    ov.hEvent = event;
    ResetEvent(event);
    DWORD xfr = 0;
    DeviceIoControl(bus, IOCTL_DS4_SUBMIT_REPORT, &sr, sr.Size, nullptr, 0, &xfr, &ov);
    const BOOL ok = GetOverlappedResult(bus, &ov, &xfr, TRUE);
    // Interpret the completion per ViGEmClient's criterion (see ds4ExSubmitLanded):
    // most non-signalling completions still delivered the report.
    return ds4ExSubmitLanded(ok != 0, ok ? 0u : GetLastError());
}

// Observable: true iff the driver accepted the unplug. PnP teardown is still
// asynchronous (there is no unplug analog of IOCTL_VIGEM_WAIT_DEVICE_READY),
// but a refused IOCTL means the target is in an unknown state and the caller
// must quarantine the serial rather than reuse it.
bool unplugTarget(HANDLE bus, ULONG serial) {
    OVERLAPPED ov{};
    ov.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    DWORD xfr = 0;
    VIGEM_UNPLUG_TARGET up;
    VIGEM_UNPLUG_TARGET_INIT(&up, serial);
    DeviceIoControl(bus, IOCTL_VIGEM_UNPLUG_TARGET, &up, up.Size, nullptr, 0, &xfr, &ov);
    const bool ok = GetOverlappedResult(bus, &ov, &xfr, TRUE) != 0;
    CloseHandle(ov.hEvent);
    return ok;
}

// "Post one buffer, wait until the driver has data." Waits on the IO completion
// event or the cancel event; on cancel, CancelIoEx so the OVERLAPPED can be
// reclaimed safely.
namespace {

template <typename Notification, ULONG IoctlCode>
bool waitNotificationImpl(HANDLE bus, ULONG serial, HANDLE cancel, Notification& out) {
    Notification req;
    if constexpr (std::is_same_v<Notification, XUSB_REQUEST_NOTIFICATION>) {
        XUSB_REQUEST_NOTIFICATION_INIT(&req, serial);
    } else {
        DS4_REQUEST_NOTIFICATION_INIT(&req, serial);
    }

    OVERLAPPED ov{};
    ov.hEvent = CreateEvent(nullptr, TRUE /* manual reset */, FALSE, nullptr);
    if (ov.hEvent == nullptr) return false;

    DWORD xfr = 0;
    BOOL ok = DeviceIoControl(bus, IoctlCode, &req, req.Size, &req, req.Size, &xfr, &ov);
    if (!ok && GetLastError() != ERROR_IO_PENDING) {
        CloseHandle(ov.hEvent);
        return false;
    }

    HANDLE waits[2] = {ov.hEvent, cancel};
    DWORD waitCount = (cancel != nullptr) ? 2 : 1;
    DWORD which = WaitForMultipleObjects(waitCount, waits, FALSE, INFINITE);
    if (which != WAIT_OBJECT_0) {
        // Cancel signalled (or a wait failure) — unwind the pending IOCTL.
        CancelIoEx(bus, &ov);
        // Drain the OVERLAPPED so we can free its event safely.
        GetOverlappedResult(bus, &ov, &xfr, TRUE);
        CloseHandle(ov.hEvent);
        return false;
    }

    if (!GetOverlappedResult(bus, &ov, &xfr, TRUE)) {
        CloseHandle(ov.hEvent);
        return false;
    }
    CloseHandle(ov.hEvent);
    out = req;
    return true;
}

} // namespace

bool waitNextXusbNotification(HANDLE bus, ULONG serial, HANDLE cancel,
                              XUSB_REQUEST_NOTIFICATION& out) {
    return waitNotificationImpl<XUSB_REQUEST_NOTIFICATION, IOCTL_XUSB_REQUEST_NOTIFICATION>(
        bus, serial, cancel, out);
}

bool waitNextDS4Notification(HANDLE bus, ULONG serial, HANDLE cancel,
                             DS4_REQUEST_NOTIFICATION& out) {
    return waitNotificationImpl<DS4_REQUEST_NOTIFICATION, IOCTL_DS4_REQUEST_NOTIFICATION>(
        bus, serial, cancel, out);
}
