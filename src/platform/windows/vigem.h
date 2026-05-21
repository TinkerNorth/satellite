// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Satellite contributors.

/*
 * vigem.h — ViGEmBus driver interaction
 */
#pragma once
#include "globals.h"

HANDLE openVigemBus();
bool pluginTarget(HANDLE bus, ULONG serial);
bool pluginTargetDS4(HANDLE bus, ULONG serial);
bool submitReport(HANDLE bus, ULONG serial, const XUSB_REPORT& rpt);
bool submitReportFast(HANDLE bus, ULONG serial, const XUSB_REPORT& rpt, HANDLE event);
bool submitReportDS4Fast(HANDLE bus, ULONG serial, const DS4_REPORT& rpt, HANDLE event);
// Extended DS4 submit — carries the full 63-byte report (gyro/accel/touchpad/
// battery). Needs ViGEmBus >= 1.17; returns false if the driver rejects
// IOCTL_DS4_SUBMIT_REPORT_EX, which the adapter treats as "fall back to basic".
bool submitReportDS4ExFast(HANDLE bus, ULONG serial, const DS4_REPORT_EX& rpt, HANDLE event);
void unplugTarget(HANDLE bus, ULONG serial);

// Block until the driver completes one rumble/LED notification for `serial`.
// `cancel` is signalled by the adapter to wake the thread cleanly on unplug;
// when fired, the function aborts the pending IOCTL (CancelIoEx) and returns
// false. Returns true on a normal completion with the fields populated; false
// on cancel or driver error.
bool waitNextXusbNotification(HANDLE bus, ULONG serial, HANDLE cancel,
                              XUSB_REQUEST_NOTIFICATION& out);
bool waitNextDS4Notification(HANDLE bus, ULONG serial, HANDLE cancel,
                             DS4_REQUEST_NOTIFICATION& out);
