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

// ── DualShock 4 ─────────────────────────────────────────────────────────────

typedef enum _DS4_BUTTONS {
    DS4_BUTTON_THUMB_RIGHT      = 1 << 15,
    DS4_BUTTON_THUMB_LEFT       = 1 << 14,
    DS4_BUTTON_OPTIONS          = 1 << 13,
    DS4_BUTTON_SHARE            = 1 << 12,
    DS4_BUTTON_TRIGGER_RIGHT    = 1 << 11,
    DS4_BUTTON_TRIGGER_LEFT     = 1 << 10,
    DS4_BUTTON_SHOULDER_RIGHT   = 1 << 9,
    DS4_BUTTON_SHOULDER_LEFT    = 1 << 8,
    DS4_BUTTON_TRIANGLE         = 1 << 7,
    DS4_BUTTON_CIRCLE           = 1 << 6,
    DS4_BUTTON_CROSS            = 1 << 5,
    DS4_BUTTON_SQUARE           = 1 << 4
} DS4_BUTTONS, *PDS4_BUTTONS;

typedef enum _DS4_DPAD_DIRECTIONS {
    DS4_BUTTON_DPAD_NONE        = 0x8,
    DS4_BUTTON_DPAD_NORTHWEST   = 0x7,
    DS4_BUTTON_DPAD_WEST        = 0x6,
    DS4_BUTTON_DPAD_SOUTHWEST   = 0x5,
    DS4_BUTTON_DPAD_SOUTH       = 0x4,
    DS4_BUTTON_DPAD_SOUTHEAST   = 0x3,
    DS4_BUTTON_DPAD_EAST        = 0x2,
    DS4_BUTTON_DPAD_NORTHEAST   = 0x1,
    DS4_BUTTON_DPAD_NORTH       = 0x0
} DS4_DPAD_DIRECTIONS, *PDS4_DPAD_DIRECTIONS;

typedef struct _DS4_REPORT {
    BYTE bThumbLX;
    BYTE bThumbLY;
    BYTE bThumbRX;
    BYTE bThumbRY;
    USHORT wButtons;
    BYTE bSpecial;
    BYTE bTriggerL;
    BYTE bTriggerR;
} DS4_REPORT, *PDS4_REPORT;

VOID FORCEINLINE DS4_SET_DPAD(PDS4_REPORT Report, DS4_DPAD_DIRECTIONS Dpad) {
    Report->wButtons &= ~0xF;
    Report->wButtons |= (USHORT)Dpad;
}

VOID FORCEINLINE DS4_REPORT_INIT(PDS4_REPORT Report) {
    RtlZeroMemory(Report, sizeof(DS4_REPORT));
    Report->bThumbLX = 0x80;
    Report->bThumbLY = 0x80;
    Report->bThumbRX = 0x80;
    Report->bThumbRY = 0x80;
    DS4_SET_DPAD(Report, DS4_BUTTON_DPAD_NONE);
}

