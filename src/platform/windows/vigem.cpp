// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * vigem.cpp — ViGEmBus driver interaction
 */
#include "vigem.h"

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

// ── Fire-and-forget submit helpers ─────────────────────────────────────────
//
// All three follow the same dance:
//   1. WaitForSingleObject(event, INFINITE) -- auto-reset, signalled by the
//      kernel on the *previous* IOCTL's completion. In steady state this
//      returns immediately; under back-pressure it blocks until the kernel
//      catches up (which is the correct flow-control behaviour).
//   2. Re-initialise the caller-owned OVERLAPPED + submit struct in place
//      (they must outlive the IOCTL, which is the caller's contract).
//   3. Issue the IOCTL and return WITHOUT waiting. The kernel writes the
//      completion to OVERLAPPED and signals `event` asynchronously.
//
// On a hard synchronous failure (closed handle, driver rejected the IOCTL
// outright) we re-signal `event` so the next call doesn't deadlock waiting
// for completion that will never arrive.

static bool issueFireAndForget(HANDLE bus, DWORD ioctl, void* inBuf, DWORD inSize, OVERLAPPED& ov,
                               HANDLE event) {
    WaitForSingleObject(event, INFINITE);
    ZeroMemory(&ov, sizeof(ov));
    ov.hEvent = event;
    DWORD xfr = 0;
    BOOL ok = DeviceIoControl(bus, ioctl, inBuf, inSize, nullptr, 0, &xfr, &ov);
    if (!ok && GetLastError() != ERROR_IO_PENDING) {
        SetEvent(event);
        return false;
    }
    return true;
}

bool submitReportFast(HANDLE bus, ULONG serial, const XUSB_REPORT& rpt, HANDLE event) {
    // Legacy synchronous helper kept for tests; the hot path uses the
    // fire-and-forget variant below. Builds + waits in one go.
    OVERLAPPED ov{};
    ov.hEvent = event;
    DWORD xfr = 0;
    XUSB_SUBMIT_REPORT sr;
    XUSB_SUBMIT_REPORT_INIT(&sr, serial);
    sr.Report = rpt;
    DeviceIoControl(bus, IOCTL_XUSB_SUBMIT_REPORT, &sr, sr.Size, nullptr, 0, &xfr, &ov);
    return GetOverlappedResult(bus, &ov, &xfr, TRUE) != 0;
}

bool submitXusbFireAndForget(HANDLE bus, ULONG serial, XUSB_SUBMIT_REPORT& xsr, OVERLAPPED& ov,
                             HANDLE event, const void* reportBytes) {
    // Re-init the persistent submit struct. Size/Type bytes don't change
    // across calls but the macro is the canonical way to set them and
    // costs nothing in steady state.
    XUSB_SUBMIT_REPORT_INIT(&xsr, serial);
    // The 12-byte XUSB_REPORT is layout-identical to GamepadReport, so the
    // hot path memcpys plaintext bytes straight in -- one copy from wire
    // to kernel buffer, no intermediate.
    static_assert(sizeof(XUSB_REPORT) == 12, "XUSB_REPORT size assumption");
    std::memcpy(&xsr.Report, reportBytes, sizeof(XUSB_REPORT));
    return issueFireAndForget(bus, IOCTL_XUSB_SUBMIT_REPORT, &xsr, xsr.Size, ov, event);
}

bool submitDs4FireAndForget(HANDLE bus, ULONG serial, DS4_SUBMIT_REPORT& sr, OVERLAPPED& ov,
                            HANDLE event, const DS4_REPORT& rpt) {
    DS4_SUBMIT_REPORT_INIT(&sr, serial);
    sr.Report = rpt;
    return issueFireAndForget(bus, IOCTL_DS4_SUBMIT_REPORT, &sr, sr.Size, ov, event);
}

bool submitDs4ExFireAndForget(HANDLE bus, ULONG serial, DS4_SUBMIT_REPORT_EX& sr, OVERLAPPED& ov,
                              HANDLE event, const DS4_REPORT_EX& rpt) {
    DS4_SUBMIT_REPORT_EX_INIT(&sr, serial);
    sr.Report = rpt;
    return issueFireAndForget(bus, IOCTL_DS4_SUBMIT_REPORT_EX, &sr, sr.Size, ov, event);
}

void unplugTarget(HANDLE bus, ULONG serial) {
    OVERLAPPED ov{};
    ov.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    DWORD xfr = 0;
    VIGEM_UNPLUG_TARGET up;
    VIGEM_UNPLUG_TARGET_INIT(&up, serial);
    DeviceIoControl(bus, IOCTL_VIGEM_UNPLUG_TARGET, &up, up.Size, nullptr, 0, &xfr, &ov);
    GetOverlappedResult(bus, &ov, &xfr, TRUE);
    CloseHandle(ov.hEvent);
}

// ── Notification waits ─────────────────────────────────────────────────────
// The IOCTL is "post one buffer, wait until the driver has data". We wait on
// either the IO completion event or the caller-provided cancel event; if the
// cancel event wins we issue CancelIoEx so the OVERLAPPED structure can be
// safely reclaimed.

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
