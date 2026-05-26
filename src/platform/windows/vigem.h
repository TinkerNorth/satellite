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

// Synchronous-wait submit. Used for the legacy code paths and tests that
// want to confirm the IOCTL was accepted before returning.
bool submitReport(HANDLE bus, ULONG serial, const XUSB_REPORT& rpt);

// Per-slot submit helpers. Use a CALLER-OWNED submit struct (`xsr` /
// `ds4` / `ds4Ex`) plus a CALLER-OWNED auto-reset event so a single
// 12-byte memcpy from the wire bytes into the persistent submit
// buffer is the only data copy on the hot path -- no intermediate
// stack-local report struct, no per-call CreateEvent/CloseHandle pair.
//
// IO sequencing: OVERLAPPED is stack-local inside each helper, hEvent
// is set to the caller-owned per-slot event, then GetOverlappedResult
// is called with bWait=TRUE so the driver has finished consuming the
// submit struct by the time we return. Synchronous-wait semantics --
// match the pre-PR behaviour exactly. A previous revision experimented
// with fire-and-forget (return immediately, wait at the start of the
// NEXT call) and the dish reported "no input reaching the game" with
// no driver-side error, so we've reverted to the documented sync path.
// The hot-path wins from the slot-persistent submit buffer and the
// dropped busMtx_ around DeviceIoControl are retained.
//
// Returns true on driver acceptance (GetOverlappedResult succeeded),
// false otherwise.
bool submitXusbSync(HANDLE bus, ULONG serial, XUSB_SUBMIT_REPORT& xsr, HANDLE event,
                    const void* reportBytes);
bool submitDs4Sync(HANDLE bus, ULONG serial, DS4_SUBMIT_REPORT& sr, HANDLE event,
                   const DS4_REPORT& rpt);
bool submitDs4ExSync(HANDLE bus, ULONG serial, DS4_SUBMIT_REPORT_EX& sr, HANDLE event,
                     const DS4_REPORT_EX& rpt);

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
