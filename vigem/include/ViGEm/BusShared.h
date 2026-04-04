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
#define IOCTL_XUSB_SUBMIT_REPORT        BUSENUM_W_IOCTL (IOCTL_VIGEM_BASE + 0x201)
#define IOCTL_DS4_SUBMIT_REPORT         BUSENUM_W_IOCTL (IOCTL_VIGEM_BASE + 0x202)

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

