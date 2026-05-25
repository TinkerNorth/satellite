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

// Fire-and-forget Xbox/DS4/DS4-EX submit. Each call:
//   1. Waits on `event` (auto-reset, initially signalled) -- in steady
//      state this is a non-blocking test because the previous IOCTL's
//      completion already signalled it. Throttles us only if the kernel
//      is genuinely behind, in which case blocking back-pressure is what
//      we want.
//   2. Re-initialises the persistent OVERLAPPED and submit struct that
//      live in the per-serial slot (the caller-owned `xsr` / `ds4` /
//      `ds4Ex` and `ov` references).
//   3. Issues IOCTL_XUSB_SUBMIT_REPORT / IOCTL_DS4_SUBMIT_REPORT[_EX]
//      and returns IMMEDIATELY without GetOverlappedResult. The kernel
//      reads the submit struct asynchronously and signals `event` on
//      completion -- which is what the next call's wait will observe.
//
// Returns true if the IOCTL was either accepted synchronously or queued
// (`ERROR_IO_PENDING`). On a hard synchronous failure the event is
// re-signalled so the next call doesn't deadlock waiting on completion
// that will never arrive.
//
// IMPORTANT: `xsr`/`ds4`/`ds4Ex` and `ov` MUST outlive the IOCTL. The
// adapter places them in the per-serial IoSlot, which is freed only at
// closeBus after a final drain wait on `event`.
bool submitXusbFireAndForget(HANDLE bus, ULONG serial, XUSB_SUBMIT_REPORT& xsr, OVERLAPPED& ov,
                             HANDLE event, const void* reportBytes);
bool submitDs4FireAndForget(HANDLE bus, ULONG serial, DS4_SUBMIT_REPORT& sr, OVERLAPPED& ov,
                            HANDLE event, const DS4_REPORT& rpt);
bool submitDs4ExFireAndForget(HANDLE bus, ULONG serial, DS4_SUBMIT_REPORT_EX& sr, OVERLAPPED& ov,
                              HANDLE event, const DS4_REPORT_EX& rpt);

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
