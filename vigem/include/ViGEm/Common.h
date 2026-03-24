/*
 * ViGEm Common types - from nefarius/ViGEmClient (MIT License)
 * Copyright (c) 2017-2023 Nefarius Software Solutions e.U. and Contributors
 */
#pragma once

#ifndef _WINDOWS_
#include <windows.h>
#endif

typedef enum _VIGEM_TARGET_TYPE {
    Xbox360Wired = 0,
    DualShock4Wired = 2
} VIGEM_TARGET_TYPE, *PVIGEM_TARGET_TYPE;

typedef enum _XUSB_BUTTON {
    XUSB_GAMEPAD_DPAD_UP        = 0x0001,
    XUSB_GAMEPAD_DPAD_DOWN      = 0x0002,
    XUSB_GAMEPAD_DPAD_LEFT      = 0x0004,
    XUSB_GAMEPAD_DPAD_RIGHT     = 0x0008,
    XUSB_GAMEPAD_START           = 0x0010,
    XUSB_GAMEPAD_BACK            = 0x0020,
    XUSB_GAMEPAD_LEFT_THUMB      = 0x0040,
    XUSB_GAMEPAD_RIGHT_THUMB     = 0x0080,
    XUSB_GAMEPAD_LEFT_SHOULDER   = 0x0100,
    XUSB_GAMEPAD_RIGHT_SHOULDER  = 0x0200,
    XUSB_GAMEPAD_GUIDE           = 0x0400,
    XUSB_GAMEPAD_A               = 0x1000,
    XUSB_GAMEPAD_B               = 0x2000,
    XUSB_GAMEPAD_X               = 0x4000,
    XUSB_GAMEPAD_Y               = 0x8000
} XUSB_BUTTON, *PXUSB_BUTTON;

// Binary-compatible with XINPUT_GAMEPAD
typedef struct _XUSB_REPORT {
    USHORT wButtons;
    BYTE bLeftTrigger;
    BYTE bRightTrigger;
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
} XUSB_REPORT, *PXUSB_REPORT;

VOID FORCEINLINE XUSB_REPORT_INIT(PXUSB_REPORT Report) {
    RtlZeroMemory(Report, sizeof(XUSB_REPORT));
}

