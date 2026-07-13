// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "globals.h"

HANDLE openVigemBus();
bool pluginTarget(HANDLE bus, ULONG serial);
bool pluginTargetDS4(HANDLE bus, ULONG serial);

bool submitReport(HANDLE bus, ULONG serial, const XUSB_REPORT& rpt);

// MUST stay synchronous (GetOverlappedResult bWait=TRUE): fire-and-forget was
// tried and the dish saw "no input reaching the game" with no driver error.
bool submitXusbSync(HANDLE bus, ULONG serial, XUSB_SUBMIT_REPORT& xsr, HANDLE event,
                    const void* reportBytes);
bool submitDs4Sync(HANDLE bus, ULONG serial, DS4_SUBMIT_REPORT& sr, HANDLE event,
                   const DS4_REPORT& rpt);
bool submitDs4ExSync(HANDLE bus, ULONG serial, DS4_SUBMIT_REPORT_EX& sr, HANDLE event,
                     const DS4_REPORT_EX& rpt);

// True iff the driver accepted the unplug; false means target state unknown,
// caller must quarantine the serial. PnP teardown stays asynchronous either way.
bool unplugTarget(HANDLE bus, ULONG serial);

// `cancel` (signalled on unplug) aborts the pending IOCTL via CancelIoEx and
// returns false.
bool waitNextXusbNotification(HANDLE bus, ULONG serial, HANDLE cancel,
                              XUSB_REQUEST_NOTIFICATION& out);
bool waitNextDS4Notification(HANDLE bus, ULONG serial, HANDLE cancel,
                             DS4_REQUEST_NOTIFICATION& out);
