// SPDX-License-Identifier: LGPL-3.0-or-later
// Standalone probe: drive the REAL ViGEmBus driver on this machine, plug a
// virtual DS4, submit an extended (IMU) report with a known gyro value through
// several (IOCTL, size) variants, then read the virtual pad's HID input report
// back to prove which variant actually lands the motion bytes.
//
//   g++ -std=c++17 -O0 -I vigem/include tools/vigem_probe.cpp
//       -o vigem_probe.exe -lsetupapi -lhid
#define INITGUID
#include <windows.h>

#include <hidsdi.h>
#include <setupapi.h>

#include <cstdio>
#include <cstring>

#include "ViGEm/BusShared.h"

static const char* errName(DWORD e) {
    switch (e) {
    case 0: return "OK";
    case 1: return "INVALID_FUNCTION";
    case 50: return "NOT_SUPPORTED";
    case 87: return "INVALID_PARAMETER";
    case 122: return "INSUFFICIENT_BUFFER";
    case 259: return "NO_MORE_ITEMS/STILL_ACTIVE";
    case 997: return "IO_PENDING";
    case 1784: return "INVALID_USER_BUFFER";
    default: return "?";
    }
}

static HANDLE openBus() {
    HDEVINFO di = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_BUSENUM_VIGEM, nullptr, nullptr,
                                       DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (di == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    SP_DEVICE_INTERFACE_DATA did{};
    did.cbSize = sizeof(did);
    HANDLE h = INVALID_HANDLE_VALUE;
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(di, nullptr, &GUID_DEVINTERFACE_BUSENUM_VIGEM, i,
                                                  &did);
         ++i) {
        DWORD req = 0;
        SetupDiGetDeviceInterfaceDetailW(di, &did, nullptr, 0, &req, nullptr);
        auto* detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)malloc(req);
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (SetupDiGetDeviceInterfaceDetailW(di, &did, detail, req, nullptr, nullptr)) {
            h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
        }
        free(detail);
        if (h != INVALID_HANDLE_VALUE) break;
    }
    SetupDiDestroyDeviceInfoList(di);
    return h;
}

static bool ioctlWait(HANDLE bus, DWORD code, void* in, DWORD inSize, void* out, DWORD outSize) {
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    DWORD xfr = 0;
    DeviceIoControl(bus, code, in, inSize, out, outSize, &xfr, &ov);
    if (GetLastError() == ERROR_IO_PENDING) GetOverlappedResult(bus, &ov, &xfr, TRUE);
    CloseHandle(ov.hEvent);
    return true;
}

static bool plugDS4(HANDLE bus, ULONG serial) {
    VIGEM_PLUGIN_TARGET pt{};
    VIGEM_PLUGIN_TARGET_INIT(&pt, serial, DualShock4Wired);
    pt.VendorId = 0x054C;
    pt.ProductId = 0x05C4;
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    DWORD xfr = 0;
    BOOL ok = DeviceIoControl(bus, IOCTL_VIGEM_PLUGIN_TARGET, &pt, pt.Size, nullptr, 0, &xfr, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) ok = GetOverlappedResult(bus, &ov, &xfr, TRUE);
    CloseHandle(ov.hEvent);
    VIGEM_WAIT_DEVICE_READY wr{};
    wr.Size = sizeof(wr);
    wr.SerialNo = serial;
    ioctlWait(bus, IOCTL_VIGEM_WAIT_DEVICE_READY, &wr, wr.Size, nullptr, 0);
    return ok;
}

// event mode: 0 = fresh event each call; 1 = persistent initially-signaled
// auto-reset event + ResetEvent (the satellite's current code); 2 = same
// persistent event but NO ResetEvent + unconditional GetOverlappedResult (the
// satellite's ORIGINAL code).
static HANDLE g_persistent = nullptr;
static void submitEx(HANDLE bus, ULONG serial, DWORD ioctl, DWORD size, const DS4_REPORT_EX& rpt,
                     int eventMode) {
    DS4_SUBMIT_REPORT_EX sr{};
    DS4_SUBMIT_REPORT_EX_INIT(&sr, serial);
    sr.Report = rpt;
    OVERLAPPED ov{};
    HANDLE ev;
    if (eventMode == 0) {
        ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    } else {
        if (!g_persistent) g_persistent = CreateEventW(nullptr, FALSE, TRUE, nullptr); // initial SIGNALED
        ev = g_persistent;
    }
    ov.hEvent = ev;
    DWORD xfr = 0;
    SetLastError(0);
    BOOL dic, gor;
    DWORD gle1, gle2;
    if (eventMode == 1) {
        ResetEvent(ev);
        dic = DeviceIoControl(bus, ioctl, &sr, size, nullptr, 0, &xfr, &ov);
        gle1 = GetLastError();
        if (dic) {
            gor = TRUE;
            gle2 = 0;
        } else if (gle1 != ERROR_IO_PENDING) {
            gor = FALSE;
            gle2 = gle1;
        } else {
            gor = GetOverlappedResult(bus, &ov, &xfr, TRUE);
            gle2 = GetLastError();
        }
    } else {
        dic = DeviceIoControl(bus, ioctl, &sr, size, nullptr, 0, &xfr, &ov);
        gle1 = GetLastError();
        gor = GetOverlappedResult(bus, &ov, &xfr, TRUE);
        gle2 = GetLastError();
    }
    if (eventMode == 0) CloseHandle(ev);
    printf("        submit(evmode=%d): DeviceIoControl=%d gle=%lu(%s)  GetOverlappedResult=%d "
           "gle=%lu(%s)\n",
           eventMode, dic, gle1, errName(gle1), gor, gle2, errName(gle2));
}

// Open the virtual DS4's HID interface (Sony VID 0x054C / PID 0x05C4).
static HANDLE openVirtualDs4Hid(int& inputLen) {
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO di =
        SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (di == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    SP_DEVICE_INTERFACE_DATA did{};
    did.cbSize = sizeof(did);
    HANDLE result = INVALID_HANDLE_VALUE;
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(di, nullptr, &hidGuid, i, &did); ++i) {
        DWORD req = 0;
        SetupDiGetDeviceInterfaceDetailW(di, &did, nullptr, 0, &req, nullptr);
        auto* detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)malloc(req);
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(di, &did, detail, req, nullptr, nullptr)) {
            free(detail);
            continue;
        }
        HANDLE h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED, nullptr);
        free(detail);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES attr{};
        attr.Size = sizeof(attr);
        if (HidD_GetAttributes(h, &attr) && attr.VendorID == 0x054C && attr.ProductID == 0x05C4) {
            PHIDP_PREPARSED_DATA pp = nullptr;
            if (HidD_GetPreparsedData(h, &pp)) {
                HIDP_CAPS caps{};
                HidP_GetCaps(pp, &caps);
                inputLen = caps.InputReportByteLength;
                HidD_FreePreparsedData(pp);
            }
            result = h;
            break;
        }
        CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(di);
    return result;
}

// Read one input report; return gyroX (bytes 13..14, after the 1-byte report id).
static bool readGyroX(HANDLE hid, int inputLen, unsigned& gyroX) {
    BYTE buf[128] = {0};
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    DWORD rd = 0;
    ReadFile(hid, buf, inputLen, &rd, &ov);
    bool ok = false;
    if (WaitForSingleObject(ov.hEvent, 400) == WAIT_OBJECT_0 &&
        GetOverlappedResult(hid, &ov, &rd, TRUE) && rd >= 15) {
        gyroX = (unsigned)buf[13] | ((unsigned)buf[14] << 8);
        ok = true;
    } else {
        CancelIo(hid);
    }
    CloseHandle(ov.hEvent);
    return ok;
}

struct Variant {
    const char* name;
    DWORD ioctl;
    DWORD size;
    USHORT gyroVal;
};

int main() {
    printf("sizeof(DS4_SUBMIT_REPORT_EX)=%zu  sizeof(DS4_REPORT_EX)=%zu\n",
           sizeof(DS4_SUBMIT_REPORT_EX), sizeof(DS4_REPORT_EX));

    HANDLE bus = openBus();
    if (bus == INVALID_HANDLE_VALUE) {
        printf("FAIL: could not open ViGEmBus (GLE=%lu). Is the driver installed?\n",
               GetLastError());
        return 1;
    }
    printf("bus opened. plugging virtual DS4 (serial 1)...\n");
    plugDS4(bus, 1);
    Sleep(1800); // let the HID device enumerate

    int inputLen = 64;
    HANDLE hid = openVirtualDs4Hid(inputLen);
    printf("virtual DS4 HID: %s (InputReportByteLength=%d)\n",
           hid == INVALID_HANDLE_VALUE ? "NOT FOUND" : "opened", inputLen);

    const DWORD EX = IOCTL_DS4_SUBMIT_REPORT;       // 0x202 (correct per upstream)
    const DWORD EX_WRONG = IOCTL_DS4_SUBMIT_REPORT_EX; // 0x205 (the old guess)
    const DWORD sz71 = (DWORD)sizeof(DS4_SUBMIT_REPORT_EX);

    struct V2 {
        const char* name;
        DWORD ioctl;
        DWORD size;
        int eventMode;
        USHORT gyroVal;
    };
    V2 variants[] = {
        {"A: 0x202, size71, FRESH event", EX, sz71, 0, 0x1111},
        {"E: 0x202, size71, PERSISTENT initial-signaled + ResetEvent (current code)", EX, sz71, 1,
         0x5555},
        {"F: 0x202, size71, PERSISTENT initial-signaled, NO reset (original code)", EX, sz71, 2,
         0x6666},
        {"B: 0x202, size72, FRESH event", EX, 72, 0, 0x2222},
        {"C: 0x205 (old _EX code), FRESH event", EX_WRONG, sz71, 0, 0x3333},
    };

    for (auto& v : variants) {
        printf("\n--- %s  (gyroX=0x%04X) ---\n", v.name, v.gyroVal);
        DS4_REPORT_EX rpt{};
        rpt.Report.bThumbLX = 0x80;
        rpt.Report.bThumbLY = 0x80;
        rpt.Report.bThumbRX = 0x80;
        rpt.Report.bThumbRY = 0x80;
        rpt.Report.wButtons = (USHORT)DS4_BUTTON_DPAD_NONE;
        rpt.Report.wGyroX = (SHORT)v.gyroVal;
        rpt.Report.wAccelZ = 0x7EEE;

        bool landed = false;
        unsigned got = 0;
        for (int i = 0; i < 8 && !landed; ++i) {
            submitEx(bus, 1, v.ioctl, v.size, rpt, v.eventMode);
            if (hid != INVALID_HANDLE_VALUE) {
                unsigned g = 0;
                if (readGyroX(hid, inputLen, g)) {
                    got = g;
                    if (g == v.gyroVal) landed = true;
                }
            }
            Sleep(15);
        }
        if (hid != INVALID_HANDLE_VALUE)
            printf("        HID readback gyroX=0x%04X  => motion %s\n", got,
                   landed ? "LANDS  ✓" : "does NOT land");
    }

    if (hid != INVALID_HANDLE_VALUE) CloseHandle(hid);
    {
        VIGEM_UNPLUG_TARGET up{};
        VIGEM_UNPLUG_TARGET_INIT(&up, 1);
        ioctlWait(bus, IOCTL_VIGEM_UNPLUG_TARGET, &up, up.Size, nullptr, 0);
    }
    CloseHandle(bus);
    return 0;
}
