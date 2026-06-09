// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include <windows.h>

// Whether a DS4 *extended* report submit (IOCTL_DS4_SUBMIT_REPORT with a
// DS4_SUBMIT_REPORT_EX buffer) actually reached the virtual pad, given the
// outcome of GetOverlappedResult on the submit's OVERLAPPED.
//
// Matches ViGEmClient's vigem_target_ds4_update_ex: a non-signalling completion
// is the norm for this IOCTL, so GetOverlappedResult frequently "fails" with a
// benign status (e.g. 259 / ERROR_NO_MORE_ITEMS / STILL_ACTIVE) even though the
// driver applied the report. Only two outcomes mean the report did NOT land:
//   - ERROR_ACCESS_DENIED      — the target was unplugged underneath us.
//   - a wrong-buffer-size reject — ERROR_INVALID_PARAMETER on pre-1.17 drivers,
//                                  ERROR_INVALID_USER_BUFFER on 1.21 — i.e. the
//                                  EX report shape isn't accepted (no IMU sink).
//
// Verified against the real driver by reading the virtual pad's HID input report
// back in tools/vigem_probe.cpp (a 259 completion still delivered the gyro).
inline bool ds4ExSubmitLanded(bool overlappedOk, DWORD lastError) {
    if (overlappedOk) return true;
    return !(lastError == ERROR_ACCESS_DENIED || lastError == ERROR_INVALID_PARAMETER ||
             lastError == ERROR_INVALID_USER_BUFFER);
}
