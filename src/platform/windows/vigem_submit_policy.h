// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include <windows.h>

// Whether a DS4 extended report submit (IOCTL_DS4_SUBMIT_REPORT with a
// DS4_SUBMIT_REPORT_EX buffer) actually reached the virtual pad, given the
// GetOverlappedResult outcome on the submit's OVERLAPPED.
//
// A non-signalling completion is the norm for this IOCTL, so GetOverlappedResult
// frequently "fails" with a benign status (e.g. 259 / ERROR_NO_MORE_ITEMS /
// STILL_ACTIVE) even though the driver applied the report. Only two outcomes mean
// the report did NOT land:
//   ERROR_ACCESS_DENIED: target unplugged underneath us.
//   wrong-buffer-size reject: ERROR_INVALID_PARAMETER pre-1.17, or
//     ERROR_INVALID_USER_BUFFER on 1.21, i.e. the EX shape isn't accepted.
inline bool ds4ExSubmitLanded(bool overlappedOk, DWORD lastError) {
    if (overlappedOk) return true;
    return !(lastError == ERROR_ACCESS_DENIED || lastError == ERROR_INVALID_PARAMETER ||
             lastError == ERROR_INVALID_USER_BUFFER);
}
