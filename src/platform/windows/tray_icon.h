// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "globals.h"

#include <string>

namespace trayicon {

inline const UINT WM_REFRESH = WM_APP + 2;
inline const UINT WM_BALLOON = WM_APP + 3;
inline constexpr UINT_PTR REFRESH_TIMER_ID = 1;
inline constexpr UINT REFRESH_INTERVAL_MS = 2000;

UINT taskbarCreatedMessage();
void add(HWND hwnd, UINT refreshMs = REFRESH_INTERVAL_MS);
void remove();
void requestRefresh();
void showBalloon(std::wstring title, std::wstring body);
bool handleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT& result);

using ShellNotifyFn = decltype(&Shell_NotifyIconW);

#ifdef SATELLITE_BUILD_TESTS
void setShellNotifyForTest(ShellNotifyFn fn);
#endif

} // namespace trayicon
