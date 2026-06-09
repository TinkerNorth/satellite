/*
 * ViGEm Bus Driver shared definitions - from nefarius/ViGEmClient (MIT License)
 * Copyright (c) 2016-2023 Nefarius Software Solutions e.U. and Contributors
 * Minimal subset for direct DeviceIoControl communication with ViGEmBus.
 */
#pragma once

#include <windows.h>
#include <initguid.h>
#include "ViGEm/Common.h"

// {96E42B22-F5E9-42F8-B043-ED0F932F014F}
DEFINE_GUID(GUID_DEVINTERFACE_BUSENUM_VIGEM,
    0x96E42B22, 0xF5E9, 0x42F8, 0xB0, 0x43, 0xED, 0x0F, 0x93, 0x2F, 0x01, 0x4F);

#define VIGEM_COMMON_VERSION            0x0001
#define FILE_DEVICE_BUSENUM             FILE_DEVICE_BUS_EXTENDER
#define BUSENUM_IOCTL(_index_)          CTL_CODE(FILE_DEVICE_BUSENUM, _index_, METHOD_BUFFERED, FILE_READ_DATA)
#define BUSENUM_W_IOCTL(_index_)        CTL_CODE(FILE_DEVICE_BUSENUM, _index_, METHOD_BUFFERED, FILE_WRITE_DATA)
#define BUSENUM_RW_IOCTL(_index_)       CTL_CODE(FILE_DEVICE_BUSENUM, _index_, METHOD_BUFFERED, FILE_WRITE_DATA | FILE_READ_DATA)

#define IOCTL_VIGEM_BASE 0x801

#define IOCTL_VIGEM_PLUGIN_TARGET       BUSENUM_W_IOCTL (IOCTL_VIGEM_BASE + 0x000)
#define IOCTL_VIGEM_UNPLUG_TARGET       BUSENUM_W_IOCTL (IOCTL_VIGEM_BASE + 0x001)
#define IOCTL_VIGEM_CHECK_VERSION       BUSENUM_W_IOCTL (IOCTL_VIGEM_BASE + 0x002)
#define IOCTL_VIGEM_WAIT_DEVICE_READY   BUSENUM_W_IOCTL (IOCTL_VIGEM_BASE + 0x003)
#define IOCTL_XUSB_REQUEST_NOTIFICATION BUSENUM_RW_IOCTL(IOCTL_VIGEM_BASE + 0x200)
#define IOCTL_XUSB_SUBMIT_REPORT        BUSENUM_W_IOCTL (IOCTL_VIGEM_BASE + 0x201)
#define IOCTL_DS4_SUBMIT_REPORT         BUSENUM_W_IOCTL (IOCTL_VIGEM_BASE + 0x202)
#define IOCTL_DS4_REQUEST_NOTIFICATION  BUSENUM_RW_IOCTL(IOCTL_VIGEM_BASE + 0x203)
// CORRECTION (verified against nefarius/ViGEmBus and the ViGEmClient source):
// there is NO separate "_EX" submit IOCTL. The extended 63-byte DS4 report
// (gyro/accel/touchpad/battery) is submitted through the SAME
// IOCTL_DS4_SUBMIT_REPORT (0x202) used by the basic report — the driver
// dispatches basic vs extended purely by the input buffer size
// (sizeof(DS4_SUBMIT_REPORT) vs sizeof(DS4_SUBMIT_REPORT_EX)). See
// vigem_target_ds4_update_ex in ViGEmClient: "Same IOCTL, just different size".
//
// The 0x205 code below was a guessed value the driver does not implement, so it
// returned ERROR_NOT_SUPPORTED (50) and motion never reached the host. It is
// kept only for historical reference and is intentionally UNUSED — submitDs4ExSync
// now uses IOCTL_DS4_SUBMIT_REPORT. Do not resurrect the _EX submit codes.
#define IOCTL_XUSB_SUBMIT_REPORT_EX     BUSENUM_W_IOCTL (IOCTL_VIGEM_BASE + 0x204)
#define IOCTL_DS4_SUBMIT_REPORT_EX      BUSENUM_W_IOCTL (IOCTL_VIGEM_BASE + 0x205)

// Plugin target request
typedef struct _VIGEM_PLUGIN_TARGET {
    ULONG Size;
    ULONG SerialNo;
    VIGEM_TARGET_TYPE TargetType;
    USHORT VendorId;
    USHORT ProductId;
} VIGEM_PLUGIN_TARGET, *PVIGEM_PLUGIN_TARGET;

VOID FORCEINLINE VIGEM_PLUGIN_TARGET_INIT(
    PVIGEM_PLUGIN_TARGET PlugIn, ULONG SerialNo, VIGEM_TARGET_TYPE TargetType) {
    RtlZeroMemory(PlugIn, sizeof(VIGEM_PLUGIN_TARGET));
    PlugIn->Size = sizeof(VIGEM_PLUGIN_TARGET);
    PlugIn->SerialNo = SerialNo;
    PlugIn->TargetType = TargetType;
}

// Unplug target request
typedef struct _VIGEM_UNPLUG_TARGET {
    ULONG Size;
    ULONG SerialNo;
} VIGEM_UNPLUG_TARGET, *PVIGEM_UNPLUG_TARGET;

VOID FORCEINLINE VIGEM_UNPLUG_TARGET_INIT(PVIGEM_UNPLUG_TARGET UnPlug, ULONG SerialNo) {
    RtlZeroMemory(UnPlug, sizeof(VIGEM_UNPLUG_TARGET));
    UnPlug->Size = sizeof(VIGEM_UNPLUG_TARGET);
    UnPlug->SerialNo = SerialNo;
}

// Version check
typedef struct _VIGEM_CHECK_VERSION {
    ULONG Size;
    ULONG Version;
} VIGEM_CHECK_VERSION, *PVIGEM_CHECK_VERSION;

VOID FORCEINLINE VIGEM_CHECK_VERSION_INIT(PVIGEM_CHECK_VERSION CV, ULONG Version) {
    RtlZeroMemory(CV, sizeof(VIGEM_CHECK_VERSION));
    CV->Size = sizeof(VIGEM_CHECK_VERSION);
    CV->Version = Version;
}

// Wait device ready
typedef struct _VIGEM_WAIT_DEVICE_READY {
    ULONG Size;
    ULONG SerialNo;
} VIGEM_WAIT_DEVICE_READY, *PVIGEM_WAIT_DEVICE_READY;

VOID FORCEINLINE VIGEM_WAIT_DEVICE_READY_INIT(PVIGEM_WAIT_DEVICE_READY WR, ULONG SerialNo) {
    RtlZeroMemory(WR, sizeof(VIGEM_WAIT_DEVICE_READY));
    WR->Size = sizeof(VIGEM_WAIT_DEVICE_READY);
    WR->SerialNo = SerialNo;
}

// Submit Xbox 360 report
typedef struct _XUSB_SUBMIT_REPORT {
    ULONG Size;
    ULONG SerialNo;
    XUSB_REPORT Report;
} XUSB_SUBMIT_REPORT, *PXUSB_SUBMIT_REPORT;

VOID FORCEINLINE XUSB_SUBMIT_REPORT_INIT(PXUSB_SUBMIT_REPORT Report, ULONG SerialNo) {
    RtlZeroMemory(Report, sizeof(XUSB_SUBMIT_REPORT));
    Report->Size = sizeof(XUSB_SUBMIT_REPORT);
    Report->SerialNo = SerialNo;
}

// Submit DualShock 4 report
typedef struct _DS4_SUBMIT_REPORT {
    ULONG Size;
    ULONG SerialNo;
    DS4_REPORT Report;
} DS4_SUBMIT_REPORT, *PDS4_SUBMIT_REPORT;

VOID FORCEINLINE DS4_SUBMIT_REPORT_INIT(PDS4_SUBMIT_REPORT Report, ULONG SerialNo) {
    RtlZeroMemory(Report, sizeof(DS4_SUBMIT_REPORT));
    Report->Size = sizeof(DS4_SUBMIT_REPORT);
    Report->SerialNo = SerialNo;
    DS4_REPORT_INIT(&Report->Report);
}

// Xbox 360 rumble / LED notification — long-running async request. The driver
// completes the IOCTL whenever a process calls XInputSetState (rumble) or the
// LED slot number changes for the target. The Size/SerialNo are filled by the
// caller; LedNumber/LargeMotor/SmallMotor come back from the driver.
typedef struct _XUSB_REQUEST_NOTIFICATION {
    ULONG Size;
    ULONG SerialNo;
    UCHAR LedNumber;
    UCHAR LargeMotor;
    UCHAR SmallMotor;
} XUSB_REQUEST_NOTIFICATION, *PXUSB_REQUEST_NOTIFICATION;

VOID FORCEINLINE XUSB_REQUEST_NOTIFICATION_INIT(PXUSB_REQUEST_NOTIFICATION Notify, ULONG SerialNo) {
    RtlZeroMemory(Notify, sizeof(XUSB_REQUEST_NOTIFICATION));
    Notify->Size = sizeof(XUSB_REQUEST_NOTIFICATION);
    Notify->SerialNo = SerialNo;
}

// DualShock 4 rumble / lightbar notification — same async pattern. The driver
// fills LargeMotor / SmallMotor / LightbarColor on every output report from
// the host stack.
typedef struct _DS4_LIGHTBAR_COLOR {
    UCHAR Red;
    UCHAR Green;
    UCHAR Blue;
} DS4_LIGHTBAR_COLOR, *PDS4_LIGHTBAR_COLOR;

typedef struct _DS4_REQUEST_NOTIFICATION {
    ULONG Size;
    ULONG SerialNo;
    UCHAR LargeMotor;
    UCHAR SmallMotor;
    DS4_LIGHTBAR_COLOR LightbarColor;
} DS4_REQUEST_NOTIFICATION, *PDS4_REQUEST_NOTIFICATION;

VOID FORCEINLINE DS4_REQUEST_NOTIFICATION_INIT(PDS4_REQUEST_NOTIFICATION Notify, ULONG SerialNo) {
    RtlZeroMemory(Notify, sizeof(DS4_REQUEST_NOTIFICATION));
    Notify->Size = sizeof(DS4_REQUEST_NOTIFICATION);
    Notify->SerialNo = SerialNo;
}

// ── Extended DualShock 4 input report (ViGEmBus >= 1.17) ────────────────────
//
// Backported from nefarius/ViGEmBus's BusShared.h (MIT). The basic DS4_REPORT
// (declared in Common.h) carries only buttons / sticks / triggers. DS4_REPORT_EX
// is the full 63-byte DS4 USB input report (HID report 0x01 with the report-id
// byte stripped) — the *only* ViGEm submit path that carries the gyroscope,
// accelerometer, touchpad and battery fields. The first 9 bytes are layout-
// identical to DS4_REPORT, so an EX report degrades cleanly to a basic report.
//
// VERIFY-ON-BUMP: this struct + IOCTL_DS4_SUBMIT_REPORT_EX are hand-backported.
// When the bundled ViGEmBus driver/headers are refreshed, diff both against
// upstream. A mismatch is safe-by-construction — the driver rejects the IOCTL
// and ViGEmAdapter falls back to the basic DS4_REPORT path — but it silently
// disables Windows virtual-pad IMU until corrected.
#pragma pack(push, 1)
typedef struct _DS4_TOUCH {
    UCHAR bPacketCounter;    // timestamp / packet counter for this touch frame
    UCHAR bIsUpTrackingNum1; // bit7 = finger 1 lifted; bits0..6 = tracking id
    UCHAR bTouchData1[3];    // finger 1: two packed 12-bit coords (x then y)
    UCHAR bIsUpTrackingNum2; // finger 2
    UCHAR bTouchData2[3];
} DS4_TOUCH, *PDS4_TOUCH;

typedef struct _DS4_REPORT_EX {
    union {
        struct {
            UCHAR bThumbLX;
            UCHAR bThumbLY;
            UCHAR bThumbRX;
            UCHAR bThumbRY;
            USHORT wButtons;
            UCHAR bSpecial;
            UCHAR bTriggerL;
            UCHAR bTriggerR;
            USHORT wTimestamp; // free-running, 5.33 µs units
            UCHAR bBatteryLvl;
            SHORT wGyroX; // gyroscope, signed little-endian
            SHORT wGyroY;
            SHORT wGyroZ;
            SHORT wAccelX; // accelerometer, signed little-endian
            SHORT wAccelY;
            SHORT wAccelZ;
            UCHAR _bUnknown1[5];
            UCHAR bBatteryLvlSpecial;
            UCHAR _bUnknown2[2];
            UCHAR bTouchPacketsN; // count of touch frames that follow (0..3)
            DS4_TOUCH sCurrentTouch;
            DS4_TOUCH sPreviousTouch[2];
        } Report;
        UCHAR ReportBuffer[63];
    };
} DS4_REPORT_EX, *PDS4_REPORT_EX;
#pragma pack(pop)

#ifdef __cplusplus
static_assert(sizeof(DS4_REPORT_EX) == 63, "DS4_REPORT_EX must be the 63-byte DS4 report");
#endif

// Submit extended DualShock 4 report. MUST be packed to 1 byte: upstream wraps
// this struct in <pshpack1.h>/<poppack.h>, making it 71 bytes (4 + 4 + 63).
// Default alignment pads it to 72; the driver compares the submitted Size against
// its own (71) and rejects a 72-byte buffer with ERROR_INVALID_USER_BUFFER (1784),
// so motion never lands. (The basic DS4_SUBMIT_REPORT above is intentionally NOT
// packed — that matches upstream too, which is why buttons/sticks always worked.)
#pragma pack(push, 1)
typedef struct _DS4_SUBMIT_REPORT_EX {
    ULONG Size;
    ULONG SerialNo;
    DS4_REPORT_EX Report;
} DS4_SUBMIT_REPORT_EX, *PDS4_SUBMIT_REPORT_EX;
#pragma pack(pop)

#ifdef __cplusplus
static_assert(sizeof(DS4_SUBMIT_REPORT_EX) == 71,
              "DS4_SUBMIT_REPORT_EX must be 71 bytes (packed) to match the ViGEmBus driver");
#endif

VOID FORCEINLINE DS4_SUBMIT_REPORT_EX_INIT(PDS4_SUBMIT_REPORT_EX Report, ULONG SerialNo) {
    RtlZeroMemory(Report, sizeof(DS4_SUBMIT_REPORT_EX));
    Report->Size = sizeof(DS4_SUBMIT_REPORT_EX);
    Report->SerialNo = SerialNo;
}

