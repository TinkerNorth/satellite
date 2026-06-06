// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "globals.h"

HANDLE openVigemBus();
bool pluginTarget(HANDLE bus, ULONG serial);
bool pluginTargetDS4(HANDLE bus, ULONG serial);

// Synchronous-wait submit, for legacy paths and tests that confirm acceptance.
bool submitReport(HANDLE bus, ULONG serial, const XUSB_REPORT& rpt);

// Per-slot submit helpers using a caller-owned submit struct + auto-reset event
// so the hot path is one 12-byte memcpy with no per-call CreateEvent/Close pair.
// MUST stay synchronous (GetOverlappedResult bWait=TRUE): fire-and-forget was
// tried and the dish saw "no input reaching the game" with no driver error.
// Returns true on driver acceptance.
bool submitXusbSync(HANDLE bus, ULONG serial, XUSB_SUBMIT_REPORT& xsr, HANDLE event,
                    const void* reportBytes);
bool submitDs4Sync(HANDLE bus, ULONG serial, DS4_SUBMIT_REPORT& sr, HANDLE event,
                   const DS4_REPORT& rpt);
bool submitDs4ExSync(HANDLE bus, ULONG serial, DS4_SUBMIT_REPORT_EX& sr, HANDLE event,
                     const DS4_REPORT_EX& rpt);

// EXPERIMENTAL fire-and-forget, take 2: waits for the previous IOCTL FIRST, then
// mutates the buffer (take 1 had this backwards -> dish saw no input). Caller
// supplies a per-slot PERSISTENT OVERLAPPED the kernel writes to after we return.
bool submitXusbFireAndForget(HANDLE bus, ULONG serial, XUSB_SUBMIT_REPORT& xsr, OVERLAPPED& ov,
                             HANDLE event, const void* reportBytes);
bool submitDs4FireAndForget(HANDLE bus, ULONG serial, DS4_SUBMIT_REPORT& sr, OVERLAPPED& ov,
                            HANDLE event, const DS4_REPORT& rpt);
bool submitDs4ExFireAndForget(HANDLE bus, ULONG serial, DS4_SUBMIT_REPORT_EX& sr, OVERLAPPED& ov,
                              HANDLE event, const DS4_REPORT_EX& rpt);

void unplugTarget(HANDLE bus, ULONG serial);

// Block until one rumble/LED notification for `serial`. `cancel` (signalled on
// unplug) aborts the pending IOCTL via CancelIoEx and returns false. True on a
// normal completion with `out` populated.
bool waitNextXusbNotification(HANDLE bus, ULONG serial, HANDLE cancel,
                              XUSB_REQUEST_NOTIFICATION& out);
bool waitNextDS4Notification(HANDLE bus, ULONG serial, HANDLE cancel,
                             DS4_REQUEST_NOTIFICATION& out);
